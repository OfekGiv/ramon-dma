# ramon_cli, ramon_test and ramon_accept

Three programs built on libramon. `ramon_cli` replaces the old `dmaapi_testv2`
shell. `ramon_test` runs the board tests automatically and reports PASSED or
FAILED. `ramon_accept` is the board acceptance test: the old manual delivery
script as steps with a verdict each. The driver-level negative tests
(`--errors`, `unbind`, `kill`, CMA exhaustion) stay in `ramon_smoke`.

## ramon_cli

```sh
ramon_cli                              # interactive: TAB completion, hints, history
ramon_cli sysmon                       # one command; the exit status tells ok (0) or failed (1)
ramon_cli -f bringup.rcli              # a script, one command per line, # comments
echo "spfiginfo" | ramon_cli           # stdin works as a script too
ramon_cli --plain                      # interactive without line editing (dumb serial terminals)
```

Differences from dmaapi_testv2:

- **Numbers** are decimal unless written `0x..` (hex) or `0..` (octal). The old
  shell read every number as hex.
- **Strings** such as file names need no quotes.
- **Every command prints `ok: <name>` or `FAIL: <name>: <reason>`.** Scripts stop
  at the first failure unless you pass `-k`.
- **Destructive commands need a final `yes`:** `spfifmt yes`, `spfipdown yes`,
  `nnupdate <file> yes`, `fwupdate <file> yes`, `spfiprep 0 format yes`.
- **Ctrl-C** interrupts a command that waits (SPW receive, SPFI alert). Two
  Ctrl-C within a second exit.
- **Prefixes** work, as before: `rdrst` runs `rdrstopver`.
- The history is kept in `~/.ramon_cli_history`.

`help` lists every command, and `help <command>` shows one. The main groups:

| Group | Commands |
|---|---|
| Device | `version`, `info`, `stats [reset]`, `chan`, `reopen` |
| Buffers | `alloc`, `free`, `bufinfo`, `fill`, `verify`, `dump`, `zdma`, `zdmalist`, `axiraw` |
| Registers | `regio`, `regwins`, `rstopregio`, `regaccessio`, `regaccessioname`, `rdrstopver`, `readtimetag`, `sysmon`, `rtcttc` |
| SpaceWire | `spwinit`, `spwsync`, `loopback`, `spwstatus`, `spwreg`, `spwrx`, `spwdump`, `log`, `spwsend`, `spwrecv`, `spwdrain`, `spwloop`, `axiloop`, `loopdiag`, `spwdps`, `nnswver`, `nnupdate`, `nnconf`, `fdir`, `fdirauto`, `spwnode`, `spwterm` |
| SPFI (current NN, `spfinnid`) | `spfiprep`, `spfiinfo`, `spfiinit`, `spfifmt`, `spfiginfo`, `spfiopens`, `spfigsend`, `spfircv`, `spfiflush`, `spficlose`, `spfidel`, `spfistod`, `spfipdown`, `spfireg`, `spfiloop`, `spfitest`, `spfirxtx`, `spfitxoffs`, `spfialerts`, `spfialert`, `spfidrvinit`, `spfidrvterm` |
| Board extras | `rdlatver`, `wrlatreg`, `rdlatreg`, `fwupdate`, `scrubtest`, `meminfo`, `devtree` |

Old names that still work: `getdrvver`, `xinfo`, `dma`, `xsgdma`, `spwtest`,
`alimentest`, `exit`, `exitapp`.

Commands that changed:

- **`spwappinit <app> <node>`** is now `spwnode <node>`. One receive callback
  per NN sees every packet; the app id had no effect in the old code either.
- **`spwtest`** hung in the old tool. It is now `spwloop <nn> <len> [count]`, a
  verified loopback round trip.
- **`lalloc`/`lfree`/`ldma`** are now `zdmalist <n> <size> [count]`.
- **`spfiginfo`** reads the NN's table every time; there is no shadow table to
  reinitialise.
- **`spfitest` and `spfirxtx`** create their streams (or take `auto` ids) and
  delete them afterwards.
- **`initsg`/`term`** are gone: the device opens on the first command that needs it.

A typical session:

```text
ramon> spwinit                   # sync the SPW links
ramon> spfiprep 0                # SPFI link check, DPS, INIT, stream table
ramon> spfitest auto 64 8        # 8 x 64-page writes and reads, verified, MiB/s
ramon> loopback 0 1
ramon> spwrx 0 1                 # RX thread; spwdump 1 checks the spwsend pattern
ramon> spwdump 1
ramon> spwsend 1024 100 4        # 4 threads x 100 packets to our own node
ramon> spwstatus 0               # "rx packet(400) errors(0)"
ramon> spwrx 0 0
ramon> loopback 0 0
```

