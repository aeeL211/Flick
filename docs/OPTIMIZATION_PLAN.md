# Flick optimization and AxManager/AOSP alignment plan

Status: historical phase-1 document; research complete. Implementation and measured results are in
[`VERIFICATION.md`](VERIFICATION.md).
Target: `aeeL211/Flick` at `8d9b390` (branch `perf/axmanager-aosp-optimization`).
Reference implementation: `fahrez182/AxManager` source and plugin documentation.
Evidence baseline: GitHub Actions run `41944937637` (successful `main` build).

This document records the findings from phase 1 before any source change. The work below keeps
the existing user-visible behavior and the reverse-engineered touch math. Changes that affect
behavior at all (grab safety and version metadata) are called out explicitly.

## 1. Scope and constraints

In scope:

- Make the existing build/test workflow trustworthy and reproducible.
- Reduce work in the daemon while a target app is already in the foreground.
- Close the remaining grab race without changing the sensitivity output.
- Reduce WebUI shell calls, DOM work, and background polling.
- Align scripts with AxManager/AOSP path and shell conventions.
- Produce measurable before/after evidence and an installable `Flick-v*.zip`.

Out of scope:

- Rewriting the daemon or the WebUI framework.
- Changing the sensitivity formula, filter coefficients, defaults, config keys, or package list.
- Adding runtime dependencies or download-at-build dependencies.
- Root-only behavior. Flick remains a shell-plugin (uid 2000).
- Removing `debug.sf.hw`. It is a legacy, undocumented property, but it is opt-in and existing
  behavior; it stays and is documented as device-dependent.
- Releasing or tagging. The PR contains the branch and artifact; `main` is untouched.

Hard constraints:

- `selftest.cpp` reference values are authoritative. The filter and curve values must keep passing
  bit-for-bit (or within the documented 2-ulp / 1-raw-unit bounds through `pump`).
- No `-ffast-math`, no reassociation, keep `-ffp-contract=off`.
- All C/C++ compilation runs in GitHub Actions. No local compiler is available here.
- No force-push, no commits to `main`.
- Small, atomic commits.

## 2. Baseline

| Item | Baseline |
|---|---|
| Commit | `8d9b390` (`main`) |
| Successful workflow | `41944937637` (manual dispatch) |
| Package | `Flick-v0.9.10.zip`, 42,677 bytes |
| SHA-256 of package | `24d82a15f1420b148ea6f8d57fd2b82efa8f7fce60ea6ece1569f0a01c1193cc` |
| `flick.arm64` (stripped) | 19,360 bytes |
| `flick.arm` (stripped) | 19,464 bytes |
| Compiler mode | `-Os`, no hardening flags, no page-size override |
| Selftest | host and ARM cross-build succeeded, but job is advisory |
| Host selftest result | `SELFTEST PASS` (executed from the downloaded ARM64 CI artifact) |

The workflow is green, but that green does not currently prove the selftest because of
`continue-on-error: true` and because `build` does not depend on `selftest`.

## 3. Findings

Severity is about release risk, not code volume. “Measured” means observed from the repository or
the baseline artifact; “static” means established by reading the implementation.

### P0 - correctness and release confidence

1. **The selftest cannot fail the build.**
   `.github/workflows/build.yml` marks the `selftest` job `continue-on-error: true` and the `build`
   job has no `needs: selftest`. A math regression could ship in a green run.
2. **Feature branches and pull requests are not tested.**
   `on.push.branches` is only `[main]`, plus tags and manual dispatch. The optimization branch would
   otherwise be packaged without CI.
3. **No deterministic package or checksum.**
   `zip -r` stores current timestamps, unsorted tree order, and ambient permissions. Two builds of
   the same commit can differ; the artifact cannot be verified independently.
4. **Grab race remains between the idle check and `EVIOCGRAB`.**
   `activate()` checks `touching(fd)`, then grabs. A finger can land in that window. The safety
   contract says “never grab while a finger is down”; the current check narrows the window but does
   not close it.
