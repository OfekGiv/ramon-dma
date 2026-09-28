# ramon_dma

One Linux kernel module for the ramon ZynqMP board. It replaces the old `axidmasgk` and
`ps2psk` modules:

- **AXI DMA:** scatter-gather transfers on the PL `axi_dma` channels.
- **ZDMA:** memory-to-memory copies on the PS `zynqmp_dma` channels.
- **SpaceWire (SPW):** RX interrupts and packet sizes.
- **SPFI:** NN commands, stream write/read and alerts.
- **Register windows:** RS-TOP, SYSMON, RTC, TTC, SPW, SPFI and `reg-access` devices.

Everything goes through one device node, `/dev/ramon_dma`, which many threads can use at
once through a single fd. The userspace ABI is one header: `ramon-dma/files/ramon_dma_uapi.h`.

Target: PetaLinux 2023.x, Linux 6.1, aarch64. The device tree is the old drivers' one,
unchanged.

## Repository layout

| Path | Contents |
|---|---|
| `ramon-dma/ramon-dma.bb`, `ramon-dma/files/` | module recipe and sources; `ramon_dma_uapi.h` is the ABI |
| `ramon-smoke/ramon-smoke.bb`, `ramon-smoke/files/ramon_smoke.c` | on-target test tool |
| `ramon-api/ramon-api.bb`, `ramon-api/files/` | libramon: the static C library customers link (`README.md` there) |
| `ramon-tools/ramon-tools.bb`, `ramon-tools/files/` | `ramon_cli` (successor of `dmaapi_testv2`) and `ramon_test` (automatic tests) |
| `linux-xlnx/ramon_dma.ksyms` | export whitelist for kernels built with `CONFIG_TRIM_UNUSED_KSYMS` |
| `BUILD_NOTES.md` | per-step build/test history, board results, open questions |

Source files: `ramon_core.c` (probe/remove, `/dev/ramon_dma`, ioctl dispatch, `GET_INFO`,
`GET_STATS`), `ramon_of.c` (DT discovery), `ramon_buf.c` (buffers, mmap), `ramon_regwin.c`,
`ramon_axidma.c`, `ramon_zdma.c`, `ramon_spw.c`, `ramon_spfi.c`. Internal objects and the
locking rules are described at the top of `ramon_dma.h`; every hardware constant is in
`ramon_board.h`.

## Building

In the PetaLinux project:

```sh
cp -r ramon-dma   <proj>/project-spec/meta-user/recipes-modules/
cp -r ramon-smoke <proj>/project-spec/meta-user/recipes-apps/
printf 'CONFIG_ramon-dma\nCONFIG_ramon-smoke\n' >> <proj>/project-spec/meta-user/conf/user-rootfsconfig
petalinux-config -c rootfs     # enable ramon-dma and ramon-smoke; disable axidmasgk and ps2psk
petalinux-build -c ramon-dma && petalinux-build -c ramon-smoke
```

The userspace library and tools are two more recipes:

```sh
cp -r ramon-api ramon-tools <proj>/project-spec/meta-user/recipes-apps/
printf 'CONFIG_ramon-api\nCONFIG_ramon-tools\n' >> <proj>/project-spec/meta-user/conf/user-rootfsconfig
petalinux-config -c rootfs     # enable ramon-tools
petalinux-build -c ramon-api && petalinux-build -c ramon-tools
```

- The module recipe autoloads `ramon_dma` and installs `ramon_dma_uapi.h` to
  `${includedir}/ramon/` for userspace recipes (`DEPENDS = "ramon-dma"`,
  `-I${STAGING_INCDIR}/ramon`).
- **Hardened kernel** (`CONFIG_TRIM_UNUSED_KSYMS=y`): add `linux-xlnx/ramon_dma.ksyms` as
  `CONFIG_UNUSED_KSYMS_WHITELIST` and rebuild the kernel; see `BUILD_NOTES.md`, "Kernel
  prerequisite".
