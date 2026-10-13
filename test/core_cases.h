/*
 * The core's cases -- rc_update() on both byte paths, the ring and its idle
 * mark, normalising, the link state, the log line -- shared by
 * esp/test_core.c and stm/test_core.c. Each includes this after providing:
 *
 *   LOG_PREFIX             what the platform's log puts before "rc: "
 *   stub_log_last          the last log line
 *   read_port()            a port config for the read path
 *   line_push()            bytes the UART has received, for the read path
 *   line_fail              nonzero: the next UART read fails outright
 *
 * The protocol is a fake that writes down every byte it is given and
 * every idle, and treats a few bytes as whole frames:
 *
 *   'F'  a channel frame, raw[0] the count of frames so far
 *   'S'  a channel frame with the failsafe flag
 *   'E'  a bad frame
 */

struct fake_protocol {
	struct rc_dev rc;
	struct rc_input input;
	char seen[1024];	/* every byte parsed, '|' for each idle */
	size_t seen_len;
	unsigned parse_calls;
};

static void fake_note(struct fake_protocol *fake, char c)
{
	assert(fake->seen_len + 1 < sizeof(fake->seen));
	fake->seen[fake->seen_len++] = c;
	fake->seen[fake->seen_len] = '\0';
}

static void fake_parse(struct rc_dev *rc, const uint8_t *data, size_t len)
{
	struct fake_protocol *fake = rc->priv;

	fake->parse_calls++;
	for (size_t i = 0; i < len; i++) {
		fake_note(fake, (char)data[i]);
		switch (data[i]) {
		case 'F':
			rc->raw[0] = (uint16_t)(rc->frames + 1);
			rc_frame_done(rc, false);
			break;
		case 'S':
			rc_frame_done(rc, true);
			break;
		case 'E':
			rc_frame_error(rc);
			break;
		}
	}
}

static void fake_idle(struct rc_dev *rc)
{
	fake_note(rc->priv, '|');
}

static const struct rc_ops fake_ops = {
	.parse = fake_parse,
	.idle = fake_idle,
};

static struct fake_protocol fake;

/* A fresh fake on the fed path, raw range 172/992/1811 like CRSF. */
static struct rc_dev *fake_fed(uint32_t timeout_ms)
{
	struct rc_port_config port = { .fed = true };
	static const struct rc_line line = { .baud = 115200 };

	memset(&fake, 0, sizeof(fake));
	assert(rc_input_open(&fake.input, &port, &line) == 0);
	rc_dev_init(&fake.rc, "fake", &fake_ops, &fake, &fake.input, 4, 172,
		    992, 1811, timeout_ms);
	return &fake.rc;
}

static size_t feed(struct rc_dev *rc, const char *bytes)
{
	return rc_rx_feed(rc, (const uint8_t *)bytes, strlen(bytes));
}

static void test_update_fed(void)
{
	struct rc_dev *rc = fake_fed(0);
	uint16_t raw[4];

	assert(rc->timeout_ms == RC_DEFAULT_TIMEOUT_MS);

	/* Nothing fed, nothing parsed. */
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(fake.seen_len == 0);

	/* Bytes but no frame: parsed, still -EAGAIN. */
	assert(feed(rc, "ab") == 2);
	assert(rc_update(rc, 1) == -EAGAIN);
	assert(!strcmp(fake.seen, "ab"));

	/* A frame is stamped with now_ms. */
	assert(feed(rc, "F") == 1);
	assert(rc_update(rc, 1000) == 0);
	assert(rc->seen && rc->frame_ms == 1000 && rc->frames == 1);

	/* Three frames between two updates: one stamp, the newest raw. */
	assert(feed(rc, "FxFF") == 4);
	assert(rc_update(rc, 1010) == 0);
	assert(rc->frame_ms == 1010 && rc->frames == 4);
	assert(rc_get_raw(rc, raw, 1) == 0 && raw[0] == 4);
	assert(!strcmp(fake.seen, "abFFxFF"));

	/* No new frame: the stamp stays. */
	assert(rc_update(rc, 2000) == -EAGAIN);
	assert(rc->frame_ms == 1010);

	/* A bad frame counts, and is not a frame. */
	assert(feed(rc, "E") == 1);
	assert(rc_update(rc, 2001) == -EAGAIN);
	assert(rc->errors == 1 && rc->frames == 4);
}