5. **No compiler hardening or 16 KB page alignment.**
   The build has no stack protector, `_FORTIFY_SOURCE`, RELRO/NOW, or `-z max-page-size=16384`.
   The latter matters for Android 15+ devices with 16 KB pages and costs almost nothing for older
   devices. Warnings are not fatal, so a warning can accumulate unnoticed.

### P1 - runtime and maintenance

6. **`/proc` is fully rescanned every 3 s even while the game is already foreground.**
   `inGame()` always calls `scanProc()`, which opens `/proc`, enumerates pids, compares against the
   previous snapshot, and may open `cmdline`/`oom_score_adj` for new pids. While a tracked
   `oom_score_adj` fd is already ≤ 0, the answer is already known. `filter=0` also skips foreground
   detection entirely, so this cost only applies with the default `filter=1`.
7. **The WebUI polls blind every 3 s, including while hidden.**
   `setInterval(..., 3000)` calls `refresh(false)`, which still runs `sh ./ctl.sh status` through
   `ksu.exec` (a blocking bridge call) and then `render()`.
8. **Every poll performs a full DOM render.**
   `render()` rewrites all value labels, sliders, switches, option groups, status text, and calls
   `drawArt()`/`drawPk()`. When the config has not changed, only status fields need updating.
   `drawPk()` already has a key guard; the rest does not.
9. **Status calls can overlap.**
   `refresh()` has no in-flight guard. If `ctl.sh status` takes longer than 3 s, another call starts
   and responses can arrive out of order. The sequence/floor logic protects status flips, but not
   bridge load or stale config reads.
10. **Dragging a slider renders the whole page per pointer event.**
    `put()` calls `render()` for every `pointermove`; `render()` updates every slider, switch, option
    group, art, and package list. Only the dragged slider and its value (and the linked axis when
    sync is on) change.
11. **Search rebuilds the application list on every keystroke.**
    `input` calls `drawPick()` immediately, which filters/sorts and recreates rows and icons.
12. **Scripts hardcode the shell-mode AxManager folder and use relative system tools.**
    `customize.sh` falls back to `/data/user_de/0/com.android.shell/axeron`, although AxManager
    documents `$AXERONDIR` and passes `MODPATH` inside the active plugins folder. This fallback can
    stop the wrong instance (or none) after a mode switch. `getprop`, `setprop`, and `cmd` are
    resolved through `PATH`; system-tool precedence can vary with “Prefer System Binaries”.
13. **No shell or WebUI syntax gate.**
    The workflow runs `sh -n` but not `shellcheck`; nothing checks the inline JavaScript. The WebUI
    is the only control surface, so a syntax error bricks the plugin UI until reinstall.
14. **No static-analysis or sanitizer run.**
    The host selftest uses plain `-O2`; a leak/UB detector and `-Werror` build would catch issues that
    warnings currently hide.

### P2 - documentation and minor cleanup

15. `setpriority(-8)` is best-effort but looks mandatory. On shell uid it may be denied; behavior
    should not depend on it. Add a comment and keep the call.
16. README build instructions still describe `-Os` only and do not mention artifact verification,
    16 KB alignment, the selftest gate, or the new behavior notes.
17. No documented assumptions for `FLICK_PROC_ROOT`-style testability. This plan adds a
    compile-time-only hook, not a runtime knob.

## 4. Proposed changes

### 4.1 CI and packaging (P0)

Rewrite `.github/workflows/build.yml`:

- Triggers: every branch push, every pull request, `v*` tags, and `workflow_dispatch`.
- `concurrency` per ref to cancel superseded branch runs; keep tag/release runs from cancelling
  each other.
- Minimal permissions: `contents: read` by default; `contents: write` only on the tag release job.
- `test` job (required):
  - host selftest with `-O2 -ffp-contract=off -Werror`;
  - host selftest under ASan+UBSan with `halt_on_error=1`;
  - cross-compile the selftest for both ABIs;
  - fake-`/proc` foreground unit test (see 4.2) with `-DFLICK_PROC_ROOT` and `-DFLICK_PROC_TEST`;
  - `shellcheck` on all scripts (warning level, sh dialect);
  - `node --check` on the JavaScript extracted from `index.html`;
  - `sh -n` on every script.