- Out-of-tree for a quick check: `make -C ramon-dma/files KERNEL_SRC=<kernel-build-artifacts>`.

**Never load `ramon_dma` together with the old `axidmasg_drv`** (both bind the same DT node) or
with `ps2psk` (it holds the ZDMA channels).

## Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `max_buf_mb` | 256 | largest single buffer (`BUF_ALLOC`), MiB, 1..4096 |
| `zdma_channels` | 4 | `DMA_MEMCPY` channels taken for `ZDMA_COPY`, 0..16 |
| `spfi_mask` | `0x1` | SPFI NNs to use (bit *n* = NN *n*). Only NN0 is wired on this board; a masked NN is never touched |

All are read-only after load.

## Device tree (unchanged)

| What | Found by |
|---|---|
| AXI DMA channels | own node `xlnx,nanrec-axidmasg-1.0`: `dmas` / `dma-names`; **channel index = position in `dmas`**; direction from the child `xlnx,axi-dma-mm2s-channel` / `-s2mm-channel` |
| SPW NN A / NN B | `xlnx,nanrec-spw-wr-1.0` nodes in tree order: `reg[0]`, `interrupts[0]` |
| SPFI NN0 / NN1 | first `xlnx,nanrec-spfi-wr-1.0` node: `reg[n]`, `interrupts[n]` |
| SPFI RX message table / TX read-offset table | DT labels `vH_RS_nanrec_spfi_top_spfi_rx_msg_mem_ctrl_nn{a,b}` / `..._tx_rd_offset_ctrl_nn{a,b}` via `/__symbols__` |
| RS-TOP | `xlnx,nanrec-rs-top-1.0`; probe writes 1 to offset `0x40` (TRST/SPI_EN), as before |
| reg-access windows | `reg-access` node: `reg-devices` phandles (up to 8), names from `reg-device-names` |
| ZDMA | no DT: `DMA_MEMCPY` capability |

Board channel map (userspace decides roles; these are the measured ones):

| Use | AXI channel |
|---|---|
| SPW NN A RX / TX | 0 / 1 |
| SPW NN B RX / TX | 2 / 3 |
| SPFI NN0 data write (axi_dma @ `0xa0020000`) | **5** |
| SPFI NN1 data write (unused on this board) | 4 |

`dmesg | grep ramon_dma` after load lists every channel (name, direction, axi_dma address and
node), every register window, the SPW/SPFI interrupts and the SPFI tables.

## ABI in short

- **Commands:** every command is `ioctl(fd, RAMON_IOC_*, &struct)` with an `_IOWR` struct, and
  every struct ends in a `struct ramon_status` trailer. The driver copies the struct back even
  when the ioctl fails.
- **The trailer:** `code` (`RAMON_E_*`), `err` (the negative errno the ioctl returned), `arg[2]`
  (the two most relevant values) and `msg` (a sentence naming the offending values). On success
  `code` is 0, and `msg` may still carry a note (e.g. an SPFI `rx_err_code`). The trailer
  belongs to that call, so it is safe with many threads on one fd.
- **Error helpers:** `ramon_err_name()`, `ramon_err_desc()` and `ramon_err_errno()` in the
  header translate codes.
- **Buffers:** buffers are opaque handles owned by the fd. `BUF_ALLOC` returns a handle, and
  `mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, handle << PAGE_SHIFT)` maps it.
- **DMA operands:** every operand is `(handle, offset, len)`; physical addresses are never
  accepted.
- **Buffer lifetime:** a buffer lives until it is freed and no mapping or transfer uses it any
  more. Closing the fd (or the process dying) frees all of that fd's buffers.
- **Timeouts:** timeouts are ioctl fields in ms. 0 selects the default; values above 60000 are
  clamped, except `SPFI_WAIT_ALERT`, where 0 means "wait forever".
- **Unknown ioctls:** return `-ENOTTY`.