## ramon_test

```sh
ramon_test                        # every default test
ramon_test --prep                 # SPFI bring-up first (after power-up)
ramon_test spw-loop spfi-perf     # only these
ramon_test --long                 # also spfi-maxlist (2 x 5000-page writes)
ramon_test --minutes 10           # also the 10-minute soak
ramon_test --json > run.json      # plus one JSON line with every result and metric
ramon_test --list                 # the tests
```

Tests whose prerequisite is missing, such as a synced SPW link or an SPFI link
that is up, print `SKIP` and do not fail the run unless you pass `--strict`.
The exit status is 0 for PASSED, 1 for FAILED and 2 for a usage or device
error.

| Test | Checks |
|---|---|
| `version` | ABI, driver version against sysfs, GET_INFO |
| `buffers` | allocation sizes, zeroed memory, pattern, BUF_INFO, CMA returned |
| `cpu-bw` | CPU speed on DMA buffers and on ordinary memory (information only) |
| `chan` | channel list, SPW RX/TX pairs, SPFI write channels |
| `regwin` | register window names, order and flags |
| `reg-rstop` | RS-TOP by name and index, 0x40 read-back, time stamp |
| `reg-sysmon` | temperatures within -40..125 C, all 37 rails |
| `reg-rtc-ttc` | the RTC is readable (a stopped RTC is only noted, as in `ramon_smoke`), TTCs are readable |
| `reg-spw-spfi` | SPW/SPFI version registers agree by name and by index |
| `reg-ro` | a read-only window refuses a write with the right error |
| `zdma` | copies, a 100-entry list, MiB/s, 4 threads |
| `stats` | driver counters follow the traffic; reset |
| `spw-sync` | at least one SPW link syncs |
| `spw-loop` | loopback write-read: payloads of 0 bytes to `--spw-max` (default 4096), a 32-packet burst in order with no overrun, round-trip MiB/s; no packet is larger than `--spw-max` |
| `spw-version` | NN version request, received in the caller's thread and through the RX thread |
| `spw-dps` | DPS answered with 0x101 |
| `spw-cancel` | RX thread stop time, one receiver per NN, request timeout |
| `axi-loop` | raw AXI scatter-gather TX, loopback, RX, compared, MiB/s (the old `xsgdma`); one packet of `--spw-max` payload bytes |
| `spfi-link` | link state (0xCC), stream table |
| `spfi-stream` | open, write, flush, read back, verify, CRC, delete |
| `spfi-perf` | write and read MiB/s for 1, 8 and `--spfi-pages` pages, each read verified; data is filled and checked outside the timed part |
| `spfi-rxtx` | one stream written while another is read |
| `spfi-alerts` | pending alerts, idle timeout, alert thread stop |
| `spfi-rules` | reserved ids, reopening, bad sizes refused before any NN traffic |
| `spfi-maxlist` (`--long`) | two 5000-item writes back to back, read back |
| `soak` (`--minutes`) | ZDMA, buffer churn, registers, counters, NN version requests and SPFI reads in parallel threads |

The SPW tests switch FPGA loopback on and off on the synced NN and restore the
previous setting. The SPFI tests use free stream ids and delete what they
create. The soak test writes one small stream and then only reads, to spare the
NN's storage.

## ramon_accept

```sh
ramon_accept                              # every step
ramon_accept --rstop-version 3.2.1        # also check the RS-TOP version of the delivery
ramon_accept sysmon ddr4                  # only these
ramon_accept --ddr4-mb 256 ddr4           # a quick DDR4 check on the first 256 MiB
ramon_accept spw-link spw-nn-loop spw-fpga-loop
ramon_accept --spw-seconds 60 spw-nn-loop # SPW for a minute per size
ramon_accept --spfi-format spfi-link spfi-prep spfi-stream   # the SPFI delivery flow
ramon_accept --spfi-scale 10 spfi-perf spfi-large spfi-odd spfi-rxtx   # SPFI at 10% of the runs
ramon_accept --spfi-long --spfi-soak spfi-long spfi-soak     # the long SPFI runs (about 45 min)
ramon_accept --nn-expect 4.4.0.1 nn-version
ramon_accept --nn-config 0=config_2SPF_4GBps_DPC.json nn-config
ramon_accept --nn-update 0=RS3_4.4.0.1.pkg nn-update   # then power-cycle
ramon_accept | tee accept-$(date +%F).log # keep the record
ramon_accept --list                       # the steps
```