/* The ring wraps, fills, drops what does not fit, and counts it. */
static void test_ring(void)
{
	struct rc_dev *rc = fake_fed(0);
	uint8_t block[RC_RING_LEN + 10];
	struct rc_link link;

	/* Many times round: what comes out is what went in, in order. */
	for (unsigned round = 0; round < 3 * RC_RING_LEN / 7 + 5; round++) {
		uint8_t seven[7];

		for (int i = 0; i < 7; i++)
			seven[i] = (uint8_t)('a' + (round * 7 + i) % 26);
		fake.seen_len = 0;
		assert(rc_rx_feed(rc, seven, 7) == 7);
		assert(rc_update(rc, 0) == -EAGAIN);
		assert(fake.seen_len == 7 &&
		       !memcmp(fake.seen, seven, 7));
	}

	/* Full: the rest is dropped and counted; nothing already in is
	 * overwritten. */
	memset(block, 'z', sizeof(block));
	block[0] = 'F';
	assert(rc_rx_feed(rc, block, sizeof(block)) == RC_RING_LEN);
	assert(rc_rx_feed(rc, block, 3) == 0);
	assert(rc_get_link(rc, 0, &link) == 0 && link.dropped == 13);
	fake.seen_len = 0;
	assert(rc_update(rc, 5) == 0);
	assert(fake.seen_len == RC_RING_LEN && fake.seen[0] == 'F');
	/* The parse sees the ring in chunks, not a byte at a time. */
	assert(fake.parse_calls < RC_RING_LEN);

	/* Room again. */
	assert(feed(rc, "xy") == 2);

	/* NULLs take nothing. */
	assert(rc_rx_feed(NULL, block, 1) == 0);
	assert(rc_rx_feed(rc, NULL, 1) == 0);
}

/* The idle mark falls between the bytes it was made after and those after
 * it, and is delivered once. */
static void test_idle(void)
{
	struct rc_dev *rc = fake_fed(0);

	feed(rc, "ab");
	rc_rx_idle(rc);
	feed(rc, "cd");
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(!strcmp(fake.seen, "ab|cd"));

	/* Not again on the next update. */
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(!strcmp(fake.seen, "ab|cd"));

	/* A mark with nothing fed since the last update lands at once, and
	 * once: the tail sits on it afterwards. */
	rc_rx_idle(rc);
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(!strcmp(fake.seen, "ab|cd|"));
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(!strcmp(fake.seen, "ab|cd|"));

	/* A mark right after a frame: the frame, then the idle. */
	feed(rc, "F");
	rc_rx_idle(rc);
	assert(rc_update(rc, 7) == 0);
	assert(!strcmp(fake.seen, "ab|cd|F|"));

	/* Several marks before one update: each where it fell. */
	feed(rc, "g");
	rc_rx_idle(rc);
	feed(rc, "h");
	rc_rx_idle(rc);
	feed(rc, "i");
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(!strcmp(fake.seen, "ab|cd|F|g|h|i"));

	/* A protocol with no idle op: the mark is passed over. */
	{
		static const struct rc_ops no_idle = { .parse = fake_parse };

		rc->ops = &no_idle;
		feed(rc, "j");
		rc_rx_idle(rc);
		feed(rc, "k");
		assert(rc_update(rc, 0) == -EAGAIN);
		assert(!strcmp(fake.seen, "ab|cd|F|g|h|ijk"));
		rc->ops = &fake_ops;
	}

	rc_rx_idle(NULL);	/* harmless */
}

/*
 * More marks than RC_IDLE_MARKS between two updates: the oldest are lost,
 * and their bytes run into the first mark still kept.
 */
