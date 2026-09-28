// SPDX-License-Identifier: MIT
/*
 * CRC-32 as the old rc_crc32sw() with initial value 0 (Ramon Chips); the
 * result equals zlib's crc32(), e.g. "123456789" -> 0xCBF43926.
 */
#include "ramon_priv.h"

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void)
{
	uint32_t i, r;
	int j;

	for (i = 0; i < 256; i++) {
		r = i;
		for (j = 0; j < 8; j++)
			r = (r & 1 ? 0 : 0xEDB88320u) ^ r >> 1;
		crc_table[i] = r ^ 0xFF000000u;
	}
}

uint32_t ramon_crc32_update(uint32_t crc, const void *data, size_t n)
{
	const uint8_t *p = data;
	size_t i;

	pthread_once(&crc_once, crc_init);
	for (i = 0; i < n; i++)
		crc = crc_table[(uint8_t)crc ^ p[i]] ^ crc >> 8;
	return crc;
}

uint32_t ramon_crc32(const void *data, size_t n)
{
	return ramon_crc32_update(0, data, n);
}
