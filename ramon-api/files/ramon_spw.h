/* SPDX-License-Identifier: MIT */
/*
 * libramon SpaceWire packet layer.
 *
 * The driver only moves bytes: SPW_WAIT_RX returns the size of a received
 * packet and an AXI transfer on the NN's RX channel must then move exactly
 * that many bytes. This layer owns that contract (one consumer per NN), builds
 * and checks the L2/L3/L4 headers and CRCs, and implements the NN services
 * (version, DPS, configuration push, software update).
 *
 * Packet on the wire (little endian, packed):
 *   L2 (8)  dst, protocol_id, reserved[2], src, reserved[3]
 *   L3 (24) nack_id, reserved[6], l3_len (= 32 + payload), header_crc
 *           (CRC-32 of L2+L3 with both CRC fields 0), payload_crc (CRC-32 of
 *           L4 + payload), packet_id, app_type
 *   L4 (8)  attribute_id, reserved[4]
 *   payload
 * On receive the FPGA appends a 16-byte footer (struct ramon_spw_footer).
 *
 * Receiving: either start the library RX thread for an NN (ramon_spw_rx_start)
 * and get every packet in a callback, or call ramon_spw_recv() from your own
 * thread while no RX thread runs on that NN. ramon_spw_request() works in both
 * modes and never polls.
 */
#ifndef RAMON_SPW_H
#define RAMON_SPW_H

#include "ramon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RAMON_SPW_HDR_BYTES		40
#define RAMON_SPW_L2L3_BYTES		32
#define RAMON_SPW_L3_BYTES		24
#define RAMON_SPW_FOOTER_BYTES		16
#define RAMON_SPW_NODE_DEFAULT		68	/* our node id on the ramon board */
#define RAMON_SPW_NN_NODE_DEFAULT	0x41	/* the NN's node id */
#define RAMON_SPW_BUF_DEFAULT		0x100000

/* NN services */
#define RAMON_SPW_NN_PROTOCOL		9
#define RAMON_SPW_VERSION_APP		9
#define RAMON_SPW_VERSION_ATTR		0x98
#define RAMON_SPW_DPS_APP		1
#define RAMON_SPW_DPS_ATTR		0x01
#define RAMON_SPW_ACK_ATTR		0x101	/* DPS and configuration answers */
#define RAMON_SPW_CONFIG_APP		5
#define RAMON_SPW_CONFIG_ATTR		0x01
#define RAMON_SPW_CONFIG_MAX		4096
#define RAMON_SPW_SWUP_APP		9
#define RAMON_SPW_SWUP_START_ATTR	0x90
#define RAMON_SPW_SWUP_DATA_ATTR	0x91
#define RAMON_SPW_SWUP_END_ATTR		0x92
#define RAMON_SPW_SWUP_MAGIC		0x3A
#define RAMON_SPW_SWUP_CHUNK		4096
#define RAMON_SPW_FDIR_REQUEST_ATTR	0xA0
#define RAMON_SPW_REPLY_MS		3000

struct ramon_spw_hdr {
	uint8_t  dst;
	uint8_t  protocol_id;
	uint8_t  l2_reserved1[2];
	uint8_t  src;
	uint8_t  l2_reserved2[3];
	uint16_t nack_id;
	uint8_t  l3_reserved[6];
	uint32_t l3_len;
	uint32_t header_crc;
	uint32_t payload_crc;
	uint16_t packet_id;
	uint16_t app_type;
	uint32_t attribute_id;
	uint8_t  l4_reserved[4];
} __attribute__((packed));

struct ramon_spw_footer {
	uint32_t pkt_size;
	uint32_t error_flags;
	uint32_t packet_count;
	uint32_t reserved;
};

#ifdef __cplusplus
static_assert(sizeof(struct ramon_spw_hdr) == RAMON_SPW_HDR_BYTES, "SPW header is 40 bytes");
static_assert(sizeof(struct ramon_spw_footer) == RAMON_SPW_FOOTER_BYTES, "SPW footer is 16 bytes");
#else
_Static_assert(sizeof(struct ramon_spw_hdr) == RAMON_SPW_HDR_BYTES, "SPW header is 40 bytes");
_Static_assert(sizeof(struct ramon_spw_footer) == RAMON_SPW_FOOTER_BYTES, "SPW footer is 16 bytes");
#endif

