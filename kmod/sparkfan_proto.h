/* SPDX-License-Identifier: GPL-2.0-only
 * Pure EC thermal-mailbox protocol helpers shared by the kernel module and test.c.
 * No kernel dependencies: only fixed-width ints. */
#ifndef SPARKFAN_PROTO_H
#define SPARKFAN_PROTO_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16;
#endif

#define EC_OUTER_THERMAL	0x07
#define EC_CMD_CAPS		0x01	/* 13-byte reply */
#define EC_CMD_SET_FLOOR	0x05	/* LE16 rpm, 0xffff = disabled */
#define EC_CMD_TELEMETRY	0x07	/* 64 bytes at reply+3 */
#define FLOOR_AUTO		0xffffU
#define FLOOR_MAX_DEFAULT	13500U

struct ec_caps { u8 mode; u16 fan0_min, fan0_max, fan1_min, fan1_max; };

static inline u16 le16_at(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline void put_le16_at(u8 *p, u16 v) { p[0] = v & 0xff; p[1] = v >> 8; }

/* [outer, inner, status=0, LE16 rpm] */
static inline void build_set_floor(u8 req[5], u16 rpm)
{
	req[0] = EC_OUTER_THERMAL; req[1] = EC_CMD_SET_FLOOR; req[2] = 0;
	put_le16_at(&req[3], rpm);
}

/* reply must echo outer+inner, status 0, and the rpm. 0 = ok, -1 = mismatch, -2 = unsupported */
static inline int check_set_floor_reply(const u8 req[5], const u8 rsp[5])
{
	if (rsp[0] != req[0] || rsp[1] != req[1]) return -1;
	if (rsp[2] != 0) return -2;
	return le16_at(&rsp[3]) == le16_at(&req[3]) ? 0 : -1;
}

/* 07 01 00 cap mode f0min f0max f1min f1max  (13 bytes). 0 = ok */
static inline int parse_caps(const u8 rsp[13], struct ec_caps *c)
{
	if (rsp[0] != EC_OUTER_THERMAL || rsp[1] != EC_CMD_CAPS) return -1;
	if (rsp[2] != 0) return -2;
	c->mode = rsp[4];
	c->fan0_min = le16_at(&rsp[5]); c->fan0_max = le16_at(&rsp[7]);
	c->fan1_min = le16_at(&rsp[9]); c->fan1_max = le16_at(&rsp[11]);
	return 0;
}

/* Gate: FLOOR_AUTO passes through; anything else is clamped into fan1's range (fallback 1..13500). */
static inline u16 clamp_floor(u16 target, const struct ec_caps *c)
{
	u16 lo = c && c->fan1_min ? c->fan1_min : 1;
	u16 hi = c && c->fan1_max ? c->fan1_max : FLOOR_MAX_DEFAULT;
	if (target == FLOOR_AUTO) return target;
	if (target < lo) return lo;
	if (target > hi) return hi;
	return target;
}

/* "auto"|"0" -> FLOOR_AUTO, "max" -> 0 (caller resolves), digits -> rpm (1..0xfffe). -1 = bad input */
static inline int parse_floor_str(const char *s, u16 *out)
{
	unsigned long v = 0; size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) n--;
	if (!n) return -1;
	if ((n == 4 && !strncmp(s, "auto", 4)) || (n == 1 && s[0] == '0')) { *out = FLOOR_AUTO; return 0; }
	if (n == 3 && !strncmp(s, "max", 3)) { *out = 0; return 0; }
	for (size_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') return -1;
		v = v * 10 + (s[i] - '0');
		if (v > 0xfffe) return -1;
	}
	if (v < 1) return -1;
	*out = (u16)v;
	return 0;
}
#endif