The output starts with the tool, library and driver versions, the host, the
kernel and the date. Each step prints its readings, then `ok:` or `FAIL:`. A
summary with one line per step ends the run, then `PASSED` or `FAILED`. The
exit status is 0 for PASSED, 1 for FAILED and 2 for a usage error. A device
that does not open fails only the steps that need it.

| Step | Was | Checks |
|---|---|---|
| `rstop-version` | `rdrstopver` | RS-TOP version (0x00, 0x04, 0x08) and configuration (0x0C) read; not 0.0.0, no register reads 0xffffffff; equal to `--rstop-version` if given |
| `rstop-timetag` | `readtimetag` | RS-TOP time tag (0x10) decodes to a valid date and time; sampled twice 1.1 s apart, and reported as fixed or counting |
| `sysmon` | `sysmon` | PS and PL temperatures within limits (default -40..125 C); all 37 rails printed, and checked against the limits file |
| `ddr4` | `ddr4test` | PL DDR4: data bus (walking ones and zeros), address bus (aliasing at every power-of-two offset), then the whole region written and read back with its own addresses and with their complement |
| `spw-link` | `spwinit` | every SPW link that should be tested syncs; IP versions printed |
| `spw-nn-loop` | `loopback 0`, `spwsend ... +68 100` | FPGA loopback off: packets to our own node (68) go out, the NN routes them back; every packet comes back once, intact and in order; packets/s and MiB/s |
| `spw-fpga-loop` | `loopback 1`, `spwsend` | the same traffic through the FPGA loopback |
| `spfi-link` | `spfidrvinit`, `spfireg +204` | SPFI link up: low byte of `0xCC` is `0x88`; IP versions printed |
| `spfi-prep` | `spwdps 0`, `spfiinit 0`, `spfifmt`, `spfiginfo 1` | SPW DPS answered, INIT answered, FORMAT (with `--spfi-format`), stream table read, room for the tests (2 free ids, 2 more streams can open) |
| `spfi-stream` | stream #2: `spfitest +2 +150 +1000 1` | 150 pages x 1000 written, flushed, read back, every page verified |
| `spfi-perf` | streams #3, #5 (`verify 0`) | 150 x 1000 and 5000 x 100: write and read MiB/s |
| `spfi-large` | stream #4 | 5000 pages x 100, verified |
| `spfi-odd` | streams #10, #11, #12 | 257, 4999 and 1 page x 100, verified |
| `spfi-rxtx` | `spfirxtx`, streams #17-#20 | 100 x 5000 pages written while 80 x 5000 are read, and the reverse; reads verified, MiB/s |
| `spfi-long` | stream #13 | 5000 x 200, throughput (current measurements); only with `--spfi-long` |
| `spfi-soak` | `spfirxtx`, streams #21-#24 | the two 20-minute concurrent runs (750 and 7000 cycles), reads verified, cycles over 1 s counted; only with `--spfi-soak` |
| `mixed-write` | test #1 (new) | 3 SPW users (a message every 1 s, 1 s, 0.1 s) and 3 SPFI writers (12 pages every 1 s, 2 every 1 s, 4096 every 0.1 s) at once: every message back intact, no error, no missed deadline |
| `mixed-rw` | test #2 (new) | the same SPW users, SPFI writers of 12, 2 and 2048 pages, and a reader of 2048 pages every 0.1 s: also every page read intact |
| `nn-version` | `nnswver` | each tested NN answers with its 4KBL, RSBL, image and FW versions; the image version contains `--nn-expect` if given |
| `nn-config` | `nnconf` | the JSON configuration (at most 4096 bytes) is acknowledged; only with `--nn-config` |
| `nn-update` | `nnupdate` | the NN accepts every packet of the software package; only with `--nn-update`; power-cycle afterwards |

**SYSMON limits.** A rail is checked only if the limits file names it. The
file is `/etc/ramon/accept_limits.conf` if it exists, or `--limits FILE`. Each
line is `name min max`; names are `temp_ps`, `temp_pl` and the `ps.<rail>` /
`pl.<rail>` names the step prints. `accept_limits.example` (installed to
`/usr/share/ramon-tools/`) is a starting point: its voltage lines are
commented out until they are checked against the fitted part and the board
schematic.

