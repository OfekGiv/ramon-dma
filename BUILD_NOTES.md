# ramon_dma build notes

Nothing here has been compiled for the target. The development machine has no PetaLinux, no
aarch64 toolchain and no board. What *was* checked locally is listed per step under "checked
here"; everything else is for you to run.

Placeholders: `<proj>` = PetaLinux project, `<machine>` = its machine name,
`<kba>` = `<proj>/build/tmp/work-shared/<machine>/kernel-build-artifacts`,
`<ksrc>` = `<proj>/build/tmp/work-shared/<machine>/kernel-source`.

| Step | Content | State |
|---|---|---|
| 1 | UAPI, headers, core: probe/remove, miscdevice, dispatcher, GET_INFO | **done** on the non-hardened image (unbind verified with step 2's `ramon_smoke unbind`). Not yet reported: compile-log warnings, dmesg |
| 2 | buffers, mmap; remove gate changed from rwsem to SRCU | **done** on the non-hardened image: default, `--errors`, `unbind` pass (smoke 0.2.2). Not yet reported: dmesg after `unbind`, compile-log warnings |
| 3 | register windows, DT discovery, REGWIN_INFO / REG_IO | **done** on the non-hardened image (0.3.0: default, `--errors`, `unbind` pass) |
| 4 | AXI DMA, CHAN_INFO | **done** on the non-hardened image (0.6.0: `chan`, AXI `--errors` incl. RX timeout, `unbind` pass) |
| 5 | SPW: IRQ, WAIT_RX / CANCEL / LOOPBACK | **done** on the non-hardened image: `spw` (link sync, version round trip with NN) and `spwdps 0` pass; unsynced spw1 is expected |
| 6 | ZDMA pool, ZDMA_COPY; GET_STATS pulled forward from step 8 | **done** on the non-hardened image (0.6.0: `zdma`, `stats`, ZDMA `--errors` pass) |
| 7 | SPFI: IRQ, CMD, WRITE, READ, alerts, MEM_READ, TX_OFFS_WRITE | **done** on the non-hardened image with driver 0.8.5: `spfiprep 0 --format` and `spfi` pass (stream write on channel 5, flush, read back + verify, concurrent write/read, close/delete, alerts) |
| 8 | remove hardening, `--stress`, `--errors` sweep (GET_STATS came with batch A) | driver 0.8.6: `spw`, `spfiprep`/`spfi`, `--errors`, and `spw` again after `--errors` pass; `--stress` 10 min (10 threads) passes; `unbind` passes. **done** |
| 9 | README, final petalinux-build + autoload boot (recipes already exist) | `README.md` written; driver 0.8.8 passes default + `--errors` on target. Pending: full-image build + autoload boot, then version 1.0.0 |
| P2 | Userspace: libramon (`ramon-api`), `ramon_cli` and `ramon_test` (`ramon-tools`), 0.1.0 | **written**; host builds, self-test and emulator runs pass; not yet built with PetaLinux or run on the board. See "Phase 2" at the end |

The driver version is `0.<step>.0` while the rewrite is in progress, so
`cat /sys/module/ramon_dma/version` tells which step is on the board. Steps are now delivered in
batches to save file transfers (batch A = steps 4-6, version 0.6.0; batch B = steps 7-8; batch C = 9).

---

## Installing the recipes in the PetaLinux project (once)

This tree holds two recipes for the driver (and two more for the userspace, see "Phase 2" at the
end). Copy each recipe directory, including its `files/`, into `meta-user`:

```sh
cp -r ramon-dma   <proj>/project-spec/meta-user/recipes-modules/ramon-dma
cp -r ramon-smoke <proj>/project-spec/meta-user/recipes-apps/ramon-smoke

cat >> <proj>/project-spec/meta-user/conf/user-rootfsconfig <<'EOT'
CONFIG_ramon-dma
CONFIG_ramon-smoke
EOT
petalinux-config -c rootfs
#   user packages  ->  enable ramon-dma and ramon-smoke
#   modules / user packages -> DISABLE axidmasgk and ps2psk (the old axidmasg_drv binds the
#   same DT node, and both would autoload)
```

- `ramon-dma.bb` builds `ramon_dma.ko` (`inherit module`, `W=1`), signs it as the old recipe did,
  autoloads it, and installs `ramon_dma_uapi.h` to `${includedir}/ramon/`.
- `ramon-smoke.bb` has `DEPENDS = "ramon-dma"` and compiles against that installed header. That
  is the same path the phase-2 userspace library will use, so it also tests the header install.
- On each step I update `SRC_URI` and `PV` in both recipes. After copying a new step, re-copy
  both directories.
- Versions: the driver is `0.<step>.0`. The smoke tool is `0.<step>.<revision>`, bumped whenever
  only the tool changes, and `ramon-smoke.bb`'s `PV` always equals it. `ramon_smoke --version`
  prints it, and every run starts with a line showing both the tool and driver versions. Work
  directories follow `PV`, e.g. `.../ramon-smoke/0.2.2-r0/`.

If you would rather keep the old modules in the image for now, delete the
`KERNEL_MODULE_AUTOLOAD` line from `ramon-dma.bb` and load `ramon_dma` by hand after
`rmmod axidmasg_drv ps2psk`.

---

## Kernel prerequisite: export whitelist (once)

The target kernel is built with `CONFIG_TRIM_UNUSED_KSYMS=y`, which drops every export no in-tree
module uses. modpost then fails with e.g. `"__init_rwsem" [ramon_dma.ko] undefined!` (the old
driver hit the same thing with `__memcpy_fromio` and `of_find_node_opts_by_path`).
`linux-xlnx/ramon_dma.ksyms` whitelists every symbol the finished driver is expected to import
(steps 1-9). Listing a symbol that is already exported, or one that does not exist in this
kernel, is harmless, so the list errs on the generous side.

```sh
cp linux-xlnx/ramon_dma.ksyms <proj>/project-spec/meta-user/recipes-kernel/linux/linux-xlnx/

# kernel config fragment (the path is relative to the kernel source tree)
echo 'CONFIG_UNUSED_KSYMS_WHITELIST="ramon_dma.ksyms"' \
    >> <proj>/project-spec/meta-user/recipes-kernel/linux/linux-xlnx/bsp.cfg
```

Append to `<proj>/project-spec/meta-user/recipes-kernel/linux/linux-xlnx_%.bbappend` (it
already has the `FILESEXTRAPATHS` line and `file://bsp.cfg`):

```
SRC_URI += "file://ramon_dma.ksyms"

# CONFIG_UNUSED_KSYMS_WHITELIST is resolved relative to the kernel source tree
do_configure:prepend() {
    install -m 0644 ${WORKDIR}/ramon_dma.ksyms ${S}/ramon_dma.ksyms
}
```

```sh
petalinux-build -c kernel -x cleansstate
petalinux-build -c kernel
grep -E "TRIM_UNUSED_KSYMS|UNUSED_KSYMS_WHITELIST" <kba>/.config
grep -wc __init_rwsem <kba>/Module.symvers        # expect 1
petalinux-build                                   # every module must be rebuilt against this kernel
```

The board has to boot this new kernel image. The running kernel's exports are what `insmod`
resolves against. If a later step still reports `undefined!`, add the name to
`ramon_dma.ksyms` (one per line, no comments) and rebuild the kernel. I extend the file
whenever a step starts using a new kernel API.

---

## Step 1: core

### Build (build machine, inside the sourced PetaLinux environment)

```sh
petalinux-build -c ramon-dma
petalinux-build -c ramon-smoke

# compiler warnings do not reach the console; W=1 output is in the compile log
grep -nE "warning:|error:" <proj>/build/tmp/work/*/ramon-dma/0.1.0-r0/temp/log.do_compile
grep -nE "warning:|error:" <proj>/build/tmp/work/*/ramon-smoke/0.1.0-r0/temp/log.do_compile

<ksrc>/scripts/checkpatch.pl --strict -f ramon-dma/files/*.c ramon-dma/files/*.h

# either rebuild the image ...
petalinux-build            # then boot the new image
# ... or copy the two artifacts to a running board
scp <proj>/build/tmp/work/*/ramon-dma/0.1.0-r0/ramon_dma.ko \
    <proj>/build/tmp/work/*/ramon-smoke/0.1.0-r0/ramon_smoke  root@<board>:/tmp/
```

Expected: no `warning:` lines in either log. checkpatch: **3 known findings, all in
`ramon_dma_uapi.h`** (see "Known checkpatch findings" below); anything else is a bug.
Sparse (`C=1`) is not part of the recipe because PetaLinux does not ship it. If you have it,
run `make -C <proj>/project-spec/meta-user/recipes-modules/ramon-dma/files KERNEL_SRC=<kba> C=1`
once by hand.

Please also send the output of these one-liners. They answer the open API questions below:

```sh
grep -w of_find_node_opts_by_path <kba>/Module.symvers
grep -nE "define (XILINX|ZYNQMP)_DMA_NUM_DESCS" <ksrc>/drivers/dma/xilinx/xilinx_dma.c <ksrc>/drivers/dma/xilinx/zynqmp_dma.c
grep -E "^CONFIG_(XILINX_DMA|XILINX_ZYNQMP_DMA|ARM_SMMU_V3|ARM_SMMU|COMPAT|PROVE_LOCKING|DYNAMIC_DEBUG)=" <kba>/.config
```

### On the board

With the new image, the module is autoloaded and `ramon_smoke` is in `/usr/bin`. With the scp
route, use `insmod /tmp/ramon_dma.ko` and `/tmp/ramon_smoke`.

```sh
# The old modules must not be loaded: axidmasg_drv binds the same DT node, ps2psk holds the ZDMA channels.
rmmod axidmasg_drv ps2psk 2>/dev/null; lsmod | grep -E "axidmasg|ps2ps"   # expect nothing

lsmod | grep ramon_dma || insmod ramon_dma.ko
dmesg | grep ramon_dma               # expect: "<node>: ramon_dma 0.1.0 (abi 1) ready as /dev/ramon_dma"
ls -l /dev/ramon_dma
cat /sys/module/ramon_dma/version    # 0.1.0

ramon_smoke                        # default sequence = "info" in step 1
ramon_smoke --errors               # ENOTTY / EFAULT dispatcher checks

# unbind with an fd open: superseded by `ramon_smoke unbind` (step 2), which unbinds and
# rebinds by itself. The manual `hold` flow never checked that the unbind took effect.

rmmod ramon_dma
insmod ramon_dma.ko max_buf_mb=0     # must fail: "max_buf_mb=0 out of range 1..4096"
insmod ramon_dma.ko
```

Failure details for ioctls go to `dev_dbg` only. To see them:
`echo 'module ramon_dma +p' > /sys/kernel/debug/dynamic_debug/control`.

In step 1 every AXI/ZDMA/SPW/SPFI/regwin count reported by `info` is 0. That is expected: those
sub-blocks come in later steps.

### Checked here (not the target)

- The module builds with `W=1` and zero warnings against the host's x86_64 **6.8** headers
  (Ubuntu, gcc 11, `CONFIG_OF` off). That catches typos and type errors, not arm64 or 6.1
  differences. Sparse is not installed here, so `C=1` was **not** run.
- A deliberately mismatched ioctl/struct pair fails to compile (the `static_assert` guard works).
- `ramon_dma_uapi.h` compiles as C99 and C++11 with `-Wall -Wextra -pedantic`. Every struct has
  the same size and trailer offset under `-m32` as under x86_64 (i386 aligns `__u64` to 4, so a
  padding mistake would have shown).
- `checkpatch.pl --strict` (from the 6.8 tree): clean except the 3 known findings.
- `ramon-smoke/files/ramon_smoke.c` builds with host gcc `-Wall -Wextra` and no warnings, against
  a copy of the header in the `${includedir}/ramon` layout the recipe uses.
- The recipes have **not** been parsed by bitbake. `COPYING` in both recipes has the md5 in
  `LIC_FILES_CHKSUM` (same file and checksum as the old recipes). `LICENSE` uses the SPDX name
  `GPL-2.0-only` instead of the old `GPLv2`, which newer Yocto releases flag as obsolete.

### Known checkpatch findings (ramon_dma_uapi.h)

```
ERROR: Macros with complex values should be enclosed in parentheses   (RAMON_ERR_LIST)
CHECK: Macro argument reuse 'X' - possible side-effects?              (RAMON_ERR_LIST)
ERROR: Macros with complex values should be enclosed in parentheses   (RAMON_ERR_ENUM)
```

checkpatch cannot parse X-macros, and the spec asks for an X-macro error list, so these two
requirements conflict. The alternative is to spell out each code by hand (an enum plus a
separate table), which gives up the single source of truth. **Decision needed from you.** I kept
the X-macro.

### APIs and facts not confirmed here

| Item | Why it matters | Step |
|---|---|---|
| `of_find_node_opts_by_path` exported? | It was undefined only because of `TRIM_UNUSED_KSYMS`; it is in the whitelist, so the plain `of_find_node_by_path()` can be used instead of porting the manual resolver. Confirm with the `Module.symvers` grep after the kernel rebuild. | 3, 7 |
| `XILINX_DMA_NUM_DESCS`, `ZYNQMP_DMA_NUM_DESCS` | Xilinx: **5000** on the board kernel, so `RAMON_AXI_MAX_ITEMS` = 5000 (0.8.8). ZynqMP: still unknown; the ZDMA chunk (16) assumes 32 | 4, 6 |
| xilinx_dma / zynqmp_dma built-in or modules | `MODULE_SOFTDEP` only helps if they are modules; `-EPROBE_DEFER` covers both cases | 4, 6 |
| SMMU enabled for the PL node? | If yes, `dma_free_coherent` after an unbind (buffers of a still-open fd) needs care | 2, 8 |
| DMA mask | Not changed; left at the platform default (32-bit unless dma-ranges says otherwise), exactly like the old driver, because the AXI DMA IP may be 32-bit | 2 |
| `compat_ptr_ioctl` | Used as `.compat_ioctl`. On arm64 it is identical to calling `unlocked_ioctl` directly; it is the kernel idiom for "compat = native" | 1 |
| `platform_driver.remove` returning `int` | Correct for 6.1 (`remove_new` arrived in 6.3) | 1 |

### Design choices made in step 1 (please review)

- **`ramon_dev.gate`, an addition to section 2** (an rw_semaphore in step 1, SRCU since step 2).
  Every ioctl and mmap runs inside a read section. `remove()` sets `dead`, wakes all waiters,
  then `synchronize_srcu()`s. Without it, a handler that passed the `dead` check could touch
  MMIO or a DMA channel after `remove()` released them, because devm unmaps after `remove()`
  returns. Every wait condition includes `dead`, so remove never waits longer than one DMA
  timeout. It became SRCU because an rwsem can deadlock: mmap enters with `mmap_lock` held, an
  ioctl inside the gate can fault on a user pointer (needs `mmap_lock`), and a queued
  `down_write` in remove blocks new readers. SRCU readers never block.
- Error list columns are `X(name, code, errno, desc)` (4, not 3), so the namespaces can have
  gaps. The errno per code is in the header; the notable choices are `NOT_PRESENT` → `ENXIO`
  (distinct from `REMOVED` → `ENODEV`), range errors → `ERANGE`, `REGWIN_READ_ONLY` → `EROFS`,
  and bad channel/window index → `ENOENT`.
- `GET_STATS` gained `spfi_unexpected[2]`. The IRQ spec says "count as unexpected" but had no
  field for it.
- There is one UAPI struct per ioctl, named after it (`struct ramon_spfi_cmd` for
  `RAMON_IOC_SPFI_CMD`). Opcode `0x13` is `RAMON_SPFI_OP_FLUSH_STREAM` (`FLASH_STREAM` in the
  old code).
- `needs_dev_alive = false` only for ioctls that touch no hardware (`GET_INFO` now; `BUF_FREE`,
  `BUF_INFO` and `GET_STATS` later).
- `ramon_fail()` fills the trailer and the dispatcher emits the single `dev_dbg` for it, so the
  handler signature stays exactly as specified.
- The Makefile and the recipes' `SRC_URI` list only the objects/sources that exist so far; each
  step adds its own.

---

## Step 2: buffers and mmap

New: `ramon_buf.c` (`BUF_ALLOC` / `BUF_FREE` / `BUF_INFO`, `mmap`, per-file cleanup). The core
now uses an SRCU gate instead of the rwsem (see "Design choices"). Driver version `0.2.0`.

### Build

Re-copy `ramon-dma/` and `ramon-smoke/` into `meta-user` (new `ramon_buf.c` in `SRC_URI`,
`PV = "0.2.0"`), then:

```sh
petalinux-build -c ramon-dma && petalinux-build -c ramon-smoke
grep -nE "warning:|error:" <proj>/build/tmp/work/*/ramon-dma/0.2.0-r0/temp/log.do_compile
```

New kernel imports: `init_srcu_struct`, `cleanup_srcu_struct`, `__srcu_read_lock`,
`__srcu_read_unlock`, `synchronize_srcu`, `dma_alloc_attrs`, `dma_free_attrs`,
`dma_mmap_attrs`, `idr_alloc_cyclic`, `idr_find`, `idr_remove`, `idr_get_next`. They are all in
`linux-xlnx/ramon_dma.ksyms`. On the non-hardened image the whitelist does not matter; on the
hardened one any of these that modpost reports must be added and the kernel rebuilt.

### On the board

```sh
grep -E "CmaTotal|CmaFree" /proc/meminfo     # the CMA checks are skipped if CmaTotal is 0
ramon_smoke                                  # info, buf, churn, kill
ramon_smoke --errors                         # dispatcher + buffer argument errors + CMA exhaustion
ramon_smoke unbind                           # automatic unbind with an fd open AND a buffer mapped, then rebind (root)
dmesg | grep -iE "ramon|warn|oops|bug|cma"
```

What the new tests check:
- `buf`: sizes 1, one page, page+904 and 1 MiB+1. Each gets a page-rounded, zeroed buffer
  that maps and holds a pattern, and its handle is dead after `BUF_FREE`. Mapping a prefix of a
  buffer works. A 16 MiB buffer freed while still mapped stays usable, and `CmaFree` comes back
  only at `munmap`.
- `churn`: 8 threads × 300 alloc/mmap/verify/free of random sizes on one fd. Every fresh buffer
  must be zero, which catches two threads getting the same handle.
- `kill`: a child maps 16 MiB and is `kill -9`ed; `CmaFree` must come back.
- `--errors`: size 0, size above `max_buf_mb`, unknown handle (free/info), `MAP_PRIVATE`, mapping
  longer than the buffer, unknown handle / offset 0 in mmap. Then it allocates `max_buf_mb`
  buffers until the allocator fails and expects `RAMON_E_BUF_ALLOC_FAILED`. That briefly takes
  **all of CMA**, so do not run `--errors` while something else on the board needs CMA.
  Nothing new should appear in dmesg for the argument errors.
- `unbind` (replaces the manual `hold`, needs root): unbinds the device itself, taking the name
  from `/sys/class/misc/ramon_dma/device`, while a 1 MiB buffer is mapped. After the unbind:
  - `/dev/ramon_dma` is gone;
  - GET_INFO still works and the mapping still reads back;
  - `BUF_ALLOC` gives `RAMON_E_REMOVED`/`ENODEV`, and `mmap` gives `ENODEV`.

  Then it unmaps and frees the buffer, so the memory is freed on the **unbound** device, closes
  the fd, rebinds, and checks that a new fd works. Freeing on the unbound device is the path
  that is only safe without an SMMU for this device (see the table above). Watch dmesg for
  warnings from the DMA or IOMMU layers.

Optional: reload with `insmod ramon_dma.ko max_buf_mb=8` and rerun `ramon_smoke --errors`. That
checks the module parameter (the above-max test then uses 8 MiB + 1) and exhausts CMA in
8 MiB steps rather than 256 MiB ones.

### Checked here (not the target)

- Host x86_64 6.8 `W=1` build clean; checkpatch unchanged (the 3 known UAPI findings only).
- Every symbol the host build imports (minus x86/instrumentation helpers) is in
  `ramon_dma.ksyms`.
- The smoke tool builds with `-Wall -Wextra -Wformat=2 -Wshadow` against the installed-header
  layout.

### Behaviour worth knowing

- Handles are per fd, start at 1, are allocated cyclically (a freed handle is not reused at once),
  and stay ≤ 0x7FFFF so `handle << 12` fits a 32-bit `off_t`.
- `mmap` needs `MAP_SHARED` and maps from the start of the buffer; `len` may be shorter than
  the buffer. mmap failures return errnos from the same table as ioctls
  (`NO_SUCH_HANDLE` → `ENOENT`, `BUF_MMAP_LEN` → `EINVAL`, `REMOVED` → `ENODEV`); the
  `RAMON_E_*` name and details go to `dev_dbg`.
- Buffers are freed at `BUF_FREE`, or later if still mapped or (from step 4) in use by a
  transfer, and in any case when the fd is closed or the process dies.

---

## Step 3: register windows and DT discovery

New: `ramon_of.c` (Appendix C lookups for SPW, SPFI, RS-TOP, reg-access) and
`ramon_regwin.c` (window table, `REGWIN_INFO`, `REG_IO`). Driver `0.3.0`, smoke `0.3.0`.
**First hardware write:** at probe, RS-TOP offset `0x40` is set to 1 (TRST/SPI_EN), exactly as the
old driver did.

### Build

Re-copy `ramon-dma/` and `ramon-smoke/` into `meta-user`, then:

```sh
petalinux-build -c ramon-dma && petalinux-build -c ramon-smoke
grep -nE "warning:|error:" <proj>/build/tmp/work/*/ramon-dma/0.3.0-r0/temp/log.do_compile
```

New kernel imports on arm64 are expected to be the OF helpers (`of_find_compatible_node`,
`of_address_to_resource`, `of_count_phandle_with_args`, `__of_parse_phandle_with_args`,
`of_property_read_string_helper`, `of_node_put`), `devm_ioremap`, `devm_kmalloc`, `strscpy`,
`strncmp`, `strnlen` and (with `FORTIFY_SOURCE`) `fortify_panic`. They are all in
`linux-xlnx/ramon_dma.ksyms`.

### On the board

```sh
rmmod ramon_dma; insmod /tmp/ramon_dma.ko
dmesg | grep ramon_dma        # one "regwin <i> <name>: <phys> size ..." line per window, then
                              # "<n> register windows, <m> absent"; a line saying why each absent one is missing
ramon_smoke                   # info, buf, churn, kill, regwin
ramon_smoke --errors          # now also the register-window errors
ramon_smoke unbind            # register windows must be gone after the unbind (devm), no oops
```

`regwin` checks and prints:
- **The window table.** It lists every window and checks the 12 fixed names and indices, and
  the RO flags on SYSMON/RTC/TTC. Absent windows must have size 0.
- **RS-TOP.** It prints the version `0x0.0x4.0x8`, checks that name and index lookups agree, and
  writes 1 to `0x40` again (the probe write) and reads it back.
- **SYSMON.** PS and PL temperatures must fall between −40 and 125 °C. That is the check that the
  PS windows map the right registers.
- **RTC and TTC.** It samples `rtc[0x10]` twice, 1.2 s apart (prints only), and prints `ttc<n>[0x0c]`.
- **Lookups.** `spw0`/`spw1`/`spfi0`/`spfi1` found by prefix must read the same as by index:
  SPW version at `0x0`, SPFI `WR_VERSION` at word 16.
- **Concurrency.** 8 threads × 5000 RS-TOP + SYSMON reads, mixing name and index lookups.

`--errors` adds:
- index past the end (`REGWIN_INFO` and `REG_IO`);
- an unknown name, and a 32-character name without a NUL;
- an unaligned offset, an offset at the end of the window, and an offset near 4 GiB;
- a write to a read-only window, and op 7;
- a read from an absent window, if the board has one.

Please send the `dmesg` window lines along with the smoke output. They show the SPW/SPFI unit
addresses, and so the NN A/B assignment the SPW and SPFI steps will rely on.

### Checked here (not the target)

- Host x86_64 6.8 `W=1` build clean (with `CONFIG_OF` off there, so the OF paths are
  type-checked but not run); checkpatch unchanged (3 known UAPI findings only).
- Every host import (minus x86/instrumentation helpers) is in `ramon_dma.ksyms`.
- Smoke builds with `-Wall -Wextra -Wformat=2 -Wshadow`.

### Behaviour worth knowing

- The UAPI gained `enum ramon_win_index` (`RAMON_WIN_RS_TOP` ... `RAMON_WIN_FIXED`) and
  `RAMON_TTC_COUNT`. That is purely additive, so `RAMON_ABI_VERSION` stays 1.
- Every window is mapped once at probe (devm). Steps 5 and 7 will take their SPW/SPFI register
  pointers from this table instead of mapping again.
- Sizes: SPW `0x1000` and SPFI `0x2000`, as the old driver mapped them (with a warning if the DT
  `reg` is smaller); RS-TOP `max(DT size, 0x44)`; reg-access the DT size; PS windows as in
  Appendix A.4.
- `REG_IO` by name accepts the full name or the part before `@` (`"spw0"`). Exact names win.
- A block missing from the DT is not a probe failure: its windows are listed as `ABSENT` and
  dmesg says why.

---

## Batch A (steps 4-6): AXI DMA, SPW, ZDMA, GET_STATS

New files: `ramon_axidma.c`, `ramon_spw.c`, `ramon_zdma.c`. `ramon_of.c` now parses the AXI
channels (Appendix C.1). DT discovery runs **once** per probe into `struct ramon_of_hw`, and
`ramon_regwin.c` / `ramon_spw.c` read from it. That is a restructure of step 3 with the same
windows, order and log lines. Driver `0.6.0`, smoke `0.6.0`.

### Build

Re-copy `ramon-dma/` and `ramon-smoke/` into `meta-user`, then:

```sh
petalinux-build -c ramon-dma && petalinux-build -c ramon-smoke
grep -nE "warning:|error:" <proj>/build/tmp/work/*/ramon-dma/0.6.0-r0/temp/log.do_compile
```

New kernel imports include `dma_request_chan`, `dma_request_chan_by_mask`,
`dma_release_channel`, `dev_err_probe`, `devm_request_threaded_irq`, `devm_free_irq`,
`irq_of_parse_and_map`, the completion and wait helpers, `vmemdup_user`, `kvfree` and
`_raw_spin_lock_irq`. They are all in `linux-xlnx/ramon_dma.ksyms` (140 names now). The
hardened image needs a kernel rebuild with the current file before it can load this module.

### On the board

```sh
rmmod axidmasg_drv ps2psk 2>/dev/null      # ps2psk would hold the ZDMA channels
rmmod ramon_dma; insmod /tmp/ramon_dma.ko
dmesg | grep ramon_dma      # per AXI channel: "axi ch<i> <name>: MEM_TO_DEV (mm2s), device-id, axi_dma <node>, copy_align"
                            # "<n> AXI channels, <m> usable"; "zdma ch<i> ..."; "zdma: <n> of 4 memcpy channels"
                            # "spw<nn> spw<nn>@...: irq <n>"
ramon_smoke                 # info buf churn kill regwin chan spw zdma stats
ramon_smoke --errors
ramon_smoke unbind
dmesg | tail -40
```

What the new tests do:
- `chan`: lists every channel; AXI first, then ZDMA, with valid directions.
- `spw` (smoke 0.6.1), per present NN, using AXI channels `2*nn` / `2*nn+1` (the old spw_api
  layout; the S2MM one is RX):
  1. Loopback on, which isolates RX from NN traffic: drain unsolicited packets (every popped
     size is followed by the RX DMA of that packet, so the stream stays aligned), then an idle
     200 ms wait must time out and one `SPW_CANCEL` must release two waiters.
  2. Loopback off, as the old `spwinit` left it, then the old `spw_init()` link sync: read
     `0x28`, and until `(v & 7) == 5` write 0 to `0x1C` and sleep 10 ms (at most 1000 times).
  3. The old `get_nn_version()` round trip: an RX thread blocks in `SPW_WAIT_RX` while the
     main thread sends a 40-byte header-only request (dst `--target`, default `0x41`; src
     `--node`, default 68 as the old `spwappinit 0 +68`; protocol 9, app type 9, attribute
     `0x98`; header and payload CRC as the old `rc_crc32sw`). The RX thread DMAs exactly the
     reported size. The reply must be addressed to `--node`, and both of its CRCs must match.
     Its source, attribute, footer and payload (hex and text) are printed.
  4. The saved loopback setting is restored.
- `zdma`:
  - a single 1 MiB copy;
  - a 100-entry list of scattered sizes, which crosses the 16-entry chunks;
  - a 4096-entry list;
  - 8 threads × 100 copies competing for the pool.
- `stats`: prints every counter, then checks that `reset` zeroes them.
- `--errors` adds:
  - bad channel/index, 0 and `RAMON_AXI_MAX_ITEMS + 1` items, a bad items pointer, `len 0`, an unknown handle, an
    item past the buffer end, and an `offset + len` overflow;
  - **an AXI RX timeout on NN0's RX channel with loopback on and no traffic.** That
    terminate soft-resets that axi_dma IP (both directions), which is expected;
  - ZDMA `n = 0`, `n = 4097`, a bad pointer, a list whose second entry is invalid (it must
    copy nothing), and dst past the end;
  - SPW `nn 2`, loopback `enable 2`, and a wait on an absent NN.

If the version round trip fails, send the whole `spw` output and `dmesg`. The header layout,
CRC and node ids come from the old app (`spw_api.cpp`, `get_nn_version()`, `rc_crc32.cpp`).
The CRC was cross-checked on the host against the old `rc_crc32sw` (2000 random buffers, 0
mismatches). The first `spw` version (smoke 0.6.0) used FPGA loopback with a dummy header and
timed out in `SPW_WAIT_RX`: loopback did not return the packet.

### Checked here (not the target)

- Host x86_64 6.8 `W=1` build clean; checkpatch unchanged (the 3 known UAPI findings only).
- Every host import (minus x86/instrumentation helpers) is in `ramon_dma.ksyms`.
- Smoke builds with `-Wall -Wextra -Wformat=2 -Wshadow`.

### Behaviour worth knowing

- **AXI:**
  - A transfer is synchronous: prepare (copy the items, pin every buffer, build the sg list),
    then run under the channel mutex, then release.
  - Items must satisfy the channel's `copy_align` (printed at probe; set by xilinx_dma when
    the IP has no DRE), else `RAMON_E_NOT_ALIGNED`.
  - Any timeout, kill or bad status terminates the channel, and dmesg (ratelimited) names
    the axi_dma IP that was reset.
  - An absent channel keeps its index and gives `RAMON_E_NOT_PRESENT`.
  - Probe fails only if **no** AXI channel is usable. `-EPROBE_DEFER` from any channel defers
    the whole probe, before anything touches the hardware.