/* header fields of an outgoing message; src is the configured node id */
#define RAMON_SPW_MSG_KEEP_ID	0x1u	/* send packet_id as given (default: a per-context counter) */
struct ramon_spw_msg {
	uint8_t  dst;
	uint8_t  protocol_id;
	uint16_t app_type;
	uint32_t attribute_id;
	uint16_t packet_id;
	uint16_t nack_id;
	uint32_t flags;		/* RAMON_SPW_MSG_* */
};

/* gather list for the payload */
struct ramon_iov {
	const void *base;
	size_t len;
};

/* ramon_spw_rx.flags */
#define RAMON_SPW_RX_HDR_CRC_BAD	0x01u
#define RAMON_SPW_RX_PAYLOAD_CRC_BAD	0x02u
#define RAMON_SPW_RX_TRUNCATED		0x04u	/* shorter than header + footer, or l3_len does not fit */
#define RAMON_SPW_RX_NOT_FOR_US		0x08u	/* hdr->dst is not our node id */
#define RAMON_SPW_RX_FOOTER_ERR		0x10u	/* footer->error_flags != 0 */
#define RAMON_SPW_RX_BAD		(RAMON_SPW_RX_HDR_CRC_BAD | RAMON_SPW_RX_PAYLOAD_CRC_BAD | \
					 RAMON_SPW_RX_TRUNCATED)

/* a received packet; hdr, payload and raw point into the buffer that was parsed */
struct ramon_spw_rx {
	uint32_t nn;
	uint32_t raw_size;	/* bytes received, header + payload + footer */
	uint32_t payload_len;	/* from l3_len */
	uint32_t flags;		/* RAMON_SPW_RX_* */
	const struct ramon_spw_hdr *hdr;
	const uint8_t *payload;
	const uint8_t *raw;
	struct ramon_spw_footer footer;	/* a copy (it may be unaligned in raw); zero if absent */
};

/*
 * Called on the library RX thread of the NN for every packet that no pending
 * ramon_spw_request() took. rx and everything it points to is valid only
 * during the call: copy what you keep. The callback must return quickly and
 * must not call ramon_spw_rx_stop() or ramon_spw_fini() (they join this
 * thread). It may send packets.
 */
typedef void (*ramon_spw_rx_cb)(void *user, const struct ramon_spw_rx *rx);

/* ramon_spw_config.flags */
#define RAMON_SPW_NO_CRC_CHECK		0x1u	/* requests also accept replies with bad CRCs */
#define RAMON_SPW_ACCEPT_ANY_DST	0x2u	/* do not flag packets addressed to other nodes */

/* every 0 selects the default shown */
struct ramon_spw_config {
	uint8_t  node_id;		/* our L2 source and the dst we accept: 68 */
	uint8_t  nn_node;		/* the NN's node id for the services: 0x41 */
	uint16_t reserved;
	uint32_t rx_buf_bytes;		/* RX landing buffer per NN: 1 MiB; larger packets use a temporary one */
	uint32_t tx_slots;		/* TX buffers shared by all senders: 2 */
	uint32_t tx_slot_bytes;		/* largest packet sent, header included: 1 MiB */
	uint32_t sync_tries;		/* link resets before giving up: 1000 */
	uint32_t sync_sleep_ms;		/* between resets: 10 */
	uint32_t reset_value;		/* value written to the reset register while not synced: 0 */
	uint32_t nn_mask;		/* NNs to use: spw_mask from GET_INFO */
	uint32_t flags;			/* RAMON_SPW_NO_CRC_CHECK, RAMON_SPW_ACCEPT_ANY_DST */
};
void ramon_spw_config_default(struct ramon_spw_config *cfg);

/*
 * Allocates the TX buffers and one RX buffer per NN and finds each NN's AXI
 * channel pair. Does not touch the links. Fails with RAMON_EL_BUSY if the
 * layer is already initialised on this context.
 */
