#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "rc.h"
#include "rc_priv.h"

/* Bytes one rc_update() takes off the UART at most, so a line that never
 * goes quiet cannot hold the caller: 24 ms of CRSF at 420000 baud. */
#define RC_READ_MAX	1024

static bool rc_bound(const struct rc_dev *rc)
{
	return rc && rc->ops && rc->ops->parse && rc->input;
}

void rc_dev_init(struct rc_dev *rc, const char *name,
		 const struct rc_ops *ops, void *priv, struct rc_input *input,
		 uint8_t channel_count, uint16_t raw_min, uint16_t raw_mid,
		 uint16_t raw_max, uint32_t timeout_ms)
{
	memset(rc, 0, sizeof(*rc));
	rc->name = name;
	rc->ops = ops;
	rc->priv = priv;
	rc->input = input;
	rc->channel_count = channel_count;
	rc->raw_min = raw_min;
	rc->raw_mid = raw_mid;
	rc->raw_max = raw_max;
	rc->timeout_ms = timeout_ms ? timeout_ms : RC_DEFAULT_TIMEOUT_MS;
}

/* ------------------------------------------------------------- ring -- */

size_t rc_ring_put(struct rc_ring *ring, const uint8_t *data, size_t len)
{
	uint32_t head = ring->head;
	uint32_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
	uint32_t room = RC_RING_LEN - (head - tail);
	size_t n = len < room ? len : room;

	for (size_t i = 0; i < n; i++)
		ring->buf[(head + i) & (RC_RING_LEN - 1)] = data[i];
	__atomic_store_n(&ring->head, head + (uint32_t)n, __ATOMIC_RELEASE);
	if (n < len)
		__atomic_store_n(&ring->dropped,
				 ring->dropped + (uint32_t)(len - n),
				 __ATOMIC_RELAXED);
	return n;
}

/* The slot first, then the count that announces it. */
void rc_ring_mark_idle(struct rc_ring *ring)
{
	uint32_t count = ring->idle_count;

	__atomic_store_n(&ring->idle_at[count % RC_IDLE_MARKS], ring->head,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&ring->idle_count, count + 1, __ATOMIC_RELEASE);
}

/* Parse the next len bytes of the ring, which the caller knows are there. */
static void rc_ring_parse(struct rc_dev *rc, struct rc_ring *ring,
			  uint32_t len)
{
	uint8_t chunk[64];
	uint32_t tail = ring->tail;

	while (len) {
		uint32_t n = len < sizeof(chunk) ? len : sizeof(chunk);

		for (uint32_t i = 0; i < n; i++)
			chunk[i] = ring->buf[(tail + i) & (RC_RING_LEN - 1)];
		tail += n;
		__atomic_store_n(&ring->tail, tail, __ATOMIC_RELEASE);
		rc->ops->parse(rc, chunk, n);
		len -= n;
	}
}

/*
 * Everything fed so far, with each idle mark where it fell.
 *
 * Why this is safe with one writer and one reader and no lock:
 *
 *   - head is loaded before idle_count, both with acquire. The writer
 *     publishes the bytes before a mark (head, release) before it
 *     publishes the mark (idle_count, release). So every mark this call
 *     sees is at a head the writer had published by then -- it may be
 *     past the head loaded here, if bytes came in between; such a mark
 *     waits for the next call -- and a mark it does not see was made at
 *     or after that head, so it can never land behind the tail later.
 *   - The slot is loaded after idle_count's acquire, so it holds that
 *     mark's position or, if the writer has since gone RC_IDLE_MARKS
 *     marks further and reused the slot, a later mark's. A later mark is
 *     still a place where the line really was idle: the frames it skips
 *     run together and are thrown away, never misread. A mark that falls
 *     behind the tail that way is passed over.
 *   - More than RC_IDLE_MARKS marks since the last call: the oldest are
 *     gone, and delivery starts at the oldest still kept.
 */