- `build` job (`needs: test`):
  - locate the NDK robustly (`ANDROID_NDK_HOME`, then `ANDROID_NDK_LATEST_HOME`, then the newest
    `$ANDROID_HOME/ndk/*`), print `source.properties`, and fail early if missing;
  - build unstripped `-O2` binaries for `aarch64-linux-android24` and
    `armv7a-linux-androideabi24` with `-Werror` and the hardening flags below;
  - record `llvm-size` before stripping, then strip with `llvm-strip`;
  - verify ELF class/machine, no unexpected `NEEDED`, no `TEXTREL`, `GNU_RELRO`, `BIND_NOW`,
    non-executable stack, and `LOAD` alignment ≥ 16 KB;
  - validate `module.prop` (id, versionCode, numeric `axeronPlugin`, version format);
  - stage files, normalize line endings, set explicit modes.
- `package` job (`needs: build`):
  - call `tools/package.py`, which mirrors AxManager's deterministic packer: sorted file list,
    fixed/`SOURCE_DATE_EPOCH` timestamps, LF for text files, explicit modes, no directory entries;
  - write `SHA256SUMS`, `build-info.txt` (commit, NDK version, flags, sizes), and a package manifest;
  - upload `Flick-*.zip`, checksum, and metrics as artifacts.
- `release` job (`needs: package`, tags only): attach zip, checksum, and info to the GitHub release.
- Remove `continue-on-error` everywhere; a failed test stops packaging.

Hardening flags (after confirming the math still passes):

```text
-O2 -DNDEBUG -D_FORTIFY_SOURCE=2 -fstack-protector-strong
-Wall -Wextra -Werror -Wformat=2 -Wshadow -Wundef -Wcast-align -Wpointer-arith
-ffp-contract=off -fno-exceptions -fno-rtti -fvisibility=hidden
-fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
-ffile-prefix-map=$PWD=.
-Wl,--gc-sections -Wl,--as-needed -Wl,--build-id=none
-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,max-page-size=16384
-nostdlib++ -lm
```

If `-O2` changes any selftest reference value, stop and fall back to `-Os` for the binary; the
selftest gate decides, not the size result. `-O2` is expected to help the per-event math; the
`/proc` change is the larger win.

New helper files:

- `tools/package.py` - deterministic packaging, manifest, checksum.
- `tools/check_webui.py` - extract the inline `<script>` and run `node --check`; fail on extraction
  ambiguity (exactly one script block expected).
- `proc_test.cpp` - unit test for foreground scan skipping and re-scan after background (4.2).

### 4.2 Daemon (P0/P1)

All changes are local to `flick.cpp`; the math functions and their inputs are not touched.

1. **Skip the full scan while already foreground.**
   Split the current `inGame()` into:
   - `foregroundFromTracked()`: pread every tracked `oom_score_adj`, close dead fds, return true on
     the first value ≤ 0;
   - `inGame()`: if tracked fds already report foreground, return true immediately; otherwise
     `scanProc()` once and re-check.

   Semantics: once a target is foreground, no other app can be the top app; the state is true until
   that process changes adj or dies. When it does, the same call rescans and can find another
   target. `filter=0` is unchanged and still bypasses detection.

2. **Close the grab race.**
   After `EVIOCGRAB, 1`, call `touching(fd)` again:
   - `0`: proceed, drain the queue, activate.
   - `1`: release the grab, close, return 1 (retry after `retryMs`), so a finger that landed in the
     window is never held.
   - `-1`: release the grab, set the existing `error:touch` state, return -1.

   There is still one brief grab/release in the race window, but no event is withheld after the
   re-check. This is the safest option short of an atomic kernel API that does not exist.

