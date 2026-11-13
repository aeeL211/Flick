# Verification results

Status: complete. Branch `perf/axmanager-aosp-optimization`, package `Flick-v0.9.11.zip`.
This document records the before/after evidence for the work described in
[`OPTIMIZATION_PLAN.md`](OPTIMIZATION_PLAN.md). It was written after the package-producing workflow
was green and the downloaded artifact had been checked locally.

## 1. Result

- The required test job and the build/package job are green on GitHub Actions.
- The touch math is unchanged: host, sanitizer, and both ARM cross-builds pass every reference case
  with worst error `0`.
- The daemon no longer walks `/proc` on every 3 s tick while a target is already foreground. The
  scan test shows one initial scan, no scan while the state is stable, and exactly one rescan after
  the foreground period.
- The WebUI stops polling while hidden, serializes status calls, repaints only the status area on a
  poll, repaints one slider during a drag, and debounces package search.
- The package is deterministic and checksum-verified. The downloaded ARM64 selftest from the same
  run was executed on an aarch64 Android device and passed.
- No configuration key, default, filter coefficient, or user-facing control was removed or renamed.
  The only intentional user-visible behavior changes are the post-grab finger re-check and the
  version bump.

## 2. Baseline and final artifacts

| Item | Baseline | Final |
|---|---|---|
| Commit | `8d9b390` (`main`) | `84b2919` (packaged source) |
| Workflow run | [41944937637](https://github.com/aeeL211/Flick/actions/runs/41944937637) | [43380422292](https://github.com/aeeL211/Flick/actions/runs/43380422292) |
| Plugin version | `v0.9.10`, code `28` | `v0.9.11`, code `29` |
| Package | `Flick-v0.9.10.zip`, 42,677 bytes | `Flick-v0.9.11.zip`, 43,330 bytes |
| Package SHA-256 | `24d82a15f1420b148ea6f8d57fd2b82efa8f7fce60ea6ece1569f0a01c1193cc` | `eb4320b11570ef160f21d2b66371286cf867bf0c6b38f25b0e1891c4804e1690` |
| NDK | runner default | `27.3.13750724` |
| Compiler mode | `-Os`, no hardening/page-size flags | `-Os` plus hardening and 16 KB alignment |

The package was downloaded from the `Flick` artifact of run `43380422292` and checked with
`sha256sum -c`. The later documentation/CI runs `43381083297` and `43381304008` reproduced the same
zip hash, so those commits did not change the package. The artifact contains the zip, its `.sha256`,
the manifest, `build-info.txt`, and the size/flag metrics.

The zip timestamp is pinned to the last `module.prop` change, not to the latest commit. The build job
checks out the full history so `git log` can resolve that change; a shallow checkout would treat
`HEAD` as the last change. Documentation and CI-only commits therefore leave the packaged bytes and
the hash unchanged.

## 3. Build and package size

All binary numbers are from the downloaded final artifact; `text` is the `llvm-size` value reported
by the workflow.

| Metric | Baseline | Final | Delta |
|---|---:|---:|---:|
| Package bytes | 42,677 | 43,330 | +653 (+1.53%) |
| `flick.arm64` stripped | 19,360 | 20,056 | +696 (+3.60%) |
| `flick.arm` stripped | 19,464 | 20,364 | +900 (+4.62%) |
| text, arm64 | 15,678 | 16,643 | +965 (+6.15%) |
| text, arm | 16,964 | 18,107 | +1,143 (+6.74%) |
| `webroot/index.html` | 55,432 | 57,218 | +1,786 (+3.22%) |

The increase is the cost of the safety re-check, the foreground-period bookkeeping, stack protection,
`_FORTIFY_SOURCE`, PIE, and the WebUI polling/render changes. The installed payload remains about
103 KB uncompressed. The full package is only 1.5% larger than the baseline.

## 4. Correctness evidence

The reference values in `selftest.cpp` were not edited. Every scenario reports worst error `0`:

| Check | Result |
|---|---|
| Host selftest `-O2` | `SELFTEST PASS` |
| Host selftest ASan + UBSan | `SELFTEST PASS` |
| ARM64 cross-built selftest | `SELFTEST PASS` (executed on the aarch64 device) |
| ARM cross-built selftest | `SELFTEST PASS` |
| Curve / sensitivity / frame factor / One Euro / rounding | exact or within the documented bounds |
| Five full scenarios (accel, decel, half-zone, filter, everything off) | all pass, worst `0` raw |
| `SYN_DROPPED` handling | passes |

`proc_test.cpp` uses a fake `/proc` root so the real host process table is never touched. It proves
the intended scan behavior:

```text
PASS one full scan finds the target
PASS no scan while already foreground
PASS one rescan after the foreground period
PASS pid cache is fresh after the transition scan
PASS incremental scan finds the new pid
SELFTEST PROC PASS
```

The first call scans once. The second call, with the same process still foreground, performs no
scan. Moving the process to the background causes exactly one full rescan; a new foreground process
is then found by the incremental scan. A full rescan after a skipped foreground period also clears
the stale PID cache, so PID reuse cannot hide a later launch.

## 5. Runtime work

### 5.1 Foreground detection

| Item | Baseline | Final |
|---|---|---|
| Tick | 3000 ms | 3000 ms |
| `/proc` walks per 60 s while target stays foreground | 20 | 1 (the initial scan) |
| `/proc` walks while foreground state is stable | every tick | 0 |
| Full rescan after foreground period | not applicable | exactly 1 |

This is a modeled count from the unchanged 3 s tick and is confirmed by the scan counter in
`proc_test.cpp`; it is not a device power measurement. The daemon still performs the incremental
scan while idle, so a target launch is detected on the next tick.

### 5.2 Grab safety

`activate()` checks `touching(fd)` before `EVIOCGRAB` and now checks again immediately after the
grab. If a finger landed in the gap, the grab is released at once and activation fails with the
touch error, before the daemon processes any touch event. This narrows the previous race to the
ioctl itself and keeps the existing retry/idempotent activation contract.

### 5.3 Compiler mode

The host microbenchmark processes complete `SYN_REPORT` frames through `handleEvent`/`closeFrame`.
The package-producing run measured:

| Mode | ns/event | Relative |
|---|---:|---:|
| `-O2` | 25.33 | baseline |
| `-Os` | 24.63 | 2.8% faster |

An earlier run measured 25.12 (`-O2`) and 25.14 (`-Os`). The two runs disagree in both directions,
so neither shows a repeatable `-O2` advantage. `-Os` is retained for the smaller artifact. Both
modes produced the same math checksum (`535020526216600`). This is a host compiler comparison, not
a phone FPS measurement.

### 5.4 WebUI

The changes are small and local; they do not alter the controls or the saved config format.

| Area | Baseline | Final |
|---|---|---|
| Polling while tab hidden | interval kept running | interval cleared on `visibilitychange` |
| Resume | next tick | one immediate status refresh, then interval |
| Overlapping status calls | possible | serialized through one promise chain, `refreshActive` guard |
| 3 s poll render | full `render()` | `renderStatus()` only |
| Slider drag | full `render()` per pointer move | one slider (plus linked slider) repainted |
| Package search | `drawPick()` per input event | 120 ms debounce |

`python3 tools/check_webui.py index.html` extracts the inline script and runs `node --check`; it
passed in CI (`index.html: inline JavaScript syntax OK`). The WebUI claims above are verified by
code inspection and the syntax gate, not by a browser runtime profile.

## 6. Script and AxManager alignment

- `customize.sh` derives the plugins root from `MODPATH` (the parent of the current plugin
  directory), with the `AXERONDIR/plugins` fallback only when `MODPATH` is not absolute. It aborts
  if no root can be found.
- `hw.sh` uses `/system/bin/getprop` and `/system/bin/setprop`.
- `notify.sh` uses `/system/bin/cmd` and an explicit `IFS='='`.
- `uninstall.sh` uses `/system/bin/sh`.
- All scripts pass `sh -n`; ShellCheck 0.11.0 passes at warning severity in CI.

The legacy `debug.sf.hw` enhancer remains opt-in. Its effect is device-dependent and `setprop` can
be denied in shell mode; the previous value is preserved and restored.

## 7. Build hardening and AOSP checks

The daemon is built with:

```text
-std=c++17 -Os -DNDEBUG -D_FORTIFY_SOURCE=2
-ffp-contract=off -fno-exceptions -fno-rtti -fvisibility=hidden
-fstack-protector-strong -fno-unwind-tables -fno-asynchronous-unwind-tables
-ffunction-sections -fdata-sections -ffile-prefix-map=<work>=.
-pie -nostdlib++
-Wl,--gc-sections -Wl,--as-needed -Wl,--build-id=none
-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,max-page-size=16384
```

The workflow rejects a binary that is not PIE/DYN, lacks `BIND_NOW` or `GNU_RELRO`, has `TEXTREL`,
has `RPATH`/`RUNPATH`, has an executable stack, links anything other than `libc`/`libm`/`libdl`, or
has a first `LOAD` alignment below 16 KB. The final logs show `BIND_NOW`, `GNU_RELRO`, a non-exec
`GNU_STACK`, and `LOAD ... 0x4000` (16 KB) for both ABIs. Android remains API 24 minimum; the 16 KB
maximum page size also keeps the binaries valid on 4 KB devices.

## 8. Reproducibility and artifact validation

- `tools/package.py` sorts files, writes explicit modes, omits directory entries, normalizes CRLF
  for text files, and uses a fixed timestamp.
- CI packages the same staging tree twice and compares the zips with `cmp`; the final run passed.
- CI verifies `sha256sum -c Flick-*.zip.sha256`; the downloaded artifact also passed locally.
- The downloaded manifest was checked file by file: all nine hashes, sizes, and modes match.
- The ARM64 selftest downloaded from the same run was executed on the aarch64 device and printed
  `SELFTEST PASS`.
- The package contains exactly: `module.prop`, five shell scripts, `webroot/index.html`, and the
  two ABI binaries.
- The docs/CI commits after the version bump rebuilt the exact same zip (`eb4320...`) with the same
  `06:41:06` timestamp, confirming that only packaged inputs affect the hash.

## 9. CI runs

| Run | Purpose | Result |
|---|---|---|
| [41944937637](https://github.com/aeeL211/Flick/actions/runs/41944937637) | baseline `main` build | success (selftest was advisory) |
| [43379993436](https://github.com/aeeL211/Flick/actions/runs/43379993436) | pre-version-bump optimization build | success, tests required |
| [43380422292](https://github.com/aeeL211/Flick/actions/runs/43380422292) | package build for `v0.9.11` | success, artifact verified |
| [43381083297](https://github.com/aeeL211/Flick/actions/runs/43381083297) | docs/CI reproducibility run | success, same zip hash |
| [43381304008](https://github.com/aeeL211/Flick/actions/runs/43381304008) | docs follow-up reproducibility run | success, same zip hash |

An intermediate run before the full-history fix used the docs commit timestamp and produced a
different zip hash; it was superseded. The final workflow checks out full history, and runs
`43381083297` and `43381304008` reproduced `eb4320...` from run `43380422292`.

The last run passed host and sanitizer selftests, the proc test, ShellCheck/`sh -n`, the WebUI syntax
check, both cross-builds, ELF verification, deterministic double packaging, and checksum validation.

## 10. Deviations and assumptions

- **`-Os` retained.** The plan allowed `-O2` if it was measurably faster. Two runs showed no
  repeatable gain, so the smaller binary was kept.
- **Version bump.** `v0.9.11` / `versionCode=29` records the safety and performance changes for
  AxManager updates. It changes no runtime behavior.
- **Package growth accepted.** The +1.53% package size buys the post-grab safety check, hardening,
  and the WebUI changes; no feature was removed to offset it.
- **No device FPS claim.** There is no physical-device gameplay benchmark in this environment. The
  measurable runtime evidence is the scan count and the host math benchmark; actual frame time still
  depends on the device, panel, and game.
- **No browser runtime profile.** WebUI improvements are code-level and syntax-checked, not measured
  with a browser profiler.
- **Absolute tool paths** assume the standard Android `/system/bin/{getprop,setprop,cmd,sh}` layout
  used by AxManager in shell mode. `debug.sf.hw` remains a legacy, device-dependent opt-in feature.
- **16 KB validation is ELF-level.** The workflow proves the load alignment and hardening properties;
  no 16 KB-page physical device was available for a runtime test.
- **The touch math is unchanged.** All reference scenarios pass with worst error `0`; the only
  intentional user-visible change is that activation is refused while a finger lands during the grab
  window, which is the documented safety contract.