int ramon_spw_init(ramon_ctx *c, const struct ramon_spw_config *cfg, struct ramon_status *st);
/* stops the RX threads and frees the buffers; do not call it while other threads send */
void ramon_spw_fini(ramon_ctx *c);
int ramon_spw_is_inited(const ramon_ctx *c);
/* the configuration in use (defaults filled in), or the defaults before init */
void ramon_spw_get_config(const ramon_ctx *c, struct ramon_spw_config *cfg);
/* 1 if NN nn is present and enabled */
int ramon_spw_present(const ramon_ctx *c, uint32_t nn);

/* Links. These work without ramon_spw_init(). */
/* 1 synced ((reg 0x28 & 7) == 5), 0 not synced, < 0 error; raw may be NULL */
int ramon_spw_link_status(ramon_ctx *c, uint32_t nn, uint32_t *raw, struct ramon_status *st);
/*
 * The old spw_init() loop: resets the link until it reports synced. Returns 0
 * with *tries (may be NULL) = resets needed, or -ENOLINK / RAMON_EL_SPW_LINK_DOWN.
 */
int ramon_spw_sync(ramon_ctx *c, uint32_t nn, uint32_t *tries, struct ramon_status *st);
/*
 * FPGA loopback on NN nn. A change resets the link: after disabling, the link
 * is re-synced (the result of that sync is returned); after enabling, the
 * call waits for the reset to settle.
 */
int ramon_spw_loopback(ramon_ctx *c, uint32_t nn, int enable, struct ramon_status *st);
int ramon_spw_loopback_get(ramon_ctx *c, uint32_t nn, int *enabled, struct ramon_status *st);

/* Pure helpers (no context). CRC-32 as the old rc_crc32sw() with initial value 0 (== zlib crc32). */
uint32_t ramon_crc32(const void *data, size_t n);
uint32_t ramon_crc32_update(uint32_t crc, const void *data, size_t n);
uint32_t ramon_spw_header_crc(const struct ramon_spw_hdr *h);
/* header + gathered payload + both CRCs into out; -EMSGSIZE if it does not fit cap */
int ramon_spw_build(uint8_t src_node, const struct ramon_spw_msg *m, const struct ramon_iov *iov,
		    unsigned n_iov, void *out, size_t cap, uint32_t *out_len);
/*
 * Parses a received packet (header + payload + footer). Returns 0 and sets
 * rx->flags for CRC, length, address and footer problems; -EINVAL only if raw
 * is shorter than a header (rx->hdr is NULL then).
 */
int ramon_spw_parse(const void *raw, uint32_t raw_size, uint8_t our_node, struct ramon_spw_rx *rx);

/*
 * Sends one packet: waits for a free TX buffer, builds header + payload +
 * CRCs in it and runs one AXI transfer on the NN's TX channel. Thread-safe;
 * up to tx_slots sends run in parallel.
 */
int ramon_spw_send(ramon_ctx *c, uint32_t nn, const struct ramon_spw_msg *m,
		   const struct ramon_iov *iov, unsigned n_iov, uint32_t timeout_ms,
		   struct ramon_status *st);
/* zero copy: pkt already holds a complete packet (ramon_spw_build) at off */
int ramon_spw_send_buf(ramon_ctx *c, uint32_t nn, const ramon_buf *pkt, uint64_t off, uint32_t len,
		       uint32_t timeout_ms, struct ramon_status *st);

/* receive mode A: library thread per NN */
int ramon_spw_rx_start(ramon_ctx *c, uint32_t nn, ramon_spw_rx_cb cb, void *user,
		       struct ramon_status *st);
/*
 * Stops the RX thread (SPW_CANCEL + join). SPW_CANCEL releases every waiter
 * on that NN in every process, and the thread may take up to one wait period
 * (1 s) to notice.
 */
int ramon_spw_rx_stop(ramon_ctx *c, uint32_t nn);
int ramon_spw_rx_running(ramon_ctx *c, uint32_t nn);

/*
 * receive mode B: waits for one packet in the calling thread and copies it
 * (header + payload + footer) into buf. rx points into buf. Fails with
 * RAMON_EL_BUSY while an RX thread or another receiver runs on the NN, and
 * with -EMSGSIZE (packet consumed and dropped) if cap is too small.
 * CRC problems are reported in rx->flags, not as an error.
 */
int ramon_spw_recv(ramon_ctx *c, uint32_t nn, void *buf, uint32_t cap, uint32_t timeout_ms,
		   struct ramon_spw_rx *rx, struct ramon_status *st);
