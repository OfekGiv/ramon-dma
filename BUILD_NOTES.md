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
| 7 | SPFI: IRQ, CMD, WRITE, READ, alerts, MEM_READ, TX_OFFS_WRITE | 0.8.0 built and loaded; `spfiprep 0` passes (INIT, GET_ALL_STREAM_STATUS, MEM_READ, SPFI REG_IO). Stream write/read (`spfi`), `--errors`, `unbind`, `--stress` pending |
| 8 | remove hardening, `--stress`, `--errors` sweep (GET_STATS came with batch A) | 0.8.0 built; runs pending |
| 9 | README, final petalinux-build + autoload boot (recipes already exist) | not started |

The driver version is `0.<step>.0` while the rewrite is in progress, so
`cat /sys/module/ramon_dma/version` tells which step is on the board. Steps are now delivered in
batches to save file transfers (batch A = steps 4-6, version 0.6.0; batch B = steps 7-8; batch C = 9).

---

## Installing the recipes in the PetaLinux project (once)

This tree holds two recipes. Copy each recipe directory, including its `files/`, into `meta-user`:

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
| `XILINX_DMA_NUM_DESCS`, `ZYNQMP_DMA_NUM_DESCS` | The AXI item limit (512) and the ZDMA chunk size (16, assuming 32 descriptors) | 4, 6 |
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
  - bad channel/index, 0 and 513 items, a bad items pointer, `len 0`, an unknown handle, an
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

- **Generic `RAMON_E_CANCELLED` and `RAMON_E_TIMEOUT` have no user.** Every cancel and timeout
  has a subsystem-specific code, and the spec wants every code used. Should I remove them before
  the ABI is frozen? No other code changes number.
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
