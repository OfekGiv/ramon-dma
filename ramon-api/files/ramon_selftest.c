// SPDX-License-Identifier: MIT
/*
 * Host self-test of libramon's pure functions (no device needed):
 * CRC, SPW packet build/parse, SPFI table helpers, SYSMON and RS-TOP
 * decoding, error formatting. Run with "make check".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ramon.h"
#include "ramon_spw.h"
#include "ramon_spfi.h"

static int failures;

#define CHECK(cond, ...) do { \
		if (!(cond)) { \
			failures++; \
			printf("FAIL: %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static void test_crc(void)
{
	static const char v[] = "123456789";
	uint32_t a;

	CHECK(ramon_crc32(v, 9) == 0xCBF43926u, "crc32(\"123456789\") = 0x%08x", ramon_crc32(v, 9));
	a = ramon_crc32_update(ramon_crc32(v, 4), v + 4, 5);
	CHECK(a == 0xCBF43926u, "chained crc = 0x%08x", a);
	CHECK(ramon_crc32(NULL, 0) == 0, "crc of nothing");
}

/* packet as the FPGA delivers it: built packet + 16-byte footer */
static uint32_t with_footer(uint8_t *buf, uint32_t len, uint32_t err)
{
	struct ramon_spw_footer f = { len, err, 7, 0 };

	memcpy(buf + len, &f, sizeof(f));
	return len + sizeof(f);
}

static void test_spw(void)
{
	static uint8_t pay1[1000], pay2[37], buf[2048];
	struct ramon_iov iov[2] = { { pay1, sizeof(pay1) }, { pay2, sizeof(pay2) } };
	struct ramon_spw_msg m = { 0x41, 9, 0x1234, 0x98, 77, 0, RAMON_SPW_MSG_KEEP_ID };
	struct ramon_spw_rx rx;
	struct ramon_spw_hdr h;
	uint32_t len, raw;
	size_t i;

	for (i = 0; i < sizeof(pay1); i++)
		pay1[i] = (uint8_t)(i * 7);
	for (i = 0; i < sizeof(pay2); i++)
		pay2[i] = (uint8_t)(0xA0 + i);
	CHECK(!ramon_spw_build(68, &m, iov, 2, buf, sizeof(buf), &len), "build");
	CHECK(len == 40 + 1037, "built %u bytes", len);
	memcpy(&h, buf, sizeof(h));
	CHECK(h.dst == 0x41 && h.src == 68 && h.protocol_id == 9 && h.app_type == 0x1234 &&
	      h.attribute_id == 0x98 && h.packet_id == 77, "header fields");
	CHECK(h.l3_len == 32 + 1037, "l3_len %u", h.l3_len);
	CHECK(h.header_crc == ramon_spw_header_crc(&h), "header crc");

	/* as the NN would receive it, and as our own node */
	raw = with_footer(buf, len, 0);
	CHECK(!ramon_spw_parse(buf, raw, 0x41, &rx), "parse");
	CHECK(rx.flags == 0, "clean packet flags 0x%x", rx.flags);
	CHECK(rx.payload_len == 1037 && !memcmp(rx.payload, pay1, 1000) &&
	      !memcmp(rx.payload + 1000, pay2, 37), "payload round trip");
	CHECK(rx.footer.pkt_size == len && rx.footer.packet_count == 7, "footer copy");
	ramon_spw_parse(buf, raw, 68, &rx);
	CHECK(rx.flags == RAMON_SPW_RX_NOT_FOR_US, "not-for-us flag 0x%x", rx.flags);

	buf[40 + 500] ^= 1;
	ramon_spw_parse(buf, raw, 0x41, &rx);
	CHECK(rx.flags == RAMON_SPW_RX_PAYLOAD_CRC_BAD, "payload corruption flags 0x%x", rx.flags);
	buf[40 + 500] ^= 1;
	buf[4] ^= 0x10;		/* src */
	ramon_spw_parse(buf, raw, 0x41, &rx);
	CHECK(rx.flags == RAMON_SPW_RX_HDR_CRC_BAD, "header corruption flags 0x%x", rx.flags);
	buf[4] ^= 0x10;

	ramon_spw_parse(buf, raw - 100, 0x41, &rx);
	CHECK(rx.flags & RAMON_SPW_RX_TRUNCATED, "truncated flags 0x%x", rx.flags);
	CHECK(ramon_spw_parse(buf, 39, 0x41, &rx) == -EINVAL && !rx.hdr, "shorter than a header");
	with_footer(buf, len, 0x4);
	ramon_spw_parse(buf, raw, 0x41, &rx);
	CHECK(rx.flags == RAMON_SPW_RX_FOOTER_ERR, "footer error flags 0x%x", rx.flags);

	/* empty payload, and a buffer too small */
	CHECK(!ramon_spw_build(68, &m, NULL, 0, buf, sizeof(buf), &len) && len == 40, "empty");
	raw = with_footer(buf, len, 0);
	ramon_spw_parse(buf, raw, 0x41, &rx);
	CHECK(rx.flags == 0 && rx.payload_len == 0, "empty payload flags 0x%x", rx.flags);
	CHECK(ramon_spw_build(68, &m, iov, 2, buf, 100, &len) == -EMSGSIZE, "too small");
	CHECK(!strcmp(ramon_spw_swup_status_str(RAMON_SWUP_END, 2), "bad package CRC"),
	      "sw update status text");
}