/* mode B: receives and drops packets until none arrives for idle_ms */
int ramon_spw_drain(ramon_ctx *c, uint32_t nn, uint32_t idle_ms, uint32_t *n_dropped,
		    struct ramon_status *st);

/* which reply a request waits for; any_* = 1 ignores that field */
struct ramon_spw_match {
	int any_src;
	uint8_t src;
	int any_app;
	uint16_t app_type;
	int any_attr;
	uint32_t attribute_id;
};

/*
 * Sends a request and waits for the first CRC-clean packet addressed to us
 * that matches want (NULL = any). With an RX thread running, the thread hands
 * the reply over and other packets still reach the callback; one request per
 * NN at a time (RAMON_EL_BUSY otherwise). Without one, the caller's thread
 * receives, dropping earlier and non-matching packets. The reply (header +
 * payload + footer) is copied into reply; rx points into it.
 * Fails with RAMON_EL_SPW_REPLY_TIMEOUT if nothing matched in time
 * (timeout_ms 0 = RAMON_SPW_REPLY_MS).
 */
int ramon_spw_request(ramon_ctx *c, uint32_t nn, const struct ramon_spw_msg *m,
		      const struct ramon_iov *iov, unsigned n_iov,
		      const struct ramon_spw_match *want, void *reply, uint32_t cap,
		      uint32_t timeout_ms, struct ramon_spw_rx *rx, struct ramon_status *st);

/* NN services; dst is cfg.nn_node, timeout_ms 0 = RAMON_SPW_REPLY_MS */
struct ramon_nn_version {
	char kbl[201];		/* 4KBL */
	char rsbl[201];
	char image[201];
	char fw[21];
};
int ramon_spw_nn_version(ramon_ctx *c, uint32_t nn, struct ramon_nn_version *v, uint32_t timeout_ms,
			 struct ramon_status *st);
/* DPS request; the NN must answer with attribute 0x101 */
int ramon_spw_nn_dps(ramon_ctx *c, uint32_t nn, uint32_t timeout_ms, struct ramon_status *st);
/* JSON configuration, at most 4096 bytes; answer 0x101 = ACK, anything else RAMON_EL_SPW_NACK */
int ramon_spw_nn_config_push(ramon_ctx *c, uint32_t nn, const void *data, uint32_t len,
			     uint32_t timeout_ms, struct ramon_status *st);

enum ramon_swup_phase {
	RAMON_SWUP_START = 0,
	RAMON_SWUP_DATA  = 1,
	RAMON_SWUP_END   = 2,
};
typedef void (*ramon_progress_fn)(void *user, uint32_t done, uint32_t total);
/*
 * NN software update: start packet, 4096-byte data packets, end packet; each
 * waits up to 3 s for the NN's answer. On a non-zero answer: RAMON_EL_SPW_NACK
 * with st->arg[0] = status, st->arg[1] = phase << 16 | packet index.
 */
int ramon_spw_nn_sw_update(ramon_ctx *c, uint32_t nn, uint8_t sw_type, const void *image, size_t len,
			   ramon_progress_fn progress, void *user, struct ramon_status *st);
/* text for an answer status of the given phase, e.g. "bad package CRC" */
const char *ramon_spw_swup_status_str(int phase, uint32_t status);

struct ramon_spw_stats {
	uint64_t tx_packets;
	uint64_t tx_bytes;
	uint64_t tx_failures;
	uint64_t rx_packets;		/* consumed and parsed */
	uint64_t rx_bytes;
	uint64_t rx_crc_errors;
	uint64_t rx_truncated;
	uint64_t rx_not_for_us;
	uint64_t rx_footer_errors;
	uint64_t rx_oversize;		/* larger than the RX buffer (a temporary buffer was used) */
	uint64_t rx_consume_failures;	/* the RX transfer after a size failed: that packet is lost */
	uint64_t rx_dropped;		/* no callback, or not what a mode-B request waited for */
	uint64_t rx_wait_errors;
	uint64_t rx_to_request;		/* handed to a waiting ramon_spw_request() */
};
int ramon_spw_get_stats(ramon_ctx *c, uint32_t nn, struct ramon_spw_stats *s, int reset);

#ifdef __cplusplus
}
#endif

#endif /* RAMON_SPW_H */
