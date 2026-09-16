/* Userspace unit tests for the pure protocol/gate logic in sparkfan_proto.h.  cc -Wall -Wextra test.c -o test && ./test */
#include <stdio.h>
#include <stdlib.h>
#include "sparkfan_proto.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
	u8 req[5], rsp[5];
	u16 v;
	struct ec_caps c;

	/* frames: matches the documented EC bytes: full speed 07 05 00 BC 34, auto 07 05 00 FF FF */
	build_set_floor(req, 13500);
	CHECK(req[0] == 0x07 && req[1] == 0x05 && req[2] == 0x00 && req[3] == 0xBC && req[4] == 0x34);
	build_set_floor(req, FLOOR_AUTO);
	CHECK(req[3] == 0xFF && req[4] == 0xFF);

	/* reply check: echo ok, status ff = unsupported, wrong rpm = mismatch */
	build_set_floor(req, 9000);
	memcpy(rsp, req, 5);
	CHECK(check_set_floor_reply(req, rsp) == 0);
	rsp[2] = 0xff; CHECK(check_set_floor_reply(req, rsp) == -2);
	memcpy(rsp, req, 5); rsp[3] ^= 1; CHECK(check_set_floor_reply(req, rsp) == -1);
	memcpy(rsp, req, 5); rsp[1] = 0x03; CHECK(check_set_floor_reply(req, rsp) == -1);

	/* caps: the documented device reply 07 01 00 01 00 EC 04 28 23 62 07 BC 34 */
	const u8 caps_rsp[13] = { 0x07, 0x01, 0x00, 0x01, 0x00, 0xEC, 0x04, 0x28, 0x23, 0x62, 0x07, 0xBC, 0x34 };
	CHECK(parse_caps(caps_rsp, &c) == 0);
	CHECK(c.mode == 0 && c.fan0_min == 1260 && c.fan0_max == 9000 && c.fan1_min == 1890 && c.fan1_max == 13500);
	{ u8 bad[13]; memcpy(bad, caps_rsp, 13); bad[2] = 0xff; CHECK(parse_caps(bad, &c) == -2); }

	/* gate: clamp into fan1 range, auto passes, unknown caps fall back to 1..13500 */
	CHECK(clamp_floor(9000, &c) == 9000);
	CHECK(clamp_floor(100, &c) == 1890);
	CHECK(clamp_floor(20000, &c) == 13500);
	CHECK(clamp_floor(FLOOR_AUTO, &c) == FLOOR_AUTO);
	CHECK(clamp_floor(20000, NULL) == 13500);
	CHECK(clamp_floor(1, NULL) == 1);

	/* sysfs input parsing */
	CHECK(parse_floor_str("auto\n", &v) == 0 && v == FLOOR_AUTO);
	CHECK(parse_floor_str("0", &v) == 0 && v == FLOOR_AUTO);
	CHECK(parse_floor_str("max", &v) == 0 && v == 0);
	CHECK(parse_floor_str("9000\n", &v) == 0 && v == 9000);
	CHECK(parse_floor_str("65534", &v) == 0 && v == 65534);
	CHECK(parse_floor_str("65535", &v) == -1);	/* would alias FLOOR_AUTO */
	CHECK(parse_floor_str("", &v) == -1);
	CHECK(parse_floor_str("9k", &v) == -1);
	CHECK(parse_floor_str("-1", &v) == -1);
	CHECK(parse_floor_str("MAX", &v) == -1);

	printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
	return fails ? 1 : 0;
}