- **SPW:**
  - Every RX interrupt queues its `RX_PKT_SIZE` (64 deep, overruns counted). **Behaviour
    change:** the old driver kept only the last size, so packets that arrive while nobody
    waits are now returned one by one instead of being lost.
  - `SPW_CANCEL` releases only waiters that started before it.
  - `SPW_LOOPBACK` accepts 0/1 and touches only its own NN.
  - SPW timeouts are routine and are never logged.
- **ZDMA:**
  - The whole list is validated before anything is copied, so an invalid list copies nothing
    and `done` stays 0.
  - The pool takes up to `zdma_channels` (default 4, range 0..16) DMA_MEMCPY channels; a
    copy with no free channel sleeps killably.
  - `RAMON_E_ZDMA_NO_CHANNELS` when none were acquired.
- **Remove order:** free the SPW IRQs → mark dead (full barrier) → complete every DMA
  completion and wake every waitqueue → `misc_deregister` → drain the SRCU gate → release
  the ZDMA channels, then the AXI channels.

---

## Batch B (steps 7-8): SPFI, remove hardening, stress, error sweep

New file: `ramon_spfi.c`. `ramon_of.c` now also resolves the SPFI interrupts and the two DT-label
memories (through `/__symbols__` and `of_find_node_by_path()`; the export is whitelisted).
Driver `0.8.0`, smoke `0.8.0`.

