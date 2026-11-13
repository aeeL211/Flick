# Decisions

This file records assumptions and choices that were not fully specified. New entries are added
when a decision can affect behavior, compatibility, CI, or security. No new third-party
dependencies are introduced by the control-server work; the implementation only uses the C
library, POSIX sockets, and existing build tools.

## D-001: The control server is an isolated opt-in path

Chosen: `--listen PORT` adds a loopback-only, non-blocking server to the existing `poll()` loop.
Without the flag no socket is created and no token is read. The touch math, config-file parsing,
grab logic, state file, scripts, and command output remain unchanged.

Rejected: a separate daemon, a thread, or an always-on port. Those would change the default
attack surface and the no-argument behavior.

## D-002: Token handling is stricter than the minimum requirement

Chosen: 32..256 bytes, printable ASCII without spaces, copied to a fixed buffer, original
environment storage wiped, `unsetenv` called immediately, and the copy wiped on shutdown.
Comparison is constant-time in content and length.

Reason: the token travels inside a space-separated line, and the daemon forks hooks that would
otherwise inherit the environment. Rejecting spaces keeps the grammar unambiguous. The token is
never logged.

## D-003: SET is strict, file loading stays lenient

The current `parseLine` is deliberately lenient: malformed values are ignored and numeric values
are clamped. That behavior is frozen because existing config files and the WebUI depend on it.

Chosen: SET uses the same dispatch and range constants in a strict mode. A value outside the range
or not fully parseable is rejected, and the whole SET is atomic. The config file continues to use
the lenient path. `pkgs` reuses the existing package character/count rules but rejects an empty or
malformed list instead of silently falling back.

## D-004: STATUS state mapping

The existing state file only has `idle`, `active`, and `error:<reason>`. The protocol needs
`off`, `idle`, and `active`.

Chosen: `active` means the grab is currently held; `off` means no grab and either PAUSE is set or
the current state is an error; `idle` means no grab and no error/pause. If PAUSE is requested while
a finger is down, STATUS can briefly report `active` with `paused=1` until the safe release path
runs. This is intentional and observable.

## D-005: One authenticated client plus one pending handshake

Chosen: a new connection is accepted as pending while the current authenticated client keeps
working. Only a successful HELLO closes and replaces the old client. A failed handshake leaves the
old client untouched. A third connection is closed immediately.

Reason: this satisfies "a new client that authenticates replaces the old" without letting an
unauthenticated connection evict a working app.

## D-006: Config persistence is merged and coalesced

Chosen: SET merges changed keys into the existing file, preserves unknown lines, removes duplicate
old occurrences of changed keys, writes a unique temporary file in the same directory, then
renames. At most one write starts per 500 ms; a pending write is flushed on exit.

A write failure does not roll back the live value. App-mode SET and WebUI `ctl.sh save` must not be
used as simultaneous writers; if they race, the last rename wins. This avoids a blocking rollback
protocol that would complicate the single-loop design.

## D-007: No notify.sh in app mode

Chosen: when `--listen` is active, `notify.sh` is skipped for every state change. `hw.sh` remains
unchanged. The Android application is expected to post its own notification. This is explicit in
the requirement and is not configurable.

## D-008: Raw release binary names

Chosen: release assets use `flick-arm64` and `flick-arm` (not `flick.arm64`/`flick.arm`) with
individual `.sha256` files. The deterministic module zip and its checksum keep their existing
names and layout. No tag is created by this work.

## D-009: Protocol version and daemon version

Chosen: the wire protocol is `proto=1`. The daemon reports the `module.prop` version string compiled
into the binary through `FLICK_VERSION`; host tests that do not pass the macro use `dev`. A feature
release bumps `module.prop` to `v0.10.0` / `versionCode=30`. The version is metadata only and does
not alter the no-argument runtime path.

## D-010: Branch and review base

Chosen: work happens on `feat/app-control-server`, created from the current optimization branch
because that branch contains the baseline workflow and hardening changes this work builds on. The
pull request targets the existing optimization branch (stacked review), not `main`; `main` is not
modified and no force-push is used.
