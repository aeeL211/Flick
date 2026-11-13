# Flick control protocol v1

Status: implementation specification. This document is written before the code and is the final
reference for the optional control server. Changes to the wire format require a new protocol
version.

## 1. Scope and compatibility

The control server is opt-in. Without `--listen` the daemon opens no socket, never reads
`FLICK_TOKEN`, and keeps the current argument handling, output, timing, grab behavior, and state
file format. `ctl.sh`, the WebUI, `selftest.cpp`, and `proc_test.cpp` keep their current contracts.

New command-line options:

| Option | Meaning |
|---|---|
| `--listen PORT` | Bind an IPv4 stream socket to `127.0.0.1:PORT` only. Non-blocking. `SO_REUSEADDR` is set. |
| `--config PATH` | Already exists in the daemon. It remains the single source of the config path; no second option is added. |

`--listen` may be combined with `--daemon`, `--pidfile`, and `--dev`. The listener is created
before the daemonize fork, so all startup errors are reported on stderr with an exit code.

### 1.1 Authentication token

- Read from `FLICK_TOKEN`.
- Minimum 32 bytes, maximum 256 bytes.
- Only printable ASCII `!` through `~` (0x21 to 0x7e) is accepted, so the token cannot contain a
  space, control character, or line break. A token with any other byte is invalid.
- The value is copied to a fixed buffer, the original environment value is wiped, and
  `unsetenv("FLICK_TOKEN")` is called immediately. The copy is wiped on shutdown.
- The token is never printed, written to the config, or included in an error message.
- Comparison is constant-time over both content and length.

### 1.2 Startup failures

| Condition | Exit code |
|---|---|
| `--listen` without a valid token | `2` |
| `--listen` port malformed or outside 1..65535 | `2` |
| Bind or listen fails, including `EADDRINUSE` | `3` |
| Existing exits (already running, touchscreen, uinput, daemonize, hidden `/proc`) | unchanged |

On these failures the daemon prints one `ERR ...` line on stderr. It never prints the token.

### 1.3 Notifications

When `--listen` is active, `notify.sh` is not called. The Android application is responsible for
its own notifications. `hw.sh` remains active and is called by the existing `hwSync` path.

## 2. Transport and framing

- TCP over IPv4 loopback only (`127.0.0.1`). The listener and every accepted socket are
  non-blocking and close-on-exec.
- One message is one line of UTF-8 text terminated by `LF` (`0x0a`). Replies use `LF` too.
- A line may contain at most 1024 bytes before the `LF`. The 1025th byte without a line break
  closes the connection. A trailing `CR` before `LF` is tolerated and removed.
- A NUL byte in a line closes the connection.
- Empty lines and unknown commands are answered with `ERR cmd`.
- A client must send `HELLO` before any other command.
- Authentication must complete within 2 seconds of `accept()`.
- After an authentication failure, the server writes `ERR auth`, stops reading, and closes the
  socket 300 ms later. The same path is used for a timeout.
- All socket errors other than `EAGAIN`/`EWOULDBLOCK`/`EINTR` close that connection. `SIGPIPE` is
  ignored process-wide; a failed write is handled as `EPIPE` and closes the connection.

### 2.1 Connection slots

The server keeps at most one authenticated client and one client that is still authenticating.

- A connection that completes `HELLO` replaces and closes the current authenticated client.
- A failed or timed-out handshake never replaces the authenticated client.
- If both a pending handshake and an authenticated client already exist, a further connection is
  accepted and closed immediately. A failed handshake that is still in its 300 ms linger may be
  replaced by a new pending connection.
- Replacing a client clears its subscription. The new client starts unsubscribed.

## 3. Command grammar

Commands are case-sensitive ASCII. Fields are separated by one or more spaces. Values must not
contain spaces or tabs. Keys are at most 31 bytes; values are at most 1000 bytes in addition to
the 1024-byte line limit. Violations are reported as `ERR <key> length` when a key is known, or
`ERR cmd` when the command itself is malformed.

### 3.1 HELLO

```text
HELLO <token>
```

Success reply:

```text
OK flick=<version> proto=1
```

`<version>` is the `version=` value of `module.prop` compiled into the daemon. Development and
test builds use `dev` when no version is supplied.

Failure reply, then the 300 ms linger and close:

```text
ERR auth
```

Any command other than `HELLO` before successful authentication gets the same reply and close.

### 3.2 PING

```text
PING
```

```text
PONG
```