static void test_idle_overrun(void)
{
	struct rc_dev *rc = fake_fed(0);
	char expect[64] = "";
	char byte[2] = "a";

	for (int i = 0; i < RC_IDLE_MARKS + 2; i++) {
		byte[0] = (char)('a' + i);
		feed(rc, byte);
		rc_rx_idle(rc);
	}
	assert(rc_update(rc, 0) == -EAGAIN);
	/* a, b and c before the first kept mark, then one mark per byte. */
	strcat(expect, "abc|");
	for (int i = 3; i < RC_IDLE_MARKS + 2; i++) {
		size_t n = strlen(expect);

		expect[n] = (char)('a' + i);
		expect[n + 1] = '|';
		expect[n + 2] = '\0';
	}
	assert(!strcmp(fake.seen, expect));

	/* Exactly RC_IDLE_MARKS: none lost. */
	fake.seen_len = 0;
	fake.seen[0] = '\0';
	for (int i = 0; i < RC_IDLE_MARKS; i++) {
		feed(rc, "x");
		rc_rx_idle(rc);
	}
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(fake.seen_len == 2 * RC_IDLE_MARKS);
	for (int i = 0; i < RC_IDLE_MARKS; i++)
		assert(fake.seen[2 * i] == 'x' && fake.seen[2 * i + 1] == '|');

	/*
	 * A slot the writer reused while the reader was reading it can hold
	 * a position behind the tail (see rc_update_fed()). Set one up by
	 * hand: it is passed over, and the marks after it still arrive.
	 */
	{
		struct rc_ring *ring = &fake.input.ring;
		uint32_t count = ring->idle_count;

		ring->idle_at[count % RC_IDLE_MARKS] = ring->tail - 5;
		ring->idle_count = count + 1;
		feed(rc, "z");
		rc_rx_idle(rc);
		fake.seen_len = 0;
		fake.seen[0] = '\0';
		assert(rc_update(rc, 0) == -EAGAIN);
		assert(!strcmp(fake.seen, "z|"));
		fake.seen_len = 2 * RC_IDLE_MARKS;
		fake.seen[fake.seen_len] = '\0';
	}

	/* A mark made with nothing fed after a lapped one sits at the same
	 * place: delivered with no bytes, harmless. */
	rc_rx_idle(rc);
	rc_rx_idle(rc);
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(!strcmp(fake.seen + 2 * RC_IDLE_MARKS, "||"));
}

static void test_normalise(void)
{
	struct rc_dev *rc = fake_fed(0);
	float channels[4];

	feed(rc, "F");
	assert(rc_update(rc, 0) == 0);

	/* The three fixed points exactly, and linear past them. */
	rc->raw[0] = 172;
	rc->raw[1] = 992;
	rc->raw[2] = 1811;
	rc->raw[3] = 0;
	assert(rc_get_channels(rc, channels, 4) == 0);
	assert(channels[0] == -1.0f && channels[1] == 0.0f &&
	       channels[2] == 1.0f);
	assert(fabsf(channels[3] - (-992.0f / 820.0f)) < 1e-6f);

	/* Each half its own scale: 820 below the centre, 819 above. */
	rc->raw[0] = 992 - 410;
	rc->raw[1] = 992 + 819 / 2;
	rc->raw[2] = 2047;
	assert(rc_get_channels(rc, channels, 3) == 0);
	assert(channels[0] == -0.5f);
	assert(fabsf(channels[1] - 409.0f / 819.0f) < 1e-6f);
	assert(fabsf(channels[2] - 1055.0f / 819.0f) < 1e-6f);

	/* Fewer than all is fine; more than the protocol has is not. */
	assert(rc_get_channels(rc, channels, 0) == 0);
	assert(rc_get_channels(rc, channels, 5) == -EINVAL);
	assert(rc_get_channels(rc, NULL, 1) == -EINVAL);
	assert(rc_get_channels(NULL, channels, 1) == -EINVAL);
}

