# libramon

libramon is the user-space API of the ramon board's DMA driver, `ramon_dma`
(`/dev/ramon_dma`). It is a static C library (`libramon.a`) with plain C headers
that work from C and C++. It replaces the old `spw_api`, `spfi_api`,
`axidmasgapi` and `ps2psapi` sources.

It has three layers:

| Header | What it covers |
|---|---|
| `ramon.h` | Opening the device, DMA buffers, AXI and ZDMA transfers, register windows, SYSMON, driver counters, error reporting |
| `ramon_spw.h` | SpaceWire packets: link sync, loopback, send, receive (callback thread or blocking), request/reply, NN services |
| `ramon_spfi.h` | SPFI storage streams: commands, stream table, page writes and reads, alerts, bring-up |

## Using it in a PetaLinux recipe

```bitbake
DEPENDS += "ramon-api"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -o myapp myapp.c -lramon -lpthread
}
```

In the source, include the headers as `<ramon/ramon.h>`, `<ramon/ramon_spw.h>`
and `<ramon/ramon_spfi.h>`. They are installed to `${includedir}/ramon/`, next
to the driver's `ramon_dma_uapi.h`. The binary needs no library at run time.

## A first program

```c
#include <stdio.h>
#include <ramon/ramon.h>
#include <ramon/ramon_spw.h>

static void on_packet(void *user, const struct ramon_spw_rx *rx)
{
	/* runs on the library's RX thread; copy what you keep, return quickly */
	printf("node %u sent %u bytes, attribute 0x%x\n", rx->hdr->src, rx->payload_len,
	       rx->hdr->attribute_id);
}

int main(void)
{
	struct ramon_nn_version v;
	struct ramon_status st;
	char msg[256];
	ramon_ctx *c;

	if (ramon_open(NULL, &c, &st))
		goto fail;
	if (ramon_spw_init(c, NULL, &st) || ramon_spw_sync(c, 0, NULL, &st))
		goto fail;
	if (ramon_spw_nn_version(c, 0, &v, 0, &st))
		goto fail;
	printf("NN image: %s\n", v.image);
	if (ramon_spw_rx_start(c, 0, on_packet, NULL, &st))
		goto fail;
	/* ... ramon_spw_send(), ramon_spfi_*() ... */
	ramon_close(c);		/* stops the RX thread */
	return 0;
fail:
	ramon_strerror(&st, msg, sizeof(msg));
	fprintf(stderr, "%s\n", msg);
	ramon_close(c);
	return 1;
}
```

## Conventions

- **Return values.** Every function returns 0 or a negative errno.
- **Status.** The last argument, `struct ramon_status *st`, may be NULL. When
  given, it holds the driver's explanation of a failure, such as
  `RAMON_E_BUF_RANGE: buf 3: offset 0x10000 + len 0x8000 > size 0x10000`, or a
  library code `RAMON_EL_*` for problems found before the driver was called.
  `ramon_strerror()` turns it into one line. There is no global "last error",
  so it is safe with threads.
- **Timeouts.** A `timeout_ms` of 0 means the driver default. The driver caps
  every timeout at 60 s.
- **Threads.** One `ramon_ctx` may be used from any number of threads. A
  `ramon_buf` is not locked: do not free it while another thread uses it.
  `ramon_spw_fini()`, `ramon_spfi_fini()` and `ramon_close()` must not run while
  other threads still use the context.

## Rules the hardware imposes

- DMA buffers belong to the context that allocated them. All transfers must use
  buffers of the same context. Buffers are physically contiguous and most
  likely uncached, so CPU access to them is slow; keep large CPU work in
  ordinary memory.
- `ramon_spw_rx_stop()` and `ramon_spfi_alert_stop()` use the driver's cancel
  ioctls. A cancel releases every waiter on that NN, in every process.
- `ramon_get_stats(c, 1, ...)` resets the driver counters for every process.
- Changing SPW loopback resets the link. After loopback is switched off, the
  link is re-synced before the call returns.
- Only one SPW NN is cabled on the ramon board: expect the other link to stay
  unsynced. SPFI NN1 is not wired, and the driver keeps it disabled.
- SPFI streams are non-cyclic. Stream ids 193 to 197 are reserved, at most 8
  streams may be open, and a stream must be deleted before its id is opened
  again. `ramon_spfi_stream_open()` checks all of this.