| # | ioctl | Purpose |
|---|---|---|
| 1 | `GET_INFO` | ABI and driver version, channel/window counts, SPW/SPFI masks, page size, max buffer |
| 2 | `CHAN_INFO` | channel by index: AXI first, then ZDMA |
| 3–5 | `BUF_ALLOC` / `BUF_FREE` / `BUF_INFO` | coherent buffers |
| 6 | `AXI_XFER` | synchronous SG transfer, 1..5000 items (default 2000 ms) |
| 7 | `ZDMA_COPY` | 1..4096 copies, validated up front, run in chunks of 16 (default 3000 ms per chunk) |
| 8–10 | `SPW_WAIT_RX` / `SPW_CANCEL` / `SPW_LOOPBACK` | per-NN RX sizes (queued, 64 deep), cancel, loopback |
| 11 | `SPFI_CMD` | short commands (all but DATA_WRITE / READ_STREAM) |
| 12 | `SPFI_WRITE` | DATA_WRITE + AXI transfer + write-done; items must total `tx_num_offset × 16 KiB` |
| 13 | `SPFI_READ` | offsets to the TX table + READ_STREAM into a buffer, 1..8192 offsets |
| 14–15 | `SPFI_WAIT_ALERT` / `SPFI_CANCEL_ALERT` | alerts (queued in the IRQ, 16 deep) |
| 16–17 | `SPFI_MEM_READ` / `SPFI_TX_OFFS_WRITE` | RX message table read / TX offset table write, any alignment |
| 18–19 | `REGWIN_INFO` / `REG_IO` | bounded 32-bit register access by window index or name |
| 20 | `GET_STATS` | counters (DMA, SPW, SPFI interrupt histogram, timeouts); optional reset |

**Error codes** are listed in the header (`RAMON_ERR_LIST`), grouped as generic 1–31 (9 and 10
retired), buffers 32+, AXI 64+, ZDMA 96+, SPW 128+, SPFI 160+ and register windows 192+. Each
code has a fixed errno. Argument errors are never logged; timeouts and DMA errors are logged
ratelimited.

**Register windows** have fixed indices (`enum ramon_win_index`):

- 0: `rs_top`
- 1–2: `sysmon_ps`, `sysmon_pl` (read-only)
- 3: `rtc` (read-only)
- 4–7: `ttc0`..`ttc3` (read-only)
- 8–9: `spw0@…`, `spw1@…`
- 10–11: `spfi0@…`, `spfi1@…`
- 12 and up: the `reg-access` devices by their DT names

A name also matches its part before `@` (`"spw0"`). Windows missing on the board are listed as
`ABSENT`, so indices never move.

## Using the NN

This comes from the old app and was confirmed on the board. The protocol itself (headers,
CRCs, stream tables) lives in userspace; the kernel only moves the bytes.

- **SPW bring-up:**
  1. Loopback off.
  2. Until `(reg 0x28 & 7) == 5`: write `0x1C`, wait 10 ms (at most 1000 times).
  3. Our node id is 68 and the NN is `0x41`.
- **SPW messages:** send with `AXI_XFER` on the TX channel. For the reply, `SPW_WAIT_RX`, then an
  `AXI_XFER` of **exactly** that size on the RX channel. Every popped size must be followed by
  its RX transfer, or the RX stream goes out of step. Messages:
  - DPS: protocol 9, application type 1, attribute `0x01`; the reply carries `0x101`.
  - Version: protocol 9, application type 9, attribute `0x98`.
- **SPFI bring-up:**
  1. The low byte of SPFI register `0xCC` must be `0x88` (e.g. `0x4488`).
  2. DPS over SPW.
  3. `INIT` type 0 (`rx_err_code 1`, `init_info 0x2` is normal).
  4. `FORMAT` (erases every stream).
  5. `GET_ALL_STREAM_STATUS` + `SPFI_MEM_READ` of the 64-byte header and 198 × 64-byte records.
