# ramon_cli and ramon_test

Two programs built on libramon. `ramon_cli` replaces the old `dmaapi_testv2`
shell. `ramon_test` runs the board tests automatically and reports PASSED or
FAILED. The driver-level negative tests (`--errors`, `unbind`, `kill`, CMA
exhaustion) stay in `ramon_smoke`.

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
| SpaceWire | `spwinit`, `spwsync`, `loopback`, `spwstatus`, `spwreg`, `spwrx`, `spwdump`, `log`, `spwsend`, `spwrecv`, `spwdrain`, `spwloop`, `axiloop`, `spwdps`, `nnswver`, `nnupdate`, `nnconf`, `fdir`, `fdirauto`, `spwnode`, `spwterm` |
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
| `reg-rtc-ttc` | the RTC advances, TTCs are readable |
| `reg-spw-spfi` | SPW/SPFI version registers agree by name and by index |
| `reg-ro` | a read-only window refuses a write with the right error |
| `zdma` | copies, a 100-entry list, MiB/s, 4 threads |
| `stats` | driver counters follow the traffic; reset |
| `spw-sync` | at least one SPW link syncs |
| `spw-loop` | loopback write-read: payloads of 0 bytes to `--spw-max`, a 32-packet burst in order with no overrun, round-trip MiB/s |
| `spw-version` | NN version request, received in the caller's thread and through the RX thread |
| `spw-dps` | DPS answered with 0x101 |
| `spw-cancel` | RX thread stop time, one receiver per NN, request timeout |
| `axi-loop` | raw AXI scatter-gather TX, loopback, RX, compared, MiB/s (the old `xsgdma`) |
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

## Building on a PC

```sh
make -C ../../ramon-api/files      # libramon.a first
make                               # ramon_cli and ramon_test (they need /dev/ramon_dma to run)
```