### 3.3 GET

```text
GET
```

```text
CFG sens_h=<float> sens_v=<float> zone=<int> orient=<int> smooth=<int> strength=<int> resp=<int> curve=<int> hw=<int> filter=<int> pkgs=<list>
```

The values are the effective in-memory values, not the contents of the file. Floats use the
shortest decimal form that round-trips to the stored float and always contain a decimal point
(for example `1.0`, `1.25`, `2.5`). The key order is fixed as shown.

### 3.4 SET

```text
SET k=v [k=v ...]
```

All pairs are validated before anything is applied. If every pair is valid, the complete command
is applied live, queued for atomic persistence, and answered with:

```text
OK
```

If any pair is invalid, nothing is applied, written, or announced, and the reply is:

```text
ERR <key> <reason>
```

Reasons:

| Reason | Meaning |
|---|---|
| `unknown` | The key is not one of the eleven config keys. |
| `range` | A syntactically valid number is outside the allowed range. |
| `invalid` | The value cannot be parsed as the key's type. |
| `length` | The value or key exceeds the documented limit. |

A duplicate key in one SET is allowed; the last occurrence wins. A malformed pair without `=`,
or an empty key, is answered with `ERR cmd` because there is no trustworthy key to echo.

Validation reuses the existing parser rules and bounds:

| Key | Strict SET rule | Current config-file behavior (unchanged) |
|---|---|---|
| `sens_h`, `sens_v` | finite float 1.0..5.0 | clamp to 1.0..5.0 |
| `zone` | integer 0..2 | clamp to 0..2 |
| `orient` | integer 1 or 3 | anything other than 3 becomes 1 |
| `smooth`, `hw`, `filter` | integer 0 or 1 | any nonzero integer becomes 1 |
| `strength`, `resp` | integer 1..100 | clamp to 1..100 |
| `curve` | integer 0..2 | clamp to 0..2 |
| `pkgs` | 1..16 comma-separated names, each 1..127 bytes of `[A-Za-z0-9_.]`; at least one name; fits the value limit | invalid tokens are skipped by the existing package parser and an empty list falls back to the two Free Fire packages |

`SET` applies `deriveFilter()` for `strength`/`resp`, refreshes the package tracker for `pkgs`,
calls `hwSync()` for `hw`, and forces the main loop to re-evaluate its foreground decision. A
finger already on the panel keeps the sensitivity, curve, zone, and smoothing captured at
touch-down; the existing live `strength`/`resp` behavior is unchanged.

#### Persistence

- The config file is the existing `--config` path.
- The write is merged: lines for keys not changed by SET are preserved, including unknown lines.
  For each changed key, all old occurrences are removed and exactly one effective `key=value`
  line is written. Missing changed keys are appended.
- The temporary file is written in the same directory, then `rename()`d over the config. The
  temporary name is unique to the daemon so it cannot collide with `ctl.sh`'s `config.tmp`.
- At most one config write is started per 500 ms. Multiple SETs inside that window are coalesced
  into the next write. A pending write is flushed on exit, including `STOP`.
- A write failure does not undo the live value. The effective value remains in memory and the
  failure is not allowed to corrupt the previous file. The last successful file state wins after
  a restart. The app should treat a missing file update as non-fatal.
- App-mode SET and WebUI `ctl.sh save` must not be used as simultaneous writers. If they race, the
  last rename wins. This is recorded in `docs/DECISIONS.md`.

### 3.5 STATUS

```text
STATUS
```

```text
ST state=<off|idle|active> paused=<0|1> up=<seconds> pid=<pid>
```

State mapping:

- `active`: the daemon currently holds the touchscreen grab.
- `off`: the daemon is not grabbing because it is paused, or because its last state is an error.
- `idle`: the daemon is running normally and is not grabbing (waiting for the target or release).

`paused` is the in-memory PAUSE flag, never a config value. `up` is whole seconds since the event
loop started. `pid` is `getpid()`.

### 3.6 PAUSE and RESUME

```text
PAUSE
RESUME
```

Both answer `OK`. PAUSE makes the foreground decision unconditionally false. The daemon then uses
the existing safe release path: the grab is released only when `gDown == 0` (no virtual finger is
down). If a finger is still down, the release happens on the first later loop iteration where
`gDown` is zero. RESUME clears the flag and forces a foreground re-evaluation. PAUSE is not saved
to the config and does not survive a restart.

### 3.7 SUB and UNSUB

```text
SUB
UNSUB
```