3. **Testability without runtime behavior change.**
   Add a compile-time `FLICK_PROC_ROOT` string literal (default `"/proc"`) used by `readPids()`,
   `checkPid()`, and the `oom_score_adj` path. Add `gProcScans` only under `FLICK_PROC_TEST`.
   `proc_test.cpp` creates a fake tree and asserts:
   - first `inGame()` scans once and returns true for a package with adj 0;
   - repeated `inGame()` while foreground does not scan;
   - changing adj to 500 triggers exactly one rescan and returns false;
   - a new matching pid triggers one more scan and returns true.

   Production builds use the default and pay no runtime cost.

4. **Documentation of `setpriority`.**
   Keep `setpriority(PRIO_PROCESS, 0, -8)` as best-effort; comment that denial is expected on some
   shell configurations and does not affect correctness.

No changes to: `Config`, `parseLine`, `loadConfig`, `deriveFilter`, `gainOf`, `euroStep`,
`curveFactor`, `blendGain`, `closeSlot`, `closeFrame`, event ordering, batching, or output values.

### 4.3 WebUI (P1)

Keep the same HTML, CSS, copy, and API usage. Changes are limited to scheduling and targeted DOM
updates.

1. **Visibility-aware polling.**
   Replace the unconditional `setInterval` with start/stop functions driven by
   `document.visibilityState` and `visibilitychange`. Hidden: no shell calls, no timers. Visible:
   refresh once immediately, then resume the 3 s cycle.
2. **In-flight guard.**
   A module-level `refreshing` flag skips a poll while a previous `ctl.sh status` is pending. User
   actions still use `guarded()` and their own sequence floor.
3. **Status-only render.**
   Parse `running`/`state` on every status response. Parse config lines into a compact key; if the
   key is unchanged, call a new `renderStatus()` that updates only badge, dot, status container,
   detail text, and toggle label/classes. If the config key changed (or this is the initial/action
   refresh), apply config and call the full `render()`.
4. **Slider-local repaint.**
   `put()` updates only the dragged slider's `--r`, its thumb ARIA value, and its `[data-show]`
   label; when X/Y sync is on, update the paired slider too. No `drawArt`, package list, or other
   controls are touched during the drag. `saveCfg()` remains debounced/on release.
5. **Debounced search.**
   The package picker input waits ~120 ms before `drawPick()`. Enter still blurs; opening/closing
   still renders immediately.
6. **Conservative fallback path.**
   `ksu.moduleInfo().moduleDir` remains the primary source. The documented shell-mode path stays as
   a last-resort fallback because the WebUI has no other way to read `$AXERONDIR`; the limitation is
   recorded here and in the README rather than silently guessing another path.

No changes to: command strings passed to `ctl.sh`, config key names, save format, package
validation, dialogs, icons, or status semantics.

### 4.4 Scripts (P1)

- `customize.sh`: derive the plugins root from `MODPATH` (`PL=${MODPATH%/*}`) instead of a hardcoded
  shell-mode path; keep a fallback only if `MODPATH` is not an absolute path. Use
  `/system/bin/getprop`; keep `abort` (installer-provided). Continue killing both `flick` and the
  legacy `sensit` pid files.
- `hw.sh`: use `/system/bin/getprop` and `/system/bin/setprop`; preserve the “remember and restore”
  behavior; document that the property is legacy/debug and device-dependent.
- `notify.sh`: use `/system/bin/cmd`; keep the existing title/text and fixed tag.
- `ctl.sh`: keep the plain-word protocol and atomic rename; add `shellcheck`-clean quoting and
  integer arithmetic; use absolute `/system/bin/getprop`-class tools only where PATH precedence
  matters. The status output format (`config`, `running=`, `state=`) is frozen because the WebUI
  parses it.
- `uninstall.sh`: unchanged behavior; verify it still stops via `ctl.sh` and restores `hw.sh`.

## 5. Verification and metrics