- An SPFI command the NN rejects still completes the ioctl. The per-command
  functions report it as `RAMON_EL_SPFI_NN_ERROR`, with the NN's code in
  `st->arg[0]`. The exception is `ramon_spfi_init_nn()`: on this board INIT type
  0 answers `rx_err_code 1, init_info 0x2`, which is normal.
- FORMAT (`ramon_spfi_format`, or `ramon_spfi_prep` with `format`) erases every
  stream on the NN.

## Receiving SpaceWire packets

There are two ways to receive, and only one may be active on an NN at a time:

1. **RX thread.** `ramon_spw_rx_start()` starts a library thread that delivers
   every packet to your callback. A `ramon_spw_request()` made meanwhile gets
   its reply handed over by that thread, while other packets keep reaching the
   callback.
2. **Your own thread.** Call `ramon_spw_recv()` or `ramon_spw_request()` with no
   RX thread running. Packets that arrive before the one you wait for are
   dropped.

CRC, length, address and footer problems are reported in `rx->flags`; they are
not errors. `ramon_spw_request()` only accepts clean replies addressed to your
node, unless you configure `RAMON_SPW_NO_CRC_CHECK`.

## Mapping from the old API

| Old | libramon |
|---|---|
| `axidmasg_init()` / `axidmasg_term()` | `ramon_open()` / `ramon_close()` |
| `axidmasg_alloc()`, `ps2ps_alloc()` (physical address) | `ramon_buf_alloc()` (handle and mapping; no physical addresses) |
| `axidmasg_transfer()` | `ramon_axi_xfer()`, `ramon_axi_xfer1()` |
| `ps2ps_copy()`, `ps2ps_copy_list()` | `ramon_zdma_copy1()`, `ramon_zdma_copy()` |
| `spw_init()` | `ramon_spw_init()` + `ramon_spw_sync()` per NN |
| `spw_open()` + callback | `ramon_spw_rx_start()` (one callback per NN; it sees every packet and can filter on `app_type`) |
| `spw_send()` | `ramon_spw_send()` (the payload is a gather list) |
| `spw_enable_loopback()` | `ramon_spw_loopback()` (per NN) |
| `get_nn_version()` | `ramon_spw_nn_version()` (returns the four strings) |
| `nnupdate` / `nnconf` in the old CLI | `ramon_spw_nn_sw_update()`, `ramon_spw_nn_config_push()` |
| `spw_reg_io()`, `spfi_reg_io()`, `axidmasg_rs_top_reg()`, `axidmasg_reg_access_io*()` | `ramon_reg_read()` / `ramon_reg_write()` on window `spw<n>`, `spfi<n>`, `rs_top` or a reg-access device name |
| `axidmasg_get_sysmon()` | `ramon_sysmon_read()` |
| `spfi_init_driver()` | `ramon_spfi_init()` (optional; defaults are used otherwise) |
| `spfi_open_stream()` | `ramon_spfi_stream_open()` (checks the NN rules first) or `ramon_spfi_open()` |
| `spfi_send()` (physical addresses, 16 KiB entries) | `ramon_spfi_stream_append()` / `ramon_spfi_write_buf()` / `ramon_spfi_write_mem()` |
| `spfi_receive()` (physical destination) | `ramon_spfi_read_seq()` / `ramon_spfi_read()` / `ramon_spfi_read_mem()` |
| `spfi_get_stream_infos()` | `ramon_spfi_get_table()` |
| `spfi_flush()`, `spfi_close_stream()`, `spfi_delete_stream()`, `spfi_set_tod()`, `spfi_format()`, `spfi_init()`, `spfi_graceful_power_down()` | `ramon_spfi_flush()`, `_close()`, `_delete()`, `_set_tod()`, `_format()`, `_init_nn()`, `_power_down()` |
| `spfi_register_alert()` | `ramon_spfi_alert_start()` (the alert is passed by pointer; nothing to free) |
| `spfi_statistics()`, `spw_get_statistics()` | `ramon_get_stats()`, `ramon_spw_get_stats()` |

## Building on a PC

`make && make check` builds the library for the host and runs `ramon_selftest`.
That covers the CRC, packet build and parse, table helpers and error
formatting; nothing in it needs the device.

## Versions

`RAMON_API_VERSION` in `ramon_version.h` equals the recipe's `PV`.
`ramon_open()` refuses a driver with a different ABI version, and it warns when
the driver is older than `RAMON_API_DRIVER_MIN`. `ramon_version_line()` prints
both versions.
