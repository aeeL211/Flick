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

Replies are one line of space-separated `key=value` fields. The order of the existing fields is
stable. A client MUST ignore every `key=value` field it does not recognize. A newer revision may
only append new fields at the end of a line; it may not move, rename, or change the meaning of an
existing field. Such additive fields do not change `proto=1`.

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

The `--listen` path has its own exit codes so a client can distinguish a bad configuration from a
port collision without changing any pre-existing code.

| Condition | Exit code |
|---|---|
| `--listen` without a valid token | `10` |
| `--listen` port missing, malformed, or outside 1..65535 | `10` |
| Socket creation, `bind`, or `listen` fails, including `EADDRINUSE` | `11` |
| Already running | `1` |
| Touchscreen not found | `2` |
| uinput setup failure | `3` |
| Daemonize failure | `5` |
| `/proc` hidden while `filter=1` | `6` |

On these failures the daemon prints one `ERR ...` line on stderr. It never prints the token.

#### 1.2.1 Actual startup order

The source performs startup in the following order. This order is intentionally unchanged; moving a
check would change which exit code wins when several failures are present.

1. Parse arguments. A missing or invalid `--listen` value returns `10` immediately, before the
   pid-file check.
2. Check the pid file. `running()` reads the pid, calls `kill(pid, 0)`, and then requires the first
   five bytes of `/proc/<pid>/comm` to be `flick`. A match returns `1`. See section 8.
3. With `--listen` active, read and validate `FLICK_TOKEN`; failure returns `10`.
4. Create the listener socket and call `bind()` on `127.0.0.1`, then `listen()`. Failure returns
   `11`. This happens before the touchscreen, uinput, and daemonize steps.
5. Split the config path and load the config file. A missing file is tolerated.
6. Open `--dev` or scan `/dev/input/event0..31`. Failure returns `2`.
7. Open `/dev/uinput` and clone the panel. Failure returns `3`.
8. If `filter=1`, verify that `/proc` is visible; failure returns `6`.
9. Wait 300 ms for the panel, then either daemonize (failure returns `5`) or stay in the foreground.
10. Write the pid file, publish the initial state, and enter the event loop.

Because the listener is created at step 4, a client that receives `11` may retry a different port.
A failure at step 6 or later already had a working listener; the daemon closes it before exiting.

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

- A new accepted connection always closes and replaces the previous pending handshake, including one
  that is still in its 300 ms failure linger. There is no "third connection is closed" rule.
- Replacing the pending slot never closes, replaces, or pauses the authenticated client. Only a
  successful `HELLO` can replace the authenticated client.
- A connection that completes `HELLO` replaces and closes the current authenticated client.
- A failed or timed-out handshake never replaces the authenticated client. It stays pending until
  its linger ends or a newer pending connection replaces it.
- Replacing an authenticated client clears its subscription and its hold. The new client starts
  unsubscribed and without a hold. A hold is not transferred between connections.

## 3. Command grammar

Commands are case-sensitive ASCII. Fields are separated by one or more spaces. Values must not
contain spaces or tabs. Keys are at most 31 bytes; values are at most 1000 bytes in addition to
the 1024-byte line limit. Violations are reported as `ERR <key> length` when a key is known, or
`ERR cmd` when the command itself is malformed.

### 3.1 HELLO

```text
HELLO <token>
```

One or more spaces may separate `HELLO` and the token. Success reply:

```text
OK flick=<version> proto=1 caps=hold,err
```

