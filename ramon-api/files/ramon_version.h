/* SPDX-License-Identifier: MIT */
/*
 * libramon version. RAMON_API_VERSION must equal PV in ramon-api.bb.
 */
#ifndef RAMON_VERSION_H
#define RAMON_VERSION_H

#define RAMON_API_VERSION_MAJOR		0
#define RAMON_API_VERSION_MINOR		1
#define RAMON_API_VERSION_PATCH		0
#define RAMON_API_VERSION		"0.1.0"

/* The driver this library was developed against; ramon_open() warns on anything older. */
#define RAMON_API_DRIVER_MIN_MAJOR	0
#define RAMON_API_DRIVER_MIN_MINOR	8
#define RAMON_API_DRIVER_MIN_PATCH	8
#define RAMON_API_DRIVER_MIN		"0.8.8"

#ifdef __cplusplus
extern "C" {
#endif

/* RAMON_API_VERSION of the library that was linked (may differ from the header's) */
const char *ramon_api_version(void);
/* RAMON_ABI_VERSION the library was built with */
unsigned ramon_api_abi_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RAMON_VERSION_H */