### Build

Re-copy `ramon-dma/` and `ramon-smoke/` into `meta-user`, then:

```sh
petalinux-build -c ramon-dma && petalinux-build -c ramon-smoke
grep -nE "warning:|error:" <proj>/build/tmp/work/*/ramon-dma/0.8.0-r0/temp/log.do_compile
```

### On the board

```sh
rmmod ramon_dma; insmod /tmp/ramon_dma.ko
dmesg | grep ramon_dma      # adds "spfi<nn> spfi<nn>@...: irq <n>" and the two table lines per NN
ramon_smoke                 # ... spw, zdma, spfi, stats
ramon_smoke --errors        # ends with the list of error codes it produced
ramon_smoke unbind          # now with SPFI_WAIT_ALERT / SPW_WAIT_RX threads blocked across it
ramon_smoke --stress --minutes 10
dmesg | tail -60
```

Recommended once: the same `--stress` run on a kernel built with `CONFIG_PROVE_LOCKING=y`,
`CONFIG_DEBUG_ATOMIC_SLEEP=y` and `CONFIG_KASAN=y` (lockdep and use-after-free checks).

**What the `spfi` test does to the NN.** Per NN, it creates two streams at the **two highest
free stream ids in 0..192** (read from the NN's own table). NN rules it follows (smoke 0.8.5):
never use the last 5 ids (193..197); at most 8 streams open at once (it refuses to start if two
more would exceed that); an existing stream must be deleted before its id is opened again (it
only picks ids that do not exist). It writes 8 + 3×8 pages (512 KiB) to them,
reads them back, then closes and deletes them. Existing streams are never touched. INIT and
FORMAT are never sent; `--spfi-init` adds INIT type 0 before the test. The write channel is
4 for both NNs since smoke 0.8.4 (`--spfi-chans A,B` overrides). The old `spfidrvinit` used 5/4,
but channel 5 has been removed from the FPGA; a write on it fails with a DMA internal error
(status 0x10) from its axi_dma IP and times out.

In detail:
1. It prints the table header (media, allocated and used pages).
2. OPEN, then it reads `latest_write_offs` from the table; the first page written is that + 1,
   as in the old `spfi_send`.
3. It writes 8 pages, FLUSHes (printing `rx_offset`), reads the pages back and verifies them.
4. It opens a second stream. A thread writes it 3 × 8 pages while the main thread re-reads the
   first stream 3 times (the old `spfirxtx` pattern); then both are verified.
5. CLOSE and DELETE both streams; the table must show them gone.
6. Alerts: it prints any pending alert, then an idle 200 ms wait must time out and one
   `SPFI_CANCEL_ALERT` must release two waiters.

**`--errors` additions:**
- SPFI: `nn 2`; the opcodes `0x10` and `0x77`; `MEM_READ` of size 0 and past the table;
  `TX_OFFS_WRITE` past the table; `READ` with 0 and 8193 offsets, into a 1-page buffer, and
  with a bad pointer; a `WRITE` whose items don't match `tx_num_offset`.
- **A 1 ms timeout on GET_ALL_STREAM_STATUS** (harmless; the late answer is counted in
  `spfi_unexpected`).
- **A READ of a stream that doesn't exist**, with a 300 ms timeout. If the NN answers instead,
  this is printed, not failed.
- An idle SPW wait and an SPW cancel, under loopback.
- `SIGUSR1` sent to a thread blocked in `SPFI_WAIT_ALERT` (or `SPW_WAIT_RX`): it must get
  `EINTR` / `RAMON_E_INTERRUPTED`.
- A ZDMA copy from an odd address: `RAMON_E_NOT_ALIGNED` if the engine needs alignment.
- At the end, the list of `RAMON_E_*` codes that were not produced. Engine failures (AXI/ZDMA
  PREP, SUBMIT, DMA_ERROR), `SPFI_UNEXPECTED_OPCODE`, `SPFI_WRITE_TIMEOUT` and `NO_MEMORY`
  cannot be provoked safely. `REMOVED` comes from `unbind`.

**`--stress`:** 4 ZDMA copy threads, 2 buffer-churn threads, a register reader, a GET_STATS
reader, an SPW version round-trip loop per **synced** NN, and an SPFI table loop per NN. It
prints ops/failures every minute and fails on any failure. It writes no SPFI stream data, to
avoid wearing the NN's media.

**SPW links:** an NN whose link does not sync is reported as skipped, not failed. The `spw`
test fails only if no present NN syncs.

### Checked here (not the target)

- Host x86_64 6.8 `W=1` build clean; checkpatch unchanged (the 3 known UAPI findings only).
- Every host import (minus x86/instrumentation helpers) is in `ramon_dma.ksyms`, including the
  new `ioremap` and `wait_for_completion_interruptible_timeout`.
- Smoke builds with `-Wall -Wextra -Wformat=2 -Wshadow`.

### SPFI design points beyond the spec (please review)

1. **`short_lock` per NN**, held across an `SPFI_CMD` wait. There is one pending-command slot
   per NN, so two concurrent short commands on one NN would otherwise mix up their answers.
   Writes and reads still overlap short commands as specified.
2. **CLOSE and FLUSH also take `write_lock`.** Per the register map, their ack also raises
   VC1TX, the write-done interrupt, which would complete an armed DATA_WRITE early.
3. **The IRQ decodes RX by opcode, once per interrupt:** `0xFE` → alert queue, `0x98` →
   read-done, anything else → command completion. This matches the spec for single-bit
   vectors; when VC0RX and VC1RX arrive together, it avoids handling the one `RX_COMMAND`
   register twice.
4. **Byte-safe `MEM_READ` / `TX_OFFS_WRITE`:** bytes until 4-aligned, then words, then a byte
   tail. Word access at an unaligned device address faults on arm64.
5. **`REG_IO` on the `spfi<n>` windows takes the SPFI spinlock**, as the old `SPFI_IO_REG` did.
6. **An odd offset count gets a zero pad word** after the last offset in the TX offset table,
   as the old `spfi_receive()` wrote it (an 8-byte write of `{last, 0}`).
7. **A timed-out READ_STREAM keeps its destination buffer referenced** until a later read on
   that NN completes, or until remove. The FPGA may still DMA into it later; the old driver let
   that memory be freed and reused.
8. The DATA_WRITE / READ_STREAM completion timeout defaults to 3000 ms (the spec's table). The
   old userspace waited 2000 ms.

### Open questions

- ~~Generic `RAMON_E_CANCELLED` and `RAMON_E_TIMEOUT` have no user.~~ Decided: dropped in driver
  0.8.7. Codes 9 and 10 are retired; no other code changed number.
- For the FPGA team, in addition to the three in the spec:
  - Must the TX offset table be written in pairs, i.e. is the zero pad after an odd count
    required?
  - Does CLOSE/FLUSH really raise VC1TX, and can a late READ_STREAM answer arrive after a
    timeout?
  - The old `spw_init()` writes 0 to `FPGA_SPW_RESET` (0x1C), while the loopback path writes 1.
    Does the value matter?

---

## Smoke 0.8.2: SPW commands and SPFI bring-up (driver unchanged, 0.8.0)

- **`spw` test, TX/RX mode:** the wait/cancel checks run with loopback **on**, so the NN cannot
  interfere. The round trip runs with loopback **off** (the old `spwinit` default): link
  sync, then the version request to node `0x41`. The reply is decoded as 4KBL / RSBL / Image
  (200 bytes each, at payload offsets 0, 200 and 400) and FW (20 bytes at 600), as the old
  `dmaapi_testv2.cpp` does.
- **`ramon_smoke spwdps [nn]`:** the old `spwdps` (dst `0x41`, protocol 9, app type 1,
  attribute `0x01`, no payload). The reply must carry attribute `0x101`.
- **`ramon_smoke spwsend <nn> <dst> <proto> <app> <attr> [hexpayload]`:** any SPW message,
  header and CRCs as the old `spw_send()`; the reply is printed and CRC-checked.
- **`ramon_smoke spfiprep [nn] [--format]`:** the old SPFI bring-up in order:
  1. the low byte of SPFI `0xCC` (word 51, the old `spfireg +204`) must be `0x88`; the full
     value is printed (e.g. `0x4488`, which is expected);
  2. `spwdps` on the SPW link of the same NN index;
  3. `INIT` type 0;
  4. `FORMAT`, **only with `--format`, because it erases every stream on that NN**;
  5. GET_ALL_STREAM_STATUS, printing the table.
- **The default `spfi` test** now checks `(0xCC & 0xFF) == 0x88` first. An NN whose SPFI link is down is
  skipped, with a pointer to `spfiprep`; the test fails only if no present NN is up.
  `--errors` and `--stress` skip the link-dependent SPFI checks the same way.
- The explicit SPW subcommands leave loopback **off** (the operating state). The default `spw`
  test restores whatever loopback setting it found.
- Assumption to confirm: SPFI NN *n* and SPW NN *n* are the same NN, so `spfiprep 0` sends DPS
  on `spw0`.

Suggested order on the board:

```sh
ramon_smoke --version        # 0.8.2
ramon_smoke spw              # sync + version round trip on the cabled NN
ramon_smoke spwdps 0
ramon_smoke spfiprep 0       # add --format to match the old flow exactly (erases streams)
ramon_smoke spfi
```

---

## Driver 0.8.1 / smoke 0.8.4

- **SPFI write channel is 4.** Channel 5 was removed from the FPGA. The DT still lists it, so
  the driver still acquires it, which is harmless as long as nothing transfers on it. Its first
  use returned `xilinx-vdma a0020000.dma: Channel ... has errors 10` (the AXI DMA
  internal-error bit), and the transfer was terminated after the timeout. `--spfi-chans`
  overrides the default.
- The AXI probe line now prints the axi_dma register address as well as its DT node:
  `axi ch5 axidma_tx4: MEM_TO_DEV (mm2s), device-id .., axi_dma 0x00000000a0020000
  /amba_pl@0/dma@a0020000, ...`. `ramon_smoke chan` prints `phys 0xa0020000` without zero
  padding. The mapping itself was already right; only the numeric form was missing.
- A stream left open by the failed run (197 on NN0) is skipped by the next `spfi` run, which
  picks the highest free ids.

### Smoke 0.8.5: NN stream rules

- Cyclic streams are not supported by the NN: the test opens and writes non-cyclic streams
  only (`stream_type` 0, DATA_WRITE `stream_last_offset` 0).
- At most 8 streams open, ids 193..197 never used, delete before reopening. The `spfi` test
  follows all three, and the table printout lists existing streams, marking any that sit on a
  reserved id.
- `ramon_smoke spfidel <nn> <sid>` closes a stream if it is open, then deletes it (any id,
  e.g. `spfidel 0 197` for the stream the channel-5 run left behind).

### Smoke 0.8.6: DATA_WRITE parameters as the old `spfi_send`

- On channel 4 the write stalled: `xilinx-vdma a0030000.dma: Cannot stop channel ...: 10008`,
  then our timeout and terminate. `xilinx-vdma` is the kernel xilinx_dma driver's device name
  for every AXI DMA, and `a0030000` is channel 4's axi_dma (our own log line says so). Status
  `0x10008` = running, neither halted nor idle: the MM2S engine was waiting for the stream sink
  (SPFI) to take data, i.e. SPFI did not accept the write.
- Difference found: for a non-cyclic stream the old `spfi_send` sends DATA_WRITE with
  `stream_last_offset = 0`; the test sent 8 (and 24). Fixed. `tx_offset` (table
  `latest_write_offs` + 1), `tx_num_offset`, the register order and the sequence (command, then
  DMA, then wait for VC1TX) already matched.
- On a write failure the test now prints SPFI words 48..51 (VC ctrl / write status), 19 (rx
  opcode), 21 (rx err code) and 33 (rx status), plus the SPFI interrupt histogram. It prints the
  same line once before the first write, for comparison.

### Smoke 0.8.7: SPFI command order as the old flow; word 50 tracing

- SPFI_WR_STATUS0 (word 50, `0xC8`) bits from the FPGA team: bit 5 = VC1 sticky error, no TLAST
  during a TX DMA transfer; bit 3 = VC1 full (send no more VC1 commands); bit 1 = VC1 command
  overflow. After the channel-5 run it read `0x2a`. After a reboot it reads `0x20` before our
  first DATA_WRITE, and the last RX opcode (word 19) was `0xD0`, the GET_ALL_STREAM_STATUS answer.
- The old flow is GET_ALL (`spfiginfo`), then OPEN, then DATA_WRITE. The test did OPEN, then
  GET_ALL, then DATA_WRITE, i.e. a VC1-answered command right before the VC1 write. It now uses
  the old order: the first page comes from the table read before OPEN.
- Word 50 is printed and decoded at the start, after GET_ALL_STREAM_STATUS, after OPEN and after
  a failed write, so one run shows which command sets bit 5.
- `ramon_smoke reg <window|index> <offset> [value]` reads a register, or writes it and reads it
  back, e.g. `ramon_smoke reg spfi0 0xc8`.

### Smoke 0.8.8: spfiprep diagnostics

- After a reboot: word 50 = `0x0` at boot. `spfiprep 0` (no `--format`): INIT gives
  `rx_err_code 1`, `init_info 0x2` (expected, per the user), then GET_ALL_STREAM_STATUS is not
  acknowledged within 3000 ms. The first `spfiprep` run, before the reboot, did get an answer.
- The old flow always runs FORMAT between INIT and GET_ALL (`spfiinit 0` -> `spfifmt` ->
  `spfiginfo`). Next try: `spfiprep 0 --format`.
- `spfiprep` now prints the SPFI diagnostic line (word 50 decoded, interrupt histogram,
  unexpected/timeout counters) after INIT, after FORMAT and after a failed GET_ALL, plus any
  alerts the NN queued. That shows whether the NN answered with something other than `0xD0`.
- Result: `spfiprep 0 --format` passes. FORMAT is **not** required before GET_ALL (user). Open
  question: without FORMAT, GET_ALL was sent immediately after INIT's answer and went
  unanswered. Either the NN needs time after INIT (FORMAT only added delay), or it was a
  one-off. The smoke 0.8.9 hint claiming FORMAT is required was wrong and is removed in 0.8.10.
- `ramon_smoke spficmd <nn> <opcode> [param]` (smoke 0.8.10) sends one short command and prints
  every result field, with the SPFI diagnostic line before and after, e.g.
  `spficmd 0 0x59 0` (INIT type 0), then `spficmd 0 0x50` (GET_ALL) a few seconds later.

### Smoke 0.8.11: where does VC1 bit 5 come from?

- `spfiprep 0` without FORMAT: word 50 = `0x20` (VC1 sticky, no TLAST) right after INIT, then
  GET_ALL (answered on VC1) went unanswered. Word 50 was `0x0` at boot, and `spfiprep` does no
  SPFI DMA, so no channel-4 transfer is involved. The driver issues INIT exactly as the old one
  did: word 7 = 1, word 5 = init type, then word 0 = `0x59`.
- `spfiprep` now prints the SPFI line at start, after the SPW link sync, after DPS, after INIT
  and after FORMAT, so one run shows which step sets bit 5.
- Open questions for the FPGA/NN side: how is bit 5 cleared, and does INIT (or DPS) legitimately
  leave it set? Does FORMAT clear it? That would explain why the old flow always formatted.
- SPFI virtual channels (user): INIT request and answer on VC0; GET_ALL_STREAM_STATUS request
  on VC0, answer on VC1; DATA_WRITE data on VC1 TX. So word 50 bit 5 ("VC1 no TLAST during TX
  DMA") belongs to the DATA_WRITE data path. INIT cannot set it, and `spfiprep` sends nothing on
  VC1 TX. A `0x20` seen "after INIT" is most likely left over from an earlier stalled write
  (sticky). A faulted VC1 may also block VC1 RX (GET_ALL answers), to be confirmed by the FPGA
  team.

### Driver 0.8.2 / smoke 0.8.12: experiment knobs for the SPFI write stall

After a clean `spfiprep 0 --format`, `spfi` still stalls on channel 4, while the old app and
driver work on the same board (word 50 = `0x20` there too, so bit 5 is not the cause). The
register writes and their order match the old driver exactly; what remains:
- **Timing.** The old stack writes DATA_WRITE and starts the DMA from two separate ioctls,
  which leaves tens of microseconds in between. Ours starts the DMA a few microseconds after
  the command. New driver parameter `spfi_write_gap_us` (default 0 = unchanged behaviour,
  max 100000, writable at runtime) inserts a pause:
  `echo 200 > /sys/module/ramon_dma/parameters/spfi_write_gap_us`.
- **Parameters.** `--spfi-stream N` (first stream id; the second is the highest free one) and
  `--spfi-pages N` (pages per write, 1..64) repeat the old `spfitest` arguments exactly.
- **Buffers.** The old `spfitest` took its buffers from the ps2ps device (`ps2ps_alloc`); ours
  come from the ramon_dma device. This only matters if the ps2ps DT node has a `memory-region`.

New kernel import: `usleep_range_state` / `msleep` (from `fsleep`), both whitelisted.

### Driver 0.8.3 / smoke 0.8.13: up to 8192 SG items per AXI transfer

- The NN accepts up to 5000 offsets in one DATA_WRITE. The old app builds one SG item per
  16 KiB offset, so `RAMON_AXI_MAX_ITEMS` goes from 512 to 8192, matching the TX offset table
  (8192 offsets). The item and scatterlist arrays are now kvmalloc'ed (up to 192 + 64 + 256 KiB).
  This is a UAPI constant change before the ABI freeze; `RAMON_ABI_VERSION` stays 1.
- A large write needs no long item list: one contiguous item of `n × 16 KiB` is enough, up to
  4 GiB - 1 per item and `max_buf_mb` (default 256 MiB, i.e. 16384 offsets).
- Each item uses at least one xilinx_dma descriptor. If the kernel's `XILINX_DMA_NUM_DESCS` is
  smaller than the item count, prep fails with `RAMON_E_AXI_PREP_FAILED` (`ENOSPC`). Please
  run the `grep NUM_DESCS` from step 1 to confirm your kernel's value.

### Driver 0.8.4 / smoke 0.8.14: SPFI1 is not used

- Board fact (user): only NN0's SPFI is available. The DT still lists two SPFI reg/interrupt
  pairs, so a new module parameter `spfi_mask` (default `0x1`, `RAMON_SPFI_MASK_DEFAULT` in
  `ramon_board.h`) selects the NNs the driver uses. A masked NN gets no IRQ request, no
  register use and no DT-label memories. Its SPFI ioctls return `RAMON_E_NOT_PRESENT`, and
  `GET_INFO.spfi_mask` shows only NN0. Probe logs `spfi1: not used (spfi_mask=0x1)`.
- Seen before this change: `ramon_spfi1` (IRQ 60) fired once during a failed SPFI write, while
  the old stack showed no SPFI_B interrupts. With the mask, the driver no longer requests that
  line.
- Smoke: every SPFI step keys off `GET_INFO.spfi_mask`. The `regwin` test no longer reads the
  version register of an unused SPFI window. `reg spfi1 ...` still works when asked for
  explicitly.

### Driver 0.8.5 / smoke 0.8.15: SPFI NN0 writes on channel 5 (a0020000); halt engines on release

- **Measured with the old stack:** reading `MM2S_TAILDESC` (`+0x10`) of every axi_dma engine before
  and after an old `spfitest` shows that only **`0xa0020000`** changes. That engine is our
  **channel 5** (`axidma_tx4`), so the old app's `{5,4}` mapping (NN0 = 5) was right and channel 5
  was never removed. Channel 4 (`a0030000`) is not SPFI0's path; NN0's commands got no data, and
  `ramon_spfi1` fired once, likely NN1's side receiving it. Smoke default back to `5,4`.
- **Driver bug fixed:** `xilinx_dma` frees a channel's descriptor ring on release but does not
  halt the engine. A running AXI DMA ignores the next owner's `CURDESC` write, so after our
  `rmmod`/unbind (which released without terminating) the next session's first transfer could
  walk a freed ring. The first channel-5 failure fits this: `errors 10` with
  `cdr 0x01090100` / `tdr 0x19020500` in unrelated regions. The old driver called
  `dmaengine_terminate_all()` before releasing. The driver now terminates every AXI and ZDMA
  channel before releasing it, and once more after requesting it at probe, so each session starts
  from halted engines. A `Cannot stop channel` line at probe is that cleanup hitting an engine a
  previous owner left stuck; the reset that follows clears it.

Next: reboot, then `insmod ramon_dma.ko` (0.8.5), `ramon_smoke spfiprep 0 --format`,
`ramon_smoke spfi`.
- Result: after a fresh boot with driver 0.8.5, `ramon_smoke spfiprep 0 --format` and
  `ramon_smoke spfi` pass. The SPFI write stall was the wrong channel (4 instead of 5), plus the
  engine left running by a release without terminate.

### Driver 0.8.6: a terminate no longer leaves the other direction deaf

- `spw` stopped working with 0.8.5. Cause: a terminate through xilinx_dma soft-resets the whole
  axi_dma IP, which clears the interrupt enables of both directions, and xilinx_dma re-enables
  them only for the channel it was asked to stop (it sets them otherwise only when a channel is
  acquired). 0.8.5 halted every channel at probe in DT order, so channel 0 (SPW NN A RX) lost its
  interrupt enables when channel 1 (its TX sibling) was halted. The RX DMA then completes
  silently and the SPW reply times out. SPFI passed because channel 5 was halted last.
- The same mechanism already existed on the error path: any terminate (a timeout, e.g. the
  deliberate AXI RX timeout in `--errors`) left the IP's other channels deaf.
- Fix: the probe-time halt is removed (halting before release stays, and a reboot resets the
  engines anyway). After any terminate, every other channel on the same IP is flagged; before its
  next transfer, under its own lock, it is released and re-acquired, which makes xilinx_dma
  re-enable its interrupts without another reset. It logs `axi chN (...): re-acquired after a
  reset of ...` at debug level. The channel's alignment mask is cached at probe, so preparing a
  transfer never dereferences a channel being re-acquired.

### Driver 0.8.7: ABI cleanup

- `RAMON_E_CANCELLED` (9) and `RAMON_E_TIMEOUT` (10) removed from `RAMON_ERR_LIST` (user
  decision). They had no user: every cancel and timeout has its own subsystem code. The numbers
  stay retired. `RAMON_ABI_VERSION` stays 1, since the ABI is not frozen yet.
- The `spfi_write_gap_us` debug parameter is removed; the timing hypothesis was ruled out on the
  board.
- The X-macro error list stays (user asked for the trade-off; recommendation: keep). The 3
  checkpatch findings on `ramon_dma_uapi.h` remain the known ones.

### Driver 0.8.8: AXI item limit = the kernel's descriptor pool

- The board kernel has `XILINX_DMA_NUM_DESCS = 5000` (patched; mainline has 512). Each SG item
  takes at least one xilinx_dma descriptor, so `RAMON_AXI_MAX_ITEMS` is now 5000 (was 8192):
  longer lists fail at once with `RAMON_E_BAD_COUNT` instead of halfway through prep with
  `RAMON_E_AXI_PREP_FAILED`. More than 5000 SPFI offsets per write still work with larger items
  (a contiguous item uses one descriptor per engine-maximum length, not one per 16 KiB).
- Edge case: xilinx_dma returns a finished transfer's descriptors to the pool only after the
  completion callback. A 5000-item transfer started immediately after another could briefly
  find the pool short and fail with `RAMON_E_AXI_PREP_FAILED` (`ENOSPC`); a retry succeeds.
- Still needed: `ZYNQMP_DMA_NUM_DESCS` (the ZDMA chunk of 16 assumes 32).

---

## Phase 2: libramon, ramon_cli, ramon_test

Two new recipes, both 0.1.0:

- `ramon-api` builds `libramon.a`, a static C library over `/dev/ramon_dma`. It installs the
  library to `${libdir}` and its headers (`ramon.h`, `ramon_spw.h`, `ramon_spfi.h`,
  `ramon_version.h`) to `${includedir}/ramon/`, next to the driver's `ramon_dma_uapi.h`. The
  runtime package is empty; a customer recipe needs only `DEPENDS += "ramon-api"` and
  `-lramon -lpthread`. `ramon-api/files/README.md` is the customer documentation.
- `ramon-tools` builds `ramon_cli` (the successor of `dmaapi_testv2`) and `ramon_test` (the
  automatic tests), linked statically against libramon. `ramon-tools/files/README.md` lists
  the commands and tests.
- License: MIT for both, as the old `testdmasgk.bb`; `ramon-tools` also carries the vendored
  linenoise line editor (BSD-2-Clause). The driver and `ramon-smoke` stay GPL-2.0.
- Versions: `RAMON_API_VERSION` (`ramon_version.h`) equals `PV` of `ramon-api.bb`, and
  `RAMON_TOOLS_VERSION` (`tools_common.h`) equals `PV` of `ramon-tools.bb`. Both tools print
  the tool, library and driver versions.

### Installing the two recipes (once)

```sh
cp -r ramon-api   <proj>/project-spec/meta-user/recipes-apps/ramon-api
cp -r ramon-tools <proj>/project-spec/meta-user/recipes-apps/ramon-tools

cat >> <proj>/project-spec/meta-user/conf/user-rootfsconfig <<'EOT'
CONFIG_ramon-api
CONFIG_ramon-tools
EOT
petalinux-config -c rootfs
#   user packages -> enable ramon-tools (ramon-api comes in through DEPENDS; enabling it
#   installs nothing on the target, since the library is static)
```

### Build

```sh
petalinux-build -c ramon-api
W=<proj>/build/tmp/work/<arch>/ramon-api/0.1.0-r0
grep -n "warning\|error:" $W/temp/log.do_compile          # expect nothing
grep -n "QA Issue" $W/temp/log.do_package* $W/temp/log.do_populate_sysroot 2>/dev/null
ls $W/image/usr/lib/libramon.a $W/image/usr/include/ramon/

petalinux-build -c ramon-tools
W=<proj>/build/tmp/work/<arch>/ramon-tools/0.1.0-r0
grep -n "warning\|error:" $W/temp/log.do_compile          # expect nothing
ls $W/image/usr/bin/ramon_cli $W/image/usr/bin/ramon_test

petalinux-build        # or scp the two binaries to the board's /tmp
```

`<arch>` is the work directory of target packages, e.g. `cortexa72-cortexa53-xilinx-linux`
(`ls <proj>/build/tmp/work/`).

### On the board, in this order

```sh
ramon_test --version
ramon_test --skip-spw --skip-spfi       # 1. core: buffers, channels, registers, ZDMA, stats
ramon_test spw-sync spw-version spw-dps spw-cancel
                                        # 2. SPW with the NN (no loopback)
ramon_test spw-loop axi-loop stats      # 3. SPW loopback write-read (see "Open points")
ramon_test --prep spfi-link spfi-stream spfi-perf spfi-rxtx spfi-alerts spfi-rules
                                        # 4. SPFI; add --format once if old streams are in the way
ramon_test                              # 5. everything
ramon_test --long spfi-maxlist          # 6. 2 x 5000-page writes (156 MiB of NN storage)
ramon_test --minutes 10 soak            # 7. mixed load
ramon_test --json > /tmp/run.json       # results and MiB/s figures for the record

ramon_cli                               # interactive: "help", TAB completes, Ctrl-D quits
ramon_cli spfitest auto 64 8            # the old spfitest
ramon_cli --plain                       # if the serial terminal shows escape sequences
```

Please send back the output of steps 1-5 (or the JSON line), and anything from `dmesg`.

### Checked here (host, x86_64)

- `make -C ramon-api/files check`: the library builds with `-std=c11 -Wall -Wextra -Wpedantic
  -Wshadow` without warnings, and `ramon_selftest` passes. It covers the CRC (`"123456789"` gives
  `0xCBF43926`, as the old `rc_crc32sw`), SPW packet build and parse with corrupted headers,
  payloads and footers, the SPFI table helpers, SYSMON and time-stamp decoding, and error
  formatting. It caught a wrong `l3_len` offset in the parser.
- A simulated recipe build with Yocto's hardening flags (`-D_FORTIFY_SOURCE=2
  -fstack-protector-strong -Werror=format-security`, `--as-needed`, also with `make -e`): no
  warnings. The install layout is as above. A C99 and a C++14 program build against the
  installed headers with `#include <ramon/ramon.h>`. This caught a real break: `make install`
  in `do_install` pointed at a header path that does not exist in the work directory.
- An emulator of `/dev/ramon_dma` (ioctls, mmap, SPW loopback, an NN that answers version, DPS,
  config and SW-update requests, SPFI streams): all 26 tests pass, including `--prep --format`,
  `--long`, a 1-minute soak and `--json`. The same runs are clean under AddressSanitizer +
  UBSan and ThreadSanitizer. A 79-command `ramon_cli` script is clean under ASan, and the
  interactive mode was driven through a pseudo-terminal (completion, hints, history, Ctrl-C,
  a terminal that reports no width). The emulator is not in the tree. It does not prove the
  FPGA's behaviour; it proves the code paths.

### Design points (please review)

- **Errors.** Every call returns 0 or `-errno` and fills an optional `struct ramon_status`: the
  driver's trailer, or a library code `RAMON_EL_*` (from 0x1000) for problems found before the
  ioctl. There is no global last error.
- **SPW receive.** Two modes, exclusive per NN: a library RX thread with one callback per NN,
  or the caller's own thread (`ramon_spw_recv`). Either way, every size from `SPW_WAIT_RX` is
  followed by exactly one RX transfer of that size. `ramon_spw_request` never polls. The old
  per-(node, app) user list is replaced by one callback that sees every packet; the old app-id
  filter was commented out anyway.
- **SPW loopback** changes reset the link. After switching loopback off, `ramon_spw_loopback`
  re-syncs the link before it returns. Link sync writes 0 to `0x1C`, as `ramon_smoke` does
  (`reset_value` in `struct ramon_spw_config` changes it).
- **SPFI.** `ramon_spfi_init` is optional; channels default to {5, 4}. After any SPFI timeout,
  the next SPFI call on that NN waits 500 ms, so a late answer is not taken for the next
  command's. OPEN passes the planned stream size as `stream_last_offset`, as the smoke tool and
  the old `spfitest` did; DATA_WRITE passes 0. The first page comes from the table once, then
  `struct ramon_spfi_stream` counts locally. Writes are limited to 5000 pages each.
- **AXI** `RAMON_E_AXI_PREP_FAILED` is retried once after 2 ms (see driver 0.8.8).
- **CMA** used by the library by default: 2 x 1 MiB TX buffers, plus 1 MiB of RX buffer per
  present SPW NN, plus 1 MiB per SPFI NN once a `*_mem` call is used. All sizes are in the
  config structs.
- **FDIR.** The 73-word snapshot and the automatic answer to attribute 0xA0 are in `ramon_cli`
  (`fdir`, `fdirauto`), not in the library. SPFI1's words are filled with 0xCAFECAFE while the
  driver keeps SPFI1 masked.
- **ramon_cli** guards `spfifmt`, `spfipdown`, `nnupdate`, `fwupdate` and `spfiprep ... format`
  with a final `yes`. The old tool had no guard on `spfipdown`.

### Open points

- **SPW loopback carrying data** has never run on the new driver: the smoke tool used loopback
  only to isolate the RX side. `spw-loop`, `axi-loop` and the SPW part of `stats` depend on it,
  and on the FPGA appending the 16-byte footer in loopback as it does for NN traffic. If they
  fail while `spw-version` passes, it is the loopback path.
  **Board result (tools 0.1.0):** both fail; the 0-byte packet never came back and
  `SPW_WAIT_RX` timed out. `ramon_cli loopdiag 0` (tools 0.1.2) tells the possible causes apart:
  the link needs a re-sync after loopback is switched on; looped data reaches the RX DMA without
  an RX-size interrupt (the old `xsgdma` never waited for a size, so it could not show this);
  or loopback returns nothing. The fix depends on which.
- **The largest SPW packet** the FPGA takes is unknown. `spw-loop` goes up to `--spw-max`
  (default 65536 bytes of payload); lower it if only the largest size fails.
- `nnupdate` and `nnconf` follow the old CLI's protocol, but have only met the emulator. Try
  `nnconf` before `nnupdate`.

## Acceptance test: ramon_accept (tools 0.2.0)

`ramon-tools` 0.2.0 adds a third program, `ramon_accept`: the manual delivery script
(`old_acceptance_test/DPC_master_delivery_v8.4.2_script.txt`) as automatic steps with a verdict
each and a summary to keep as the record. This first batch covers the four commands at the top
of the script. libramon and the driver are unchanged.

| Step | Was | Pass criterion |
|---|---|---|
| `rstop-version` | `rdrstopver` | 0x00/0x04/0x08/0x0C read; not 0.0.0; none 0xffffffff; equals `--rstop-version` if given |
| `rstop-timetag` | `readtimetag` | 0x10 decodes to a valid date and time (two samples, 1.1 s apart) |
| `sysmon` | `sysmon` | temperatures within limits (default -40..125 C); rails within the limits file, if any |
| `ddr4` | `ddr4test` | data bus, address bus, whole region with address and inverted-address patterns |

`ramon-tools/files/README.md` has the options, the limits file format and the DDR4 details.

### Build

```sh
cp -r ramon-tools <proj>/project-spec/meta-user/recipes-apps/   # replaces the 0.1.2 recipe
petalinux-build -c ramon-tools
W=<proj>/build/tmp/work/<arch>/ramon-tools/0.2.0-r0
grep -n "warning\|error:" $W/temp/log.do_compile          # expect nothing
ls $W/image/usr/bin/ramon_accept $W/image/usr/share/ramon-tools/accept_limits.example
```

### On the board, in this order

```sh
ramon_accept --version
ramon_accept rstop-version rstop-timetag sysmon      # 1. registers (seconds)
ramon_accept --ddr4-mb 64 ddr4                       # 2. DDR4 on 64 MiB: region, /dev/mem, speed
ramon_accept ddr4                                    # 3. the whole PL DDR4 (minutes)
ramon_accept --rstop-version <X.Y.Z> | tee /tmp/accept.log    # 4. everything, as a record
```

Please send back the output of 1 and 2 (step 2 prints the DT node, region, and MiB/s, which
tell how long step 3 will take), and the summary of 3.

### Checked here (host, x86_64)

- Builds with `-std=c11 -Wall -Wextra -Wpedantic -Wshadow` without warnings, together with
  `ramon_cli` and `ramon_test`.
- Register steps against an `LD_PRELOAD` shim answering GET_INFO and REG_IO: good board; version
  mismatch, 0xffffffff and 0.0.0; time tag all-ones, zero, counting, and a second sample that
  does not decode; SYSMON with the example file, a good file, a rail out of range, a hot sensor,
  a malformed file (bad line, unknown name, min > max, extra field) and a missing file.
- `ddr4` against a fake `/proc/device-tree` and a sparse file standing in for `/dev/mem` at
  0x5_0000_0000: label `ddr4_0`; fallback to one label containing `ddr4`; two labels of one
  node; two nodes (refused, candidates listed); `--ddr4-label`; no `__symbols__`;
  `#size-cells` 1 and 2; a truncated `reg`; `--ddr4-base/--ddr4-size`; `--ddr4-mb`; an
  unaligned base; a missing memory device. The file holds the inverted address pattern at the
  right offsets afterwards.
- Fault injection: a region whose upper half maps the lower half's pages (an address line stuck
  low) is reported by the address-bus check as "address bit 22 ... stuck", and the full pass
  reports half the words wrong with the faulty bit in the XOR column.
- AddressSanitizer + UBSan over all of the above: clean.

### Open points

- **/dev/mem.** The DDR4 step reaches the PL DDR4 through `/dev/mem`, as the old test did. It
  works on a kernel with `CONFIG_DEVMEM=y` and without `CONFIG_IO_STRICT_DEVMEM` claiming the
  range (`CONFIG_STRICT_DEVMEM` alone allows it, since the PL DDR4 is not System RAM). Is that
  true of the hardened image? If not, the alternative is a driver change: `ramon_dma` maps the
  `ddr4_0` region itself and offers it through `mmap`.
- **DDR4 calibration.** The step checks that the FPGA manager reports `operating` before touching
  the PL. If the FPGA exposes the MIG's `init_calib_complete` in a register, the step should
  check it too: an access to an uncalibrated DDR4 can hang the bus.
- **RS-TOP 0x10.** Is it the FPGA build time stamp (fixed) or a running time tag? The step accepts
  both and says which it saw.
- **SYSMON limits.** No rail is checked until `/etc/ramon/accept_limits.conf` lists it. The voltage
  lines in `accept_limits.example` are datasheet starting points for 0.85 V parts, commented out
  until someone checks them against the fitted part and the schematic.

## Acceptance batch 2: SpaceWire (tools 0.3.0)

Batch 1 (`rstop-version`, `rstop-timetag`, `sysmon`, `ddr4`) passed on the board with tools
0.2.0. Batch 2 adds three SPW steps and three NN service steps; libramon and the driver are
unchanged. `nn-config` and `nn-update` change the NN, so they run only when their file option
is given, also in a run without step names.

| Step | Was | Pass criterion |
|---|---|---|
| `spw-link` | `spwinit` | the links to test sync |
| `spw-nn-loop` | `loopback 0` + `spwsend ... +68 100` | FPGA loopback off, packets to node 68 routed back by the NN: all back once, intact, in order, no RX overrun |
| `spw-fpga-loop` | `loopback 1` + `spwsend` | the same with FPGA loopback on |
| `nn-version` | `nnswver` | each tested NN answers with its versions; image contains `--nn-expect` if given |
| `nn-config` | `nnconf` | configuration acknowledged (0x101); only with `--nn-config [NN=]FILE` |
| `nn-update` | `nnupdate` | every update packet accepted; only with `--nn-update [NN=]FILE`; runs last, power-cycle after |

Defaults follow the delivery script: payloads of 1 and 4096 bytes, 10000 packets each,
application type 100, node 68. Unlike the old `spwsend`, at most 32 packets are in flight
(`--spw-window`), and every returned packet is checked, not only counted.

### Build

```sh
cp -r ramon-tools <proj>/project-spec/meta-user/recipes-apps/
petalinux-build -c ramon-tools
W=<proj>/build/tmp/work/<arch>/ramon-tools/0.3.0-r0
grep -n "warning\|error:" $W/temp/log.do_compile          # expect nothing
```

### On the board, in this order

```sh
ramon_accept spw-link spw-nn-loop                    # 1. far-end loopback through the NN
ramon_accept spw-fpga-loop                           # 2. FPGA loopback
ramon_accept --spw-sizes 1,7,64,1023,4096 --spw-seconds 30 spw-nn-loop
                                                     # 3. more sizes, 30 s each
ramon_accept nn-version                              # 4. NN versions
ramon_accept --nn-config 0=<config.json> nn-config   # 5. NN configuration
ramon_accept --nn-update 0=<package.pkg> nn-update   # 6. NN update, then power-cycle
ramon_accept --nn-expect <new image version> nn-version     # 7. after the power cycle
ramon_accept --rstop-version <X.Y.Z> | tee /tmp/accept.log    # 8. batches 1 and 2 together
```

Please send back the output of 1, 2 and 4-7, and the throughput lines of 3.

### Checked here (host, x86_64)

- Builds without warnings, also with Yocto's hardening flags.
- An `LD_PRELOAD` fake of `/dev/ramon_dma` with buffers, `mmap`, AXI transfers,
  `SPW_WAIT_RX`/`SPW_CANCEL`/`SPW_LOOPBACK`, SPW registers and an NN that routes packets
  addressed to node 68 back (FPGA loopback returns everything): all seven steps pass together.
- Faults injected in the fake, each reported as such: a dropped packet (lost, and out of order
  at the next one), a corrupted payload (payload CRC), two packets swapped (out of order; the
  late one is reported in the next size, not counted there), a duplicate (repeated), footer
  error flags, FPGA loopback or NN returning nothing (lost after 2 s, no hang), a slow
  receiver with a short RX queue (driver overrun; passes with `--spw-window 4`), no link, an
  NN named in `--spw-nn` whose link is down, both NNs synced, `--spw-seconds` with sizes 0 to
  6200, and 150000 packets (the 16-bit packet id wraps twice).
- The fake NN also answers version, configuration and software-update requests: versions
  printed and `--nn-expect` matched or refused; empty version strings and a silent NN fail;
  configuration acknowledged, refused (attribute 0x102), too large, missing; per-NN files on
  two NNs, and a file for an NN whose link is down; update accepted (and the new image version
  seen by the next `nn-version`), refused at the start ("application running"), refused at
  the end ("bad package CRC"), NN silent mid-update (3 s timeout), empty package; malformed
  `NN=FILE` options refused.
- ThreadSanitizer, AddressSanitizer and UBSan over those runs: clean.

### Open points

- **nn-update and nn-config** have still only met the emulator (as with `ramon_cli`); try
  `nn-config` first. The progress lines tell how long an update takes on the real NN.
- **FPGA loopback carrying data** failed with tools 0.1.0 (see the phase 2 open points), and the
  `loopdiag` result has not come back yet. If `spw-fpga-loop` fails while `spw-nn-loop` passes,
  please send `ramon_cli loopdiag 0`.
- **Window.** 32 packets in flight keeps the driver's RX queue (64) from overrunning. If the
  delivery requires the old unpaced burst, `--spw-window 100000` reproduces it; an overrun then
  fails the size.

### Tools 0.3.1: SPW link sync as long as the old spw_init()

- **Board result (0.3.0):** `ramon_accept spw-link` failed on both links, while NN A syncs with
  the old driver and tools.
- **Cause:** the reset sequence is the same in both stacks (while `(0x28 & 7) != 5`, write 0 to
  `0x1C`, sleep 10 ms), but `ramon_accept` gave up after 200 resets (2 s) to spare the uncabled
  NN B. The old `spw_init()`, `ramon_smoke`, `ramon_cli spwsync` and libramon's default all try
  1000 times (10 s). NN A evidently needs more than 2 s.
- **Fix:** 1000 resets by default, as the old tool; `--spw-sync-tries N` changes it. With
  `--spw-nn`, only the named NNs are synced, so `--spw-nn 0` does not spend 10 s on NN B.
  `spw-link` now prints how many resets and seconds NN A needed, or the raw `0x28` when it gave up.
- Checked here: a fake link that syncs after 500 resets passes with the default and fails with
  `--spw-sync-tries 200` (the 0.3.0 behaviour, both links down as on the board).
- `ramon_test` still uses 200 resets; if its `spw-sync` fails the same way, it has the same cause.

On the board:

```sh
ramon_cli spwsync 0                  # confirms first: 1000 resets on 0.3.0 already
ramon_accept --spw-nn 0 spw-link     # 0.3.1: prints "synced after N reset(s), T s"
```

### libramon 0.2.0, tools 0.3.2, smoke 0.8.16: SPW core enable (0x2C)

- **Board finding (user):** the SPW core must be enabled by writing 1 to SPW register `0x2C`
  (`spw_reg_io(nn, 1, 0x2C, 1)`) when the application starts SPW. Without it the link never syncs,
  however often `0x1C` resets it. Neither the old code nor the new wrote `0x2C`; that explains
  `spw-link` and `ramon_smoke spw` failing on a clean `ramon-dma` image (any earlier syncs came
  from a core that something else had already enabled).
- **libramon:** new `ramon_spw_enable(c, nn, st)` writes 1 to `RAMON_SPW_REG_ENABLE` (`0x2C`).
  `ramon_spw_init()` calls it for every NN it sets up; `ramon_spw_sync()` calls it before its
  first reset, so `ramon_cli spwsync` works without a full init. Library version 0.2.0.
- **ramon_smoke:** its own sync loop writes `0x2C = 1` first.
- **ramon_cli spwstatus** prints `0x2c` with the other SPW registers.
- **Driver unchanged:** the enable stays in user space, as asked.
- Checked here: the fake device now refuses to sync until `0x2C` is 1. With it, `ramon_accept`
  (`spw-link`, both loop steps, `nn-version`), `ramon_cli spwsync`/`spwinit`/`spwstatus` and
  `ramon_smoke spw` (sync + version round trip) pass; a write trace shows `0x2C = 1` before the
  first `0x1C` reset. `make check` passes; builds clean, also with Yocto's hardening flags.

Build all three (ramon-tools links libramon statically, so it must be rebuilt after ramon-api):

```sh
cp -r ramon-api ramon-tools ramon-smoke <proj>/project-spec/meta-user/recipes-apps/
petalinux-build -c ramon-api && petalinux-build -c ramon-tools && petalinux-build -c ramon-smoke
```

On the board:

```sh
ramon_accept --version                 # 0.3.2, libramon 0.2.0
ramon_cli spwstatus 0                  # 0x2c=0x1 once anything has initialised SPW
ramon_smoke spw
ramon_accept --spw-nn 0 spw-link spw-nn-loop spw-fpga-loop nn-version
```

### libramon 0.2.1, tools 0.3.3: re-sync after FPGA loopback on; link rate in Mbit/s

- **Board result (tools 0.3.2):** `spw-nn-loop` passes; `spw-fpga-loop` fails.
- **Cause (most likely):** a loopback change takes effect when the link syncs again. The old
  script says so ("loopback must be set before spwinit ... the loopback register is written to PL
  when spwinit command is issued"), and the old flow always ran `loopback 1`, then `spwinit`.
  `ramon_spw_loopback()` re-synced only after switching loopback *off*; after switching it *on*
  it waited 10 ms and returned, so the link never ran in loopback and nothing came back. This is
  probably also the "FPGA loopback carrying data" open point from tools 0.1.0: `ramon_test
  spw-loop`/`axi-loop` and `ramon_cli spwloop`/`loopback` use the same call.
- **Fix:** `ramon_spw_loopback()` runs `ramon_spw_sync()` after any change, so with loopback on
  the link syncs with itself; a failure to sync is returned. `ramon_smoke` uses loopback only to
  keep NN traffic away during its wait/cancel checks and needs no change.
- Checked here: the fake device now applies a loopback change only after the link re-syncs and
  sends nothing while the link is down. With 0.2.0's loopback code `spw-fpga-loop` loses every
  packet ("lost 32, back 0", as expected on the board); with 0.2.1 it passes.
- **Link rate.** `spw-nn-loop` measured 228 packets/s at 4096 bytes: 228 x (4096 + 40 + 1 EOP)
  x 10 bits = 9.4 Mbit/s, i.e. the 10 Mbit/s every SpaceWire link starts at; 1-byte packets give
  ~4.8 Mbit/s (per-packet overhead). The link seems to stay at its start rate. Question for the
  FPGA team: what rate should NN A run at, and is it set by a register? Neither the old nor the
  new code writes one. The loop steps now print "Mbit/s on the link" (10 bits per character,
  header + payload + EOP) to compare with that rate.

Build: `petalinux-build -c ramon-api && petalinux-build -c ramon-tools` (0.2.1 / 0.3.3).
On the board:

```sh
ramon_accept --version                            # 0.3.3, libramon 0.2.1
ramon_accept --spw-nn 0 spw-link spw-fpga-loop spw-nn-loop
ramon_test spw-loop axi-loop                      # the 0.1.0 loopback open point, same fix
```

## Acceptance batch 3: SPFI (tools 0.4.0)

Nine SPFI steps in `accept_spfi.c`, following the delivery script. libramon and the driver are
unchanged (libramon 0.2.1 from the SPW fixes).

| Step | Was | Pass criterion |
|---|---|---|
| `spfi-link` | `spfidrvinit`, `spfireg +204` | `0xCC` low byte `0x88` |
| `spfi-prep` | `spwdps 0`, `spfiinit 0`, `spfifmt`, `spfiginfo 1` | DPS 0x101, INIT answered, FORMAT (`--spfi-format`), table read, room for 2 test streams |
| `spfi-stream` | #2: 150 x 1000, verify | every page read back intact |
| `spfi-perf` | #3, #5: 150 x 1000, 5000 x 100 | no errors; MiB/s |
| `spfi-large` | #4: 5000 x 100, verify | every page intact |
| `spfi-odd` | #10-#12: 257, 4999, 1 x 100, verify | every page intact |
| `spfi-rxtx` | `spfirxtx` #17-#20 | no errors, reads intact; MiB/s |
| `spfi-long` | #13: 5000 x 200 (`--spfi-long`) | no errors; MiB/s |
| `spfi-soak` | `spfirxtx` #21-#24 (`--spfi-soak`) | no errors, reads intact; cycles over 1 s counted |

Differences from the old flow, on purpose:

- **Streams.** Each test opens new streams on free ids and closes and deletes them, also after a
  failure. The old script reused fixed ids and only closed them, so a later run needed FORMAT.
- **FORMAT** only with `--spfi-format`, since it erases the NN's streams. Without it `spfi-prep`
  fails if fewer than 2 ids are free or 7 or 8 streams are open, and says so.
- **Verification.** Every page carries a 16-byte stamp (stream, run, page), so a read of the wrong
  run or page fails too. The old `spfirxtx` did not check the read data; `spfi-rxtx` and
  `spfi-soak` check the last read of each size in every cycle, outside the timed part.
- **Old `spfirxtx` arguments** map to `{wpages1, wpages2}`, `{rpages1, rpages2}`,
  `{wnum1, wnum2}`, `{rnum1, rnum2}`, cycles; cycles run back to back and their time is reported
  against 1 s ("slack"), as before.

### Build

```sh
cp -r ramon-tools <proj>/project-spec/meta-user/recipes-apps/
petalinux-build -c ramon-tools        # 0.4.0 (ramon-api 0.2.1 must be built first)
```

### On the board, in this order

```sh
ramon_accept --version                                         # 0.4.0, libramon 0.2.1
ramon_accept spfi-link
ramon_accept --spfi-format spfi-prep                           # erases the NN's streams
ramon_accept --spfi-scale 1 spfi-stream spfi-perf spfi-large spfi-odd spfi-rxtx   # quick: 1%
ramon_accept spfi-stream spfi-perf spfi-large spfi-odd spfi-rxtx                  # full
ramon_accept --spfi-long --spfi-soak spfi-long spfi-soak      # about 45 minutes
```

Please send the output of the quick run first: its MiB/s figures tell how long the full run takes
(the full default steps move about 60 GB through the NN).

### Checked here (host, x86_64)

- Builds without warnings, also with Yocto's hardening flags; all 15 default steps pass together
  against the fake device.
- The fake NN stores streams of 16 KiB pages (each distinct page body once), answers INIT, FORMAT,
  OPEN, FLUSH, CLOSE, DELETE, GET_ALL and the table, DATA_WRITE and READ_STREAM, and DPS over SPW.
- Faults, each reported: a corrupted page ("data differs"), the wrong page returned ("stamp
  differs", with the stamp read), SPFI link down, a data write that fails (the stream is still
  deleted: no streams left on the NN), 7 streams left open (prep and the concurrent test refuse;
  `--spfi-format` clears it), DPS refused; `spfi-long`/`spfi-soak` join a run without step names
  only with their flags; `--spfi-scale 1` soak: 7 and 70 cycles with per-cycle times.
- ThreadSanitizer, AddressSanitizer and UBSan over those runs: clean.

### Open points

- The fake does not prove the NN's behaviour. In particular: the NN's storage limit (the old
  script notes stream #16, 5000 x 254 = 20 GB in one stream, "fails vs. NN"); the 5000 x 100
  tests write 8 GB per stream.
- The two soak runs write about 220 GB each into one stream, as the old script did. If the NN runs
  out of space, the write fails and the step reports it.

### Tools 0.4.1: SPW test packets within --spw-max (default 4096)

- **Board result (tools 0.4.0, libramon 0.2.1):** `ramon_test spw-loop` now gets packets back
  through FPGA loopback (the re-sync fix of libramon 0.2.1 works): payloads 0..4096 intact. The
  16384-byte packet came back with a bad payload CRC while xilinx_dma reported `errors 10` (AXI
  DMA internal error, a packet that does not fit what the engine expects), and the next TX on
  channel 1 timed out: the oversize packet wedged the receive path.
- **Cause:** `ramon_test` ignored `--spw-max` in three places: the size sweep always included
  16384, the throughput burst used 16 KiB packets, `axi-loop` 64 KiB packets; the default
  `--spw-max` was 65536. The delivery script never goes above 4096 (and notes 6200 as working).
- **Fix:** `--spw-max` (default now 4096) bounds every packet of `spw-loop` (sweep, burst,
  throughput) and the `axi-loop` packet.
- Checked here: a fake FPGA that corrupts a packet above 6240 bytes and then returns nothing
  reproduces the board output (`packet 6: flags 0x2, payload 16384`); with 0.4.1 defaults
  `spw-loop` and `axi-loop` pass.
- **Questions for the FPGA team:** the largest SPW packet the FPGA handles, and what an oversize
  packet should do. Today it comes back corrupted and leaves the receive DMA stuck until the
  axi_dma IP is reset. `ramon_accept --spw-sizes` allows larger sizes for probing; keep them
  within that limit for acceptance.

```sh
petalinux-build -c ramon-tools          # 0.4.1
ramon_test spw-loop axi-loop
ramon_test --spw-max 6200 spw-loop      # the largest size the old script noted as working
```

### libramon 0.3.0, tools 0.4.2, smoke 0.8.17: SPW packets above 4096 bytes are dropped

- **User (FPGA):** the largest SPW packet is 4096 bytes of payload; a larger one must be dropped
  with a message.
- **Send:** `ramon_spw_send()` and `ramon_spw_send_buf()` refuse a payload above
  `RAMON_SPW_MAX_PAYLOAD` (4096) before anything reaches the DMA: `RAMON_EL_TOO_LARGE`, "spw0:
  payload of 4097 bytes is above the SPW maximum of 4096; packet dropped, not sent", counter
  `tx_too_large`. `ramon_cli axiloop` (raw AXI) checks the same; `ramon_smoke`'s own packet builder
  too. `ramon_accept --spw-sizes` and `ramon_test --spw-max` accept at most 4096.
- **Receive:** a packet whose payload is above 4096 is still taken off the FPGA (the driver's
  next size belongs to the next packet), then dropped with a warning ("spw0: dropped a received
  packet of 5056 bytes: payload above the SPW maximum of 4096"), counter `rx_too_large`; a caller
  receiving in its own thread gets `RAMON_EL_TOO_LARGE`, drain only counts it. `ramon_cli
  spwstatus` prints both counters.
- **Exception, to confirm with the FPGA team:** the NN software update keeps the old tool's packet
  sizes: data packets carry 4100 payload bytes (4-byte prefix + 4096-byte chunk) and the end
  packet 4104 (8-byte prefix + chunk). The old `cli_sw_update()` sent `sizeof(sw_update_data_t)`
  and `sizeof(sw_update_end_t)`, i.e. the same sizes, and its updates worked; the corruption we saw
  was on the receive side, with a 16 KiB packet. If the FPGA's limit also applies to these, the
  update chunk has to shrink, which changes the NN's update protocol.
- Checked here (fake device): 4096 sent, 4097 refused and counted; `axiloop` with a 4137-byte
  packet refused; an NN answer of 5000 payload bytes dropped with the warning, both when the
  request receives in its own thread (the request fails with the drop message) and with the RX
  thread (warning, then the request times out); `--spw-sizes 4097` and `--spw-max 5000`
  refused; `nn-update` passes with 4100/4104-byte packets. `make check` passes; builds clean, also
  with Yocto's hardening flags.

Build `ramon-api`, then `ramon-tools`, then `ramon-smoke`.

### Driver 0.8.9: an SPFI node with one reg entry gives SPFI0

- **Board result (new image, only `ramon-dma`):** `ramon_cli info` showed `spfi_mask 0x0`, and the
  driver logged `spfi: node /amba_pl@0/nanrec_spfi_wr@a0050000 has 1 reg entries, need 2; SPFI
  disabled`. The new device tree describes SPFI0 only (SPFI1 is not used on this board).
- **Cause:** discovery wanted `reg[0]` and `reg[1]` (and both interrupts) and disabled SPFI
  altogether otherwise.
- **Fix:** per NN. `reg[n]` enables SPFI NN n (its register window); `interrupts[n]` enables its
  commands. A node with one `reg` entry gives SPFI0, logged at info level ("has 1 reg entry; SPFI1
  and above absent"); a node with none still disables SPFI with a warning. The DT memory labels
  (TX read-offset and RX message tables) are looked up only for the NNs the node describes, so
  the absent NNB labels no longer warn. The `spfi_mask` module parameter (default 0x1) still
  decides which NNs are used.
- **Not compiled here** (no kernel tree). No new kernel symbols, so `ramon_dma.ksyms` is unchanged.
- **ramon_accept 0.4.3:** with `spfi_mask 0x0` the SPFI steps now say "the driver reports no SPFI
  NN (GET_INFO spfi_mask 0x0): see dmesg" instead of "no SPFI link up".

```sh
petalinux-build -c ramon-dma            # 0.8.9; then reboot or rmmod/insmod
dmesg | grep -i spfi                    # "spfi0 ...: irq N", "has 1 reg entry; SPFI1 and above absent"
ramon_cli info                          # spfi_mask 0x1
ramon_accept spfi-link spfi-prep
```

### libramon 0.3.1, tools 0.4.4, smoke 0.8.18: SPFI write channel 4 on the current design

- **Board result (new image):** `ramon_accept spfi-link` failed in `ramon_spfi_init`: "spfi0:
  channel 5 is not an AXI MEM_TO_DEV channel". The current FPGA design has one SPFI write channel,
  AXI channel **4** (user); index 5 is now past the AXI channels (a ZDMA channel).
- **Fix:** the default write channel is 4 for both NNs (`RAMON_SPFI_CHAN_NN0/NN1`, user decision);
  `ramon_smoke` the same. `--spfi-chans A,B` overrides it in `ramon_accept` (new), `ramon_test`,
  `ramon_cli` and `ramon_smoke`; the earlier design needs `--spfi-chans 5,4`.
- `spfi-link` now judges the link from the register it just read (it printed "down" whenever
  `ramon_spfi_init` failed) and fails a down link explicitly; the library's channel message is
  shortened to fit the status (it was cut off at 96 characters).
- Checked here: a fake with the current layout (AXI 0-3 SPW, 4 SPFI write, ZDMA from 5)
  reproduces the board error with `--spfi-chans 5,4` and passes `spfi-link`, `spfi-prep`,
  `spfi-stream`, `spfi-odd`, `spfi-rxtx` and `ramon_test spfi-stream` with the default, also with
  both NNs enabled on channel 4.
- **Open:** the same board run read `0xCC = 0x44`, low byte not `0x88`, so the SPFI0 link is down.
  On the earlier design `0xCC` read `0x4488`, i.e. `0x44` was the other (down) half. See the
  questions in the session notes: does SPFI need an enable on the new design, as SPW does with
  `0x2C`?

### libramon 0.4.0, tools 0.4.5, smoke 0.8.19: SPW before the SPFI link check

- **Board result:** `ramon_accept spfi-link` read `0xCC = 0x44` (link down), while SPFI worked in
  `ramon_smoke`. Review: `ramon_smoke` checks the link the same way, but its default sequence runs
  `spw` (SPW core enable `0x2C` + link sync) before `spfi`, as the delivery script ran `spwinit`
  before `spfireg +204`. On the current design the SPFI link comes up only after the SPW core of
  the same NN is enabled and synced. `ramon_accept spfi-link` did not touch SPW;
  `ramon_spfi_prep()` (`ramon_cli spfiprep`, `ramon_test --prep`, `ramon_accept spfi-prep`) and
  `ramon_smoke spfiprep` checked the SPFI link *before* their SPW sync, so all of them fail after a
  fresh boot unless SPW was set up first.
- **Fix:** SPW first, then the SPFI link, waited for up to 2 s:
  - libramon: `ramon_spfi_prep()` runs SPW init + sync, then the new
    `ramon_spfi_wait_link(c, nn, timeout_ms, &raw, st)`, then DPS and INIT;
  - `ramon_accept`: the SPFI steps set up SPW (only the SPFI NNs' links unless `--spw-nn` is
    given, so an uncabled NN costs no 10 s) and wait for each SPFI link;
  - `ramon_smoke spfiprep`: SPW enable + sync, then the link (waited for), then DPS.
- Checked here: a fake whose SPFI link reads `0x44` until the SPW core is enabled and synced, then
  `0x88`; from a fresh state `ramon_accept spfi-link` (0.2 s), `spfi-prep`, `ramon_cli spfiprep 0`
  and `ramon_smoke spfiprep 0` pass; the full default run (15 steps) and an SPFI-only run pass.

```sh
petalinux-build -c ramon-api && petalinux-build -c ramon-tools && petalinux-build -c ramon-smoke
ramon_accept spfi-link                                  # after a fresh boot
ramon_accept --spfi-format spfi-prep spfi-stream
```