static void rc_update_fed(struct rc_dev *rc, struct rc_input *input)
{
	struct rc_ring *ring = &input->ring;
	uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
	uint32_t count = __atomic_load_n(&ring->idle_count, __ATOMIC_ACQUIRE);
	uint32_t tail = ring->tail;

	if (count - input->idle_seen > RC_IDLE_MARKS)
		input->idle_seen = count - RC_IDLE_MARKS;
	while (input->idle_seen != count) {
		uint32_t mark = __atomic_load_n(
			&ring->idle_at[input->idle_seen % RC_IDLE_MARKS],
			__ATOMIC_RELAXED);

		if ((int32_t)(mark - tail) < 0) {
			input->idle_seen++;	/* behind the tail: passed */
			continue;
		}
		if (mark - tail > head - tail)
			break;			/* past head: next call */
		input->idle_seen++;
		rc_ring_parse(rc, ring, mark - tail);
		if (rc->ops->idle)
			rc->ops->idle(rc);
		tail = mark;
	}
	rc_ring_parse(rc, ring, head - tail);
}

#ifdef RC_HAVE_BUS
/* Whatever the UART already holds, in chunks, no waiting. */
static int rc_update_read(struct rc_dev *rc, struct rc_input *input)
{
	uint8_t chunk[64];

	for (size_t total = 0; total < RC_READ_MAX;) {
		int n = rc_input_read(input, chunk, sizeof(chunk));

		if (n <= 0)
			return n;
		rc->ops->parse(rc, chunk, (size_t)n);
		total += (size_t)n;
	}
	return 0;
}
#endif

int rc_update(struct rc_dev *rc, uint32_t now_ms)
{
	int ret = 0;

	if (!rc_bound(rc))
		return -EINVAL;
	if (rc->input->fed)
		rc_update_fed(rc, rc->input);
#ifdef RC_HAVE_BUS
	else if (rc->input->bus.ops)
		ret = rc_update_read(rc, rc->input);
#endif
	else
		return -EINVAL;

	if (rc->new_frame) {
		rc->new_frame = false;
		rc->seen = true;
		rc->expired = false;
		rc->frame_ms = now_ms;
		return 0;
	}
	if (rc->seen && now_ms - rc->frame_ms > rc->timeout_ms)
		rc->expired = true;
	return ret ? ret : -EAGAIN;
}

/* -------------------------------------------------------- the frame -- */

static int rc_check_read(const struct rc_dev *rc, const void *out,
			 size_t count)
{
	if (!rc_bound(rc) || !out || count > rc->channel_count)
		return -EINVAL;
	if (!rc->seen)
		return -ENODATA;
	return 0;
}

static float rc_normalise(const struct rc_dev *rc, uint16_t raw)
{
	int32_t offset = (int32_t)raw - rc->raw_mid;
	int32_t half = offset >= 0 ? rc->raw_max - rc->raw_mid
				   : rc->raw_mid - rc->raw_min;

	return (float)offset / (float)half;
}

int rc_get_channels(const struct rc_dev *rc, float *channels, size_t count)
{
	int ret = rc_check_read(rc, channels, count);

	if (ret)
		return ret;
	for (size_t i = 0; i < count; i++)
		channels[i] = rc_normalise(rc, rc->raw[i]);
	return 0;
}

int rc_get_raw(const struct rc_dev *rc, uint16_t *raw, size_t count)
{
	int ret = rc_check_read(rc, raw, count);

	if (ret)
		return ret;
	memcpy(raw, rc->raw, count * sizeof(raw[0]));
	return 0;
}

int rc_get_link(const struct rc_dev *rc, uint32_t now_ms,
		struct rc_link *link)
{
	if (!rc_bound(rc) || !link)
		return -EINVAL;

	memset(link, 0, sizeof(*link));
	link->frames = rc->frames;
	link->errors = rc->errors;
	link->dropped = __atomic_load_n(&rc->input->ring.dropped,
					__ATOMIC_RELAXED);
	if (!rc->seen) {
		link->state = RC_LINK_NO_FRAME;
		link->age_ms = UINT32_MAX;
		return 0;
	}
	link->age_ms = now_ms - rc->frame_ms;
	if (rc->expired || link->age_ms > rc->timeout_ms)
		link->state = RC_LINK_TIMEOUT;
	else if (rc->failsafe)
		link->state = RC_LINK_FAILSAFE;
	else
		link->state = RC_LINK_UP;
	return 0;
}

/* ------------------------------------------------------------- feed -- */

size_t rc_rx_feed(struct rc_dev *rc, const uint8_t *data, size_t len)
{
	if (!rc || !rc->input || !rc->input->fed || !data)
		return 0;
	return rc_ring_put(&rc->input->ring, data, len);
}

void rc_rx_idle(struct rc_dev *rc)
{
	if (!rc || !rc->input || !rc->input->fed)
		return;
	rc_ring_mark_idle(&rc->input->ring);
}