**PL DDR4.** The step finds the region through the device-tree label
`ddr4_0` (as the old test did), or else the one label containing `ddr4`.
`--ddr4-label` picks another label; `--ddr4-base` with `--ddr4-size` skips the
device tree. It needs root and `/dev/mem`: a kernel without `CONFIG_DEVMEM`,
or with `CONFIG_IO_STRICT_DEVMEM` claiming the range, makes the step fail with
that reason. It overwrites the tested range. It refuses to run when
`/sys/class/fpga_manager/fpga0/state` is not `operating`, because an access to
an unprogrammed PL can hang the CPU or end in a kernel panic. The full test
writes and reads the region twice; at the uncached speed of `/dev/mem` this
takes minutes for 4 GiB, so the step prints progress every GiB.

**SpaceWire.** The loop steps send, per payload size (`--spw-sizes`,
default `1,4096`), `--spw-count` packets (default 10000, as the delivery
script) or packets for `--spw-seconds`, with application type `--spw-app`
(default 100) to `--spw-node` (default 68). At most `--spw-window` packets
(default 32) are in flight, so the driver's 64-entry RX queue cannot overrun.
Each packet that comes back is checked: header and payload CRC, length,
application type, packet id in sequence, payload pattern, FPGA footer. A size
fails on any lost, bad, repeated or out-of-order packet, or a driver RX
overrun. Packets of an earlier size that arrive late are reported but not
counted against the current one. Throughput is payload MiB/s from the first
send to the last packet back; the figure with headers adds the 40-byte header.
Sizes above 4096 bytes, the FPGA's SPW maximum, are refused (`--spw-sizes`,
`ramon_test --spw-max`, `ramon_cli spwsend`/`axiloop`); a received packet above
it is dropped with a warning, and `ramon_cli spwstatus` counts both.
NNs: every NN whose link syncs, or those named with `--spw-nn 0,1` (an NN
named there whose link is down fails the step; with `--spw-nn` only those NNs
are synced). A link is synced as the old `spw_init()` did it: while
`(0x28 & 7) != 5`, write 0 to `0x1C` and wait 10 ms, up to `--spw-sync-tries`
times (default 1000, 10 s). The loop steps restore the FPGA loopback setting
they found.

**SPFI.** A page (the old "offset") is 16 KiB. Every SPFI test opens its own
streams on free ids and deletes them afterwards, so tests do not depend on each
other or on the NN's old streams; the NN allows at most 8 open streams.
Run `spfi-prep --spfi-format` first after power-up: the NN answers stream
commands only after the bring-up (SPW DPS, INIT) with FORMAT, and the other
SPFI and mixed steps do not do it themselves. FORMAT erases every stream on the
NN, as `spfifmt` in the delivery script. Verified tests stamp every page with its
stream, run and page number, so a read that returns another run's or another
page's data fails, not only corrupted data. `--spfi-scale PCT` runs PCT% of
the runs and cycles (for a single-cycle concurrent test, of its writes and
reads) for a quick check. The concurrent tests report each cycle's time against
the delivery script's 1 s; cycles over 1 s are counted, not failed. NNs: every
NN whose SPFI link is up, or `--spfi-nn` (SPFI1 is masked by the driver).

**Mixed load.** Every user of `mixed-write` and `mixed-rw` is a thread on a
fixed schedule from a common start, for `--mixed-seconds` (default 60). An
operation still running when its next slot comes is a missed deadline; missed
slots are skipped, not caught up. Missed deadlines fail the step unless
`--mixed-late-ok`. SPW messages (`--mixed-spw-bytes`, default 4096) go to our
own node and the NN sends them back; each is checked. The reader checks every
page's stamp and one whole page per read, and its last read completely. Each
user prints operations done, deadlines missed, average and longest operation
time and its throughput. Test #1's 4096-page writer alone needs 640 MiB/s.
For long runs, `--mixed-report-minutes N` (default 1, 0 = off) prints a line
per user every N minutes while the test runs: totals so far and, for that
interval, the change, the longest operation and the throughput; only reports
before the end are printed, so a 60-second run shows none.

**NN services.** `--nn-config` and `--nn-update` take `FILE` (every tested
NN) or `NN=FILE` (that NN; it overrides `FILE`), and can be repeated, so
different NNs can get different files as in the delivery script. A file given
for an NN whose link is down fails the step. These two steps change the NN, so
they run only when their option is given, also in a run without step names.
`nn-update` runs last: the board needs a power cycle afterwards, then
`ramon_accept nn-version --nn-expect <new version>` confirms the update.
`--nn-sw-type` sets the software type of the update (default 0, as the old
tool).

## Building on a PC

```sh
make -C ../../ramon-api/files      # libramon.a first
make                               # ramon_cli, ramon_test, ramon_accept (they need /dev/ramon_dma to run)
```