static void test_spfi(void)
{
	struct ramon_spfi_table *t = calloc(1, sizeof(*t));
	uint32_t ids[4];
	unsigned n;

	CHECK(t != NULL, "calloc");
	if (!t)
		return;
	t->rec[192].status = RAMON_SPFI_ST_EXIST | RAMON_SPFI_ST_OPEN;
	t->rec[190].status = RAMON_SPFI_ST_EXIST;
	t->rec[197].status = 0;
	n = ramon_spfi_find_free(t, ids, 3);
	CHECK(n == 3 && ids[0] == 191 && ids[1] == 189 && ids[2] == 188,
	      "find_free: %u ids %u %u %u", n, ids[0], ids[1], ids[2]);
	CHECK(ramon_spfi_count_open(t) == 1 && ramon_spfi_count_exist(t) == 2, "counts");
	CHECK(offsetof(struct ramon_spfi_record, latest_write_offs) == 36, "record layout");
	CHECK(!strcmp(ramon_spfi_op_name(RAMON_SPFI_OP_FORMAT), "FORMAT"), "op name");
	free(t);
}

static void test_board(void)
{
	struct ramon_rstop_time tm;
	uint32_t raw = (uint32_t)((25.0 + 280.239) / 509.314 * 65536.0);
	double c = ramon_sysmon_temp_c(raw);
	uint32_t v;

	CHECK(c > 24.98 && c < 25.02, "temperature %.3f", c);
	CHECK(ramon_sysmon_volts(32768, 3) == 1.5, "volts");
	CHECK(ramon_sysmon_rails[0].offset == 0x0004 && ramon_sysmon_rails[36].offset == 0x020C &&
	      ramon_sysmon_rails[10].win == RAMON_WIN_SYSMON_PL, "rail table");
	/* 27/09/26 13:45:59 */
	v = 27u << 27 | 9u << 23 | 26u << 17 | 13u << 12 | 45u << 6 | 59u;
	ramon_rstop_decode_time(v, &tm);
	CHECK(tm.day == 27 && tm.month == 9 && tm.year == 26 && tm.hour == 13 && tm.min == 45 &&
	      tm.sec == 59, "time %u/%u/%u %u:%u:%u", tm.day, tm.month, tm.year, tm.hour, tm.min,
	      tm.sec);
}

static void test_errors(void)
{
	struct ramon_status st;
	char buf[256];
	uint32_t code;

	for (code = 0; code < RAMON_E_LIB_BASE; code++)
		if (ramon_err_lookup(code))
			CHECK(strncmp(ramon_err_str(code), "RAMON_E_", 8) == 0, "name of %u", code);
	CHECK(!strcmp(ramon_err_str(RAMON_EL_SPW_NACK), "RAMON_EL_SPW_NACK"), "lib name");
	CHECK(!strcmp(ramon_err_str(0x1FFF), "RAMON_EL_UNKNOWN"), "unknown lib code");

	ramon_status_set(&st, RAMON_E_BUF_RANGE, -ERANGE, 3, 0, "buf %u: offset too big", 3);
	ramon_strerror(&st, buf, sizeof(buf));
	CHECK(strstr(buf, "RAMON_E_BUF_RANGE") && strstr(buf, "buf 3: offset too big") &&
	      strstr(buf, "errno 34"), "driver strerror \"%s\"", buf);
	ramon_status_set(&st, RAMON_EL_SPFI_NN_ERROR, -EPROTO, 5, 0x30, "OPEN stream 7: NN rx_err_code 5");
	ramon_strerror(&st, buf, sizeof(buf));
	CHECK(strstr(buf, "RAMON_EL_SPFI_NN_ERROR") && strstr(buf, "rx_err_code 5"),
	      "lib strerror \"%s\"", buf);
	ramon_status_clear(&st);
	ramon_strerror(&st, buf, sizeof(buf));
	CHECK(!strcmp(buf, "ok"), "ok strerror \"%s\"", buf);
	CHECK(!strcmp(ramon_api_version(), RAMON_API_VERSION), "version");
}

int main(void)
{
	test_crc();
	test_spw();
	test_spfi();
	test_board();
	test_errors();
	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