`<version>` is the `version=` value of `module.prop` compiled into the daemon. Development and
test builds use `dev` when no version is supplied. `caps` is a comma-separated list of optional
feature groups understood by this daemon. `hold` advertises `HOLD`/`UNHOLD`; `err` advertises the
`STATUS` error token. Clients must ignore capability names they do not know.

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
ST state=<off|idle|active> paused=<0|1> up=<seconds> pid=<pid> hold=<0|1> err=<none|reason>
```

The first four fields keep their original order and meaning. `hold` and `err` are appended fields.

State mapping:

- `active`: the daemon currently holds the touchscreen grab.
- `off`: the daemon is not grabbing because effective pause is set, or because its last state is an
  error.
- `idle`: the daemon is running normally and is not grabbing (waiting for the target or release).

`paused` is the effective pause: the in-memory PAUSE flag OR an active hold. It is never a config
value. `hold` is `1` while a `HOLD` lease is active. `up` is whole seconds since the event loop
started. `pid` is `getpid()`.

`err` is `none` when the current state is not an error. Otherwise the state file contains
`error:<reason>` and `err` is a bounded token derived from `<reason>`:

- uppercase `A`..`Z` are converted to lowercase;
- only `a`..`z`, `0`..`9`, `_`, and `-` are accepted;
- the reason must contain 1..32 characters;
- an empty, overlong, or otherwise invalid reason is reported as `unknown`.

The token never contains spaces, punctuation, or file contents. Current state reasons such as
`open errno=2` contain a space, so they appear as `err=unknown` unless they are changed to a safe
token in a future version.

### 3.6 PAUSE and RESUME

```text
PAUSE
RESUME
```

Both answer `OK`. PAUSE sets the explicit PAUSE flag, which makes the foreground decision false. The
daemon then uses the existing safe release path: the grab is released only when `gDown == 0` (no
virtual finger is down). If a finger is still down, the release happens on the first later loop
iteration where `gDown` is zero. RESUME clears only that flag and forces a foreground
re-evaluation; it does not touch a hold. If a hold is still active, effective pause remains `1`
after RESUME. PAUSE is not saved to the config and does not survive a restart.

### 3.7 HOLD and UNHOLD

```text
HOLD [<seconds>]
UNHOLD
```

Both answer `OK`. `HOLD` installs a lease for the authenticated connection. `<seconds>` is optional,
an integer from 1 to 120, and defaults to 30. A repeated `HOLD` extends the lease to at least
`now + <seconds>`; it never shortens a lease that is already longer. If the effective pause does not
change, a repeated `HOLD` produces no `EVT`. An invalid argument, extra token, or value outside
1..120 is answered with:

```text
ERR hold range
```

`UNHOLD` clears only the hold; it does not clear the PAUSE flag. While a hold is active, effective
pause is `1` and STATUS reports `hold=1`. The grab uses the same release path as PAUSE: release
happens only when `gDown == 0`, never with a virtual finger down.

The hold is not saved to the config and does not survive a restart. It is automatically released
when:

- the authenticated client disconnects;
- a different client completes `HELLO` and replaces it;
- the lease expires;
- the daemon receives `STOP` or otherwise exits.

Release on disconnect or replacement has no recipient for an `EVT`; the state baseline is still
updated. Release on lease expiry or `STOP` publishes an `EVT` if the client is still subscribed and
the state, effective pause, or hold value changed. On `STOP`, the `OK` reply is queued first, then
the `EVT` if applicable, then the socket closes.

The next hold deadline is included in the loop's `poll()` timeout, so expiry is handled without a
new thread or timer. A hold belongs to one authenticated connection and is never transferred to a
replacement connection.

### 3.8 SUB and UNSUB

```text
SUB
UNSUB
```

Both answer `OK`. A subscribed client receives one line on every change of `state`, effective
`paused`, or `hold`:

```text
EVT state=<off|idle|active> paused=<0|1> hold=<0|1>
```

`EVT` lines may be queued between replies. There is no initial EVT on SUB; use STATUS for the
current value. Changing `sens_h` or another config value alone does not produce an EVT. Extending a
lease without changing `state`, `paused`, or `hold` does not produce an EVT.

### 3.9 STATS

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

### 3.10 STOP

```text
STOP
```

The server queues `OK`, releases an active hold (queuing an `EVT` first when the client is still
subscribed and the release changes the reported state), flushes the output, closes all control
sockets, and requests a clean daemon exit.
The normal exit path releases the grab, restores `hw.sh` state, waits briefly for hooks, destroys
the uinput device, removes the pid file, and removes the state file. If the reply cannot be queued
because the output buffer is full or cannot be flushed because the peer vanished, the daemon still
exits.

## 4. Daemon integration

- The listener and both connection slots are integrated into the existing `poll()` loop. No
  thread, fork, or blocking socket call is introduced.
- The poll timeout is shortened to the nearest control deadline: authentication timeout, 300 ms
  auth-failure linger, the next hold lease expiry, or the next coalesced config write.
- Replies and events use a fixed 4096-byte per-connection output buffer. A client that fills it is
  closed rather than allowed to grow memory or stall the touch path.
- Input uses a fixed 1025-byte line buffer per connection and a fixed read chunk. No allocation is
  performed for client data.
- The control path never builds or executes a command from client input and never accepts a
  client-provided filesystem path. The only file written is the daemon's existing `--config` path.
  A SET can call the existing `hwSync()` path, which runs the fixed `hw.sh` hook just like a normal
  config change.
- The existing touch event path and math functions are not changed. The loop additions are
  readiness/deadline handling, effective pause (the PAUSE flag or a hold lease) with an immediate
  re-evaluation request, and the statistics counters.

## 5. Test plan

`ctrl_test.cpp` includes `flick.cpp` with `main` renamed, loads a synthetic test token, binds port
0 on loopback, learns the assigned port with `getsockname`, and drives the server with the same
`ctlService()` entry point used by the daemon. It covers:

1. Correct HELLO, including the version/protocol prefix and `caps=hold,err`.
2. Invalid token or port exits `10`; an occupied port exits `11`; a failed start scrubs the token.
3. Wrong token: `ERR auth`, 300 ms linger, then close.
4. HELLO timeout: no handshake for 2 seconds, then `ERR auth` and close.
5. Command before HELLO: `ERR auth`.
6. PING/PONG and unknown command.
7. SET valid live values; GET returns the same effective values.
8. SET invalid in a multi-pair command: error reply and no partial memory or file effect.
9. SET persistence merge and the 500 ms coalescing window.
10. SUB receives EVT on PAUSE/RESUME; UNSUB stops events.
11. A line longer than 1024 bytes closes the connection.
12. A second authenticated client replaces and closes the first. A newer pending connection
    replaces the older pending one without disturbing the authenticated client.
13. STATUS reports `hold` and the sanitized `err`; unsafe or overlong reasons become `unknown`.
14. HOLD reports `paused=1` and `hold=1` in EVT and STATUS; UNHOLD clears the lease.
15. A one-second lease expires and releases the hold; a repeated HOLD extends the lease without a
    spurious EVT; the poll deadline follows the lease.
16. Disconnecting the authenticated client clears its hold; a replacement client starts without a
    hold.
17. PAUSE/HOLD interaction: UNHOLD while PAUSE is active leaves `paused=1`; RESUME while HOLD is
    active leaves `paused=1`.
18. STOP replies `OK`, releases the hold, flushes the optional EVT, and sets the clean-exit request.
19. STATUS and STATS reply shapes.

The test is compiled twice in CI, once with `-O2` and once with ASan + UBSan. It is a required
step in the `test` job, and the packaging job still depends on that job. The existing selftest,
sanitizer selftest, proc test, shellcheck, WebUI syntax check, and both ARM cross-builds remain
required.

## 6. Release assets

On a `v*` tag the release job publishes:

- `Flick-v<version>.zip` and its existing `Flick-v<version>.zip.sha256`;
- `flick-arm64` and `flick-arm`, the raw stripped binaries;
- `flick-arm64.sha256` and `flick-arm.sha256`, each in `sha256sum` format.

The zip layout and deterministic packaging are unchanged. The `v0.10.0-rc2` tag uses this same asset
set and the release job marks hyphenated tags such as release candidates as prereleases. The module
version stays `v0.10.0`, so the asset names remain identical to `v0.10.0-rc1`.

## 7. Explicitly rejected alternatives

- A new thread or blocking accept/read loop: violates the single-loop and no-blocking-call rule.
- `0.0.0.0` binding: exposes control beyond the device.
- Client-supplied paths or shell commands: unnecessary attack surface.
- Persisting PAUSE or HOLD: both are session state by design. A hold must disappear with its
  connection so a crashed or frozen application cannot leave Flick paused indefinitely.
- Rewriting the sensitivity math or config-file parser semantics: both are frozen by the existing
  selftest and compatibility contract. SET uses a strict mode of the same parser and the same
  range constants.
- Notifications from the daemon in app mode: the app owns its notification.

## 8. Operational notes

### 8.1 Already-running detection and the binary name

The existing `running()` check reads the pid from `--pidfile`, requires `kill(pid, 0)` to succeed,
and then reads `/proc/<pid>/comm`. It recognizes the process only when the first five bytes of
`comm` are exactly `flick`. It does not read `/proc/<pid>/cmdline`, compare a full path, or compare
the executable inode. This behavior is unchanged by the control-server work.

Consequences:

- The binary file that is executed must have a name beginning with `flick`, such as `flick`,
  `flick.arm64`, `flick-arm64`, or `flickd`. A renamed binary such as `touchd` or `myflick` is not
  recognized, even if it is the same executable. Starting a second copy can then proceed and both
  copies may fight over the touchscreen.
- `/proc/<pid>/comm` is the kernel task name, usually the executable's file name truncated to 15
  bytes. The five-byte prefix check still matches a long name such as `flick-arm64`.
- A stale pid file whose PID was reused by an unrelated process does not block startup. If that PID
  was reused by another process whose `comm` starts with `flick`, startup is refused.
- Keep the installed binary name stable and matching the name used by `ctl.sh`; do not rename it
  only on one side of the pid-file check.
