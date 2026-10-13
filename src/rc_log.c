#include <stdint.h>

#include "rc_log.h"

static const char *rc_link_state_name(enum rc_link_state state)
{
	switch (state) {
	case RC_LINK_UP:
		return "up";
	case RC_LINK_FAILSAFE:
		return "failsafe";
	case RC_LINK_TIMEOUT:
		return "timeout";
	default:
		return "no frame";
	}
}

void rc_log_link(const struct rc_dev *rc, const struct rc_link *link)
{
	if (link->age_ms == UINT32_MAX)
		rc_info(rc, "%s, frames %lu errors %lu dropped %lu",
			rc_link_state_name(link->state),
			(unsigned long)link->frames,
			(unsigned long)link->errors,
			(unsigned long)link->dropped);
	else
		rc_info(rc, "%s, age %lu ms, frames %lu errors %lu dropped %lu",
			rc_link_state_name(link->state),
			(unsigned long)link->age_ms,
			(unsigned long)link->frames,
			(unsigned long)link->errors,
			(unsigned long)link->dropped);
}