- **SPFI streams:**
  - Non-cyclic only.
  - At most 8 open at a time.
  - Never use stream ids 193–197.
  - Delete a stream before opening its id again.
  - Take the first `tx_offset` from the NN table (`latest_write_offs + 1`) **once**, then count
    locally.
  - One offset is 16 KiB.
- **Word 50 (`0xC8`, SPFI write status):**
  - bit 5 = VC1 sticky no-TLAST (set on NN0 from boot; harmless);
  - bit 3 = VC1 full;
  - bit 1 = VC1 command overflow.

  After a stalled write, VC1 stays wedged until a reboot.

## Behaviour changes from the old drivers

- One device, one ABI (magic `'R'`), for everything formerly split between two modules and two
  hand-synced headers.
- Buffers belong to the fd and are freed with it; the old driver leaked them until rmmod.
  Physical addresses are never accepted from userspace.
- Unknown ioctls return `-ENOTTY`; the old driver returned 0 and echoed the buffer.
- `PLATFORM_RESET` (`0x58`) now actually writes the opcode; the old `case` was empty.
- SPFI write-done and read-done waits are per NN; the old driver had one global pair.
  Concurrent writes (or reads) on one NN queue on a mutex instead of corrupting state. Short
  commands on one NN are serialized across their wait.
- `SPW_LOOPBACK` takes the NN; the old driver ignored it and toggled both.
- SPW RX sizes are queued (64 deep). The old driver kept only the last one, losing packets that
  arrived while nobody waited.
- `SPFI_WAIT_ALERT` after `CANCEL_ALERT` returns `-ECANCELED`; the old driver returned fake
  success. Cancels release only waiters that started before them.
- AXI lists longer than the xilinx_dma descriptor pool (5000 on this kernel) fail with
  `RAMON_E_BAD_COUNT` instead of silently.
- A timed-out `SPFI_READ` keeps its destination buffer alive until a later read completes, since
  the FPGA may still write into it. The old driver let that memory be reused.
- DMA channels are halted before release, so the next owner never inherits a running engine.
  After a timeout resets an axi_dma IP, the IP's other channel is re-acquired so its interrupts
  work again.
- Register access is bounds- and alignment-checked; the old driver used user offsets unchecked.

## Testing

`ramon_smoke` runs the default sequence:

1. `info`, `buf`, `churn`, `kill` (driver and buffers);
2. `regwin`, `chan` (register windows and channels);
3. `spw`, `zdma`, `spfi` (the data paths);
4. `stats`.

More tests, run individually:
- `--errors`: provokes every reachable error code. It deliberately times out an AXI RX, so one
  `xilinx-vdma …: Cannot stop channel` line is expected.
- `--stress --minutes N`: everything concurrently.
- `unbind`: unbinds with waiters blocked and a buffer mapped, then rebinds.
- Explicit commands: `spwdps`, `spwsend`, `spfiprep`, `spfidel`, `spficmd`, `reg`.

`ramon_smoke --help` lists all of them with their options.

`ramon_test` (in `ramon-tools`) tests the same board through libramon: SPW loopback write-read
and NN round trips, SPFI stream write/read with throughput, register access, buffers, AXI and
ZDMA. `ramon_test --list` shows the tests. `ramon_cli` is the interactive command line.

## Open questions for the FPGA team

- Is SPW register `0x0C` (RX packet size) latched until the `0x14` ack?
- Is the `udelay(10)` before `READ_STREAM` required, or is a read-back of word 0 enough?
- Must the TX offset table be written in pairs? The driver writes a zero word after an odd
  count, as the old app did.
- Does CLOSE/FLUSH raise VC1TX? The driver keeps them from overlapping a DATA_WRITE.
- Can a READ_STREAM answer arrive after the driver's timeout?
- How are word 50's sticky bits cleared?
- Does the value written to `FPGA_SPW_RESET` (`0x1C`) matter? The old code wrote 0 in link sync
  and 1 after a loopback change.
- Channel 5 is SPFI NN0's write path; the DT still lists channel 4 and SPFI1, which are unused on
  this board.