Both answer `OK`. A subscribed client receives one line on every change of `state` or `paused`:

```text
EVT state=<off|idle|active> paused=<0|1>
```

`EVT` lines may be queued between replies. There is no initial EVT on SUB; use STATUS for the
current value. Changing `sens_h` or another config value alone does not produce an EVT.

### 3.8 STATS

```text
STATS
```

```text
STATS grabs=<n> releases=<n> syn_dropped=<n> scans=<n>
```

Counters are monotonic for the lifetime of the process:

- `grabs`: successful `EVIOCGRAB` calls that acquired the panel.
- `releases`: successful `EVIOCGRAB` calls that released the panel, including the post-grab
  safety release and shutdown.
- `syn_dropped`: `SYN_DROPPED` records observed in the panel stream.
- `scans`: `/proc` foreground scan attempts.

They are diagnostics, not exact event counts.

### 3.9 STOP

```text
STOP
```

The server queues `OK`, flushes it, closes all control sockets, and requests a clean daemon exit.
The normal exit path releases the grab, restores `hw.sh` state, waits briefly for hooks, destroys
the uinput device, removes the pid file, and removes the state file. If the reply cannot be
flushed because the peer vanished, the daemon still exits.

## 4. Daemon integration

- The listener and both connection slots are integrated into the existing `poll()` loop. No
  thread, fork, or blocking socket call is introduced.
- The poll timeout is shortened to the nearest control deadline: authentication timeout, 300 ms
  auth-failure linger, or the next coalesced config write.
- Replies and events use a fixed 4096-byte per-connection output buffer. A client that fills it is
  closed rather than allowed to grow memory or stall the touch path.
- Input uses a fixed 1025-byte line buffer per connection and a fixed read chunk. No allocation is
  performed for client data.
- The control path never builds or executes a command from client input and never accepts a
  client-provided filesystem path. The only file written is the daemon's existing `--config` path.
  A SET can call the existing `hwSync()` path, which runs the fixed `hw.sh` hook just like a normal
  config change.
- The existing touch event path and math functions are not changed. The loop additions are
  readiness/deadline handling, the PAUSE flag and immediate re-evaluation request, and the three
  statistics counters.

## 5. Test plan

`ctrl_test.cpp` includes `flick.cpp` with `main` renamed, loads a synthetic test token, binds port
0 on loopback, learns the assigned port with `getsockname`, and drives the server with the same
`ctlService()` entry point used by the daemon. It covers:

1. Correct HELLO, including the version/protocol prefix.
2. Wrong token: `ERR auth`, 300 ms linger, then close.
3. HELLO timeout: no handshake for 2 seconds, then `ERR auth` and close.
4. Command before HELLO: `ERR auth`.
5. PING/PONG and unknown command.
6. SET valid live values; GET returns the same effective values.
7. SET invalid in a multi-pair command: error reply and no partial memory or file effect.
8. SET persistence merge and the 500 ms coalescing window.
9. SUB receives EVT on PAUSE/RESUME; UNSUB stops events.
10. A line longer than 1024 bytes closes the connection.
11. A second authenticated client replaces and closes the first.
12. STOP replies `OK`, flushes, and sets the clean-exit request.
13. STATUS and STATS reply shapes.

The test is compiled twice in CI, once with `-O2` and once with ASan + UBSan. It is a required
step in the `test` job, and the packaging job still depends on that job. The existing selftest,
sanitizer selftest, proc test, shellcheck, WebUI syntax check, and both ARM cross-builds remain
required.

## 6. Release assets

On a `v*` tag the release job publishes:

- `Flick-v<version>.zip` and its existing `Flick-v<version>.zip.sha256`;
- `flick-arm64` and `flick-arm`, the raw stripped binaries;
- `flick-arm64.sha256` and `flick-arm.sha256`, each in `sha256sum` format.

The zip layout and deterministic packaging are unchanged. No tag is created by this work.

## 7. Explicitly rejected alternatives

- A new thread or blocking accept/read loop: violates the single-loop and no-blocking-call rule.
- `0.0.0.0` binding: exposes control beyond the device.
- Client-supplied paths or shell commands: unnecessary attack surface.
- Persisting PAUSE: the flag is session state by design.
- Rewriting the sensitivity math or config-file parser semantics: both are frozen by the existing
  selftest and compatibility contract. SET uses a strict mode of the same parser and the same
  range constants.
- Notifications from the daemon in app mode: the app owns its notification.