static void test_before_first_frame(void)
{
	struct rc_dev *rc = fake_fed(0);
	float channels[4];
	uint16_t raw[4];
	struct rc_link link;

	assert(rc_get_channels(rc, channels, 4) == -ENODATA);
	assert(rc_get_raw(rc, raw, 4) == -ENODATA);
	assert(rc_get_link(rc, 123, &link) == 0);
	assert(link.state == RC_LINK_NO_FRAME && link.age_ms == UINT32_MAX);
	assert(link.frames == 0 && link.errors == 0 && link.dropped == 0);

	/* Errors alone are not a frame. */
	feed(rc, "EE");
	assert(rc_update(rc, 0) == -EAGAIN);
	assert(rc_get_link(rc, 0, &link) == 0);
	assert(link.state == RC_LINK_NO_FRAME && link.errors == 2);
}

static void test_link(void)
{
	struct rc_dev *rc = fake_fed(50);
	struct rc_link link;

	feed(rc, "F");
	assert(rc_update(rc, 1000) == 0);
	assert(rc_get_link(rc, 1000, &link) == 0);
	assert(link.state == RC_LINK_UP && link.age_ms == 0 &&
	       link.frames == 1);

	/* Up to the timeout itself is up; one past is a timeout, and the
	 * age keeps climbing whether or not anyone calls rc_update(). */
	assert(rc_get_link(rc, 1050, &link) == 0);
	assert(link.state == RC_LINK_UP && link.age_ms == 50);
	assert(rc_get_link(rc, 1051, &link) == 0);
	assert(link.state == RC_LINK_TIMEOUT && link.age_ms == 51);
	assert(rc_get_link(rc, 9000, &link) == 0);
	assert(link.state == RC_LINK_TIMEOUT && link.age_ms == 8000);

	/* The receiver's own flag. */
	feed(rc, "S");
	assert(rc_update(rc, 9001) == 0);
	assert(rc_get_link(rc, 9001, &link) == 0);
	assert(link.state == RC_LINK_FAILSAFE);
	/* ... and stale beats it. */
	assert(rc_get_link(rc, 9100, &link) == 0);
	assert(link.state == RC_LINK_TIMEOUT);
	/* A good frame clears it. */
	feed(rc, "F");
	assert(rc_update(rc, 9200) == 0);
	assert(rc_get_link(rc, 9200, &link) == 0);
	assert(link.state == RC_LINK_UP);

	/* A now_ms from before the stamp is a huge age: a timeout, never a
	 * fresh link. */
	assert(rc_get_link(rc, 9199, &link) == 0);
	assert(link.state == RC_LINK_TIMEOUT && link.age_ms == UINT32_MAX);

	/* Across the wrap of a 32-bit millisecond clock. */
	feed(rc, "F");
	assert(rc_update(rc, 0xFFFFFFF0u) == 0);
	assert(rc_get_link(rc, 0x10, &link) == 0);
	assert(link.state == RC_LINK_UP && link.age_ms == 0x20);

	/* Dead for 2^31 + 1 ms: a timeout, not a fresh link. */
	feed(rc, "F");
	assert(rc_update(rc, 1000) == 0);
	assert(rc_update(rc, 1000 + 0x80000001u) == -EAGAIN);
	assert(rc_get_link(rc, 1000 + 0x80000001u, &link) == 0);
	assert(link.state == RC_LINK_TIMEOUT);

	/* Dead for 2^32 + 10 ms, the age wrapped to 10: the latch an
	 * earlier rc_update() set holds the timeout. */
	assert(rc_update(rc, 1010) == -EAGAIN);	/* 2^32 + 10 later */
	assert(rc_get_link(rc, 1010, &link) == 0);
	assert(link.age_ms == 10 && link.state == RC_LINK_TIMEOUT);

	/* The next frame clears it. */
	feed(rc, "F");
	assert(rc_update(rc, 1020) == 0);
	assert(rc_get_link(rc, 1020, &link) == 0);
	assert(link.state == RC_LINK_UP && !rc->expired);

	/* The latch needs a stale frame: an update within timeout_ms, or
	 * with no frame ever, sets nothing. */
	assert(rc_update(rc, 1070) == -EAGAIN && !rc->expired);
	assert(rc_update(rc, 1071) == -EAGAIN && rc->expired);
	{
		struct rc_dev *fresh = fake_fed(50);

		assert(rc_update(fresh, 999999) == -EAGAIN && !fresh->expired);
	}

	assert(rc_get_link(rc, 0, NULL) == -EINVAL);
	assert(rc_get_link(NULL, 0, &link) == -EINVAL);
}