| Goal | Metric | Method |
|---|---|---|
| Math unchanged | selftest PASS, worst error within documented bounds | host `-O2`, sanitizer, both NDK ABIs |
| CI trustworthy | test job required by `build`; no `continue-on-error` | workflow inspection + a deliberately failing test run on a scratch commit/branch |
| Foreground cost | `/proc` scans per 60 s while target stays foreground: 20 before, 1 after | `proc_test.cpp` with fake root and scan counter |
| Rescan correctness | exactly one scan after adj changes/background, one after a new pid | same test |
| Binary size | stripped bytes and `llvm-size` text/data | baseline vs new, both ABIs |
| Package reproducibility | same commit produces byte-identical zip; SHA-256 matches | build twice, `sha256sum -c` |
| Package validity | installable zip with exactly the expected files/modes; ELF checks pass | manifest + `llvm-readelf` |
| WebUI syntax | `node --check` passes | `tools/check_webui.py` |
| Shell quality | `shellcheck`/`sh -n` pass | CI |
| WebUI load | 0 status calls while hidden; at most one in flight; 0 full renders when config unchanged | code-level counters + manual WebUI check if possible; documented as static where not runtime-measured |
| Drag cost | one slider repaint per pointer event instead of full render | code inspection + manual check |

Baseline numbers come from artifact `41944937637`. New numbers and run IDs go into
`docs/VERIFICATION.md` after the workflow is green.

## 6. Risks and mitigations

| Risk | Mitigation |
|---|---|
| `-O2` changes float results | `-ffp-contract=off`, no fast-math, selftest gate. Fall back to `-Os` if any exact value changes. |
| Skip-scan misses a new foreground app | Only skipped while a tracked target is confirmed ≤ 0; changed adj/dead fd forces a scan immediately. Test covers the transition. |
| Post-grab recheck drops a few events | It releases immediately when a finger is down; the previous behavior could hold the touchscreen. Safety wins. Retry is 250 ms and idempotent. |
| WebUI status-only render misses a field | `renderStatus()` is the single path for status fields; full render still runs on config change and actions. Syntax gate catches parse errors. |
| Visibility logic stops polling after resume | `visibilitychange` starts with an immediate refresh; the interval guard is explicit. |
| 16 KB alignment grows the binary | Measure; expected small. Revert only if a compatibility problem is proven, not for size alone. |
| Absolute tool paths differ by device | `/system/bin/{getprop,setprop,cmd}` exist on every supported Android; all other commands stay on the AxManager/PATH contract. |
| Deterministic zip breaks installer assumptions | Mirror AxManager's own `plugins/build.py` conventions; no directory entries, explicit modes, LF text. Validate by unzipping and `sh -n`. |

## 7. Commit sequence

1. `docs: record optimization and AxManager alignment plan` (this file).
2. `ci: gate packaging on tests and add static checks` (workflow + `tools/`).
3. `perf: skip foreground proc scans and close grab race` (`flick.cpp` + `proc_test.cpp`).
4. `perf(webui): visibility-aware polling and targeted renders` (`index.html` + checker).
5. `chore: align scripts with AxManager paths` (`ctl.sh`, `customize.sh`, `hw.sh`, `notify.sh`).
6. `build: add hardening and 16 KB alignment` (workflow flags + README build docs).
7. `docs: add verification results and update README` (actual metrics, run links, artifact hash).
8. Version bump (`module.prop`) only after the build is green and measured.

Each commit must stay buildable. The workflow is updated before the feature branch push so the first
push is tested.

## 8. Assumptions

- GitHub-hosted `ubuntu-latest` provides an Android NDK and `shellcheck` (or can install
  `shellcheck` from the preinstalled apt mirror). The NDK version is printed and recorded.
- The existing `selftest.cpp` reference values remain the source of truth; no expected value is
  edited to make `-O2` pass.
- Default `filter=1` is the common path; `filter=0` intentionally does no foreground detection.
- The baseline artifact/run is representative for before/after size and behavior comparisons.
- No release tag is created by this work. The PR carries the branch, green run, artifact, and
  checksum. `main` is not modified.
- If an ambiguous choice appears during implementation, the safest behavior-preserving option is
  chosen and recorded in `docs/VERIFICATION.md`.