/* The read path: bytes off the UART, no waiting. */
static void test_update_read(void)
{
	struct rc_port_config port = read_port();
	static const struct rc_line line = { .baud = 115200 };
	struct rc_link link;

	memset(&fake, 0, sizeof(fake));
	assert(rc_input_open(&fake.input, &port, &line) == 0);
	rc_dev_init(&fake.rc, "fake", &fake_ops, &fake, &fake.input, 4, 172,
		    992, 1811, 0);

	assert(rc_update(&fake.rc, 0) == -EAGAIN);
	line_push((const uint8_t *)"abF", 3);
	assert(rc_update(&fake.rc, 40) == 0);
	assert(!strcmp(fake.seen, "abF") && fake.rc.frame_ms == 40);

	/* More than one chunk in one go. */
	{
		uint8_t many[200];

		memset(many, 'x', sizeof(many));
		many[150] = 'F';
		line_push(many, sizeof(many));
		assert(rc_update(&fake.rc, 50) == 0);
		assert(fake.seen_len == 203 && fake.rc.frames == 2);
	}

	/* A UART fault comes back when there was no frame; the byte it
	 * failed on is read next time. */
	line_push((const uint8_t *)"y", 1);
	line_fail = 1;
	assert(rc_update(&fake.rc, 60) == -EIO);
	line_fail = 0;
	assert(rc_update(&fake.rc, 60) == -EAGAIN);
	assert(fake.seen[fake.seen_len - 1] == 'y');

	/* Feeding a read receiver takes nothing. */
	assert(feed(&fake.rc, "F") == 0);
	assert(rc_get_link(&fake.rc, 60, &link) == 0 && link.frames == 2);

	assert(rc_input_close(&fake.input) == 0);
}

static void test_update_args(void)
{
	struct rc_dev *rc = fake_fed(0);
	struct rc_dev unbound = { .name = "unbound" };

	assert(rc_update(NULL, 0) == -EINVAL);
	assert(rc_update(&unbound, 0) == -EINVAL);
	rc->input = NULL;
	assert(rc_update(rc, 0) == -EINVAL);
}

static void test_log(void)
{
	struct rc_dev *rc = fake_fed(0);
	struct rc_link link = {
		.state = RC_LINK_UP, .age_ms = 12, .frames = 300,
		.errors = 2, .dropped = 0,
	};

	rc_log_link(rc, &link);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "rc: fake: up, age 12 ms, frames 300 errors 2 dropped 0"));
	link.state = RC_LINK_NO_FRAME;
	link.age_ms = UINT32_MAX;
	link.frames = 0;
	rc_log_link(rc, &link);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "rc: fake: no frame, frames 0 errors 2 dropped 0"));
	link.state = RC_LINK_TIMEOUT;
	link.age_ms = 500;
	rc_log_link(rc, &link);
	assert(!strcmp(stub_log_last, LOG_PREFIX("I")
		       "rc: fake: timeout, age 500 ms, frames 0 errors 2 "
		       "dropped 0"));
	link.state = RC_LINK_FAILSAFE;
	rc_log_link(rc, &link);
	assert(strstr(stub_log_last, "rc: fake: failsafe, age 500 ms"));
	rc_err(rc, "no frame (%d)", -ETIMEDOUT);
	assert(!strcmp(stub_log_last,
		       LOG_PREFIX("E") "rc: fake: no frame (-110)"));
}

static void run_core_cases(void)
{
	test_update_fed();
	test_ring();
	test_idle();
	test_idle_overrun();
	test_normalise();
	test_before_first_frame();
	test_link();
	test_update_read();
	test_update_args();
	test_log();
}
