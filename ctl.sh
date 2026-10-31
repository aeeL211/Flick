#!/system/bin/sh
# entry point of the webui: sh ./ctl.sh status | save key=value... | start | stop
# the axmanager webui hands a command to sh -c inside double quotes, so quotes, dollar signs and
# backticks in it get mangled. the webui therefore sends only plain words here and every real
# command lives in this file. run it with the plugin folder as the working directory.
# save writes a temp file and renames it, so the daemon never reads half a config. stop sends sigterm,
# waits up to 3 s, then kills, because the kernel drops the grab when the daemon dies. hw.sh 0 runs
# afterwards and does nothing when the property was not changed.
case $0 in */*) D=${0%/*} ;; *) D=. ;; esac
MODDIR=$(cd "$D" && pwd) || exit 1
CFG=$MODDIR/config
PID=$MODDIR/flick.pid
STATE=$MODDIR/flick.state
BIN=$MODDIR/system/bin/flick

# true only for a live flick. the pid file survives a kill or a reboot, so a reused pid must not
# count, the daemon applies the same comm check itself
alive() {
  [ -f "$PID" ] || return 1
  P=$(cat "$PID" 2>/dev/null)
  case $P in ''|*[!0-9]*) return 1 ;; esac
  kill -0 "$P" 2>/dev/null || return 1
  case $(cat "/proc/$P/comm" 2>/dev/null) in flick*) return 0 ;; esac
  return 1
}

case $1 in
  status)
    { cat "$CFG" 2>/dev/null; echo; }
    if alive; then
      echo running=1
      echo "state=$(cat "$STATE" 2>/dev/null)"
    else
      echo running=0
    fi
    ;;
  save)
    shift
    [ $# -gt 0 ] || { echo "ERR nothing to save" >&2; exit 2; }
    for A in "$@"; do
      case $A in
        *[!A-Za-z0-9_.,=]*|=*|'') echo "ERR bad argument" >&2; exit 2 ;;
        *=*) ;;
        *) echo "ERR bad argument" >&2; exit 2 ;;
      esac
    done
    printf '%s\n' "$@" > "$CFG.tmp" && mv -f "$CFG.tmp" "$CFG"
    ;;
  start)
    chmod 755 "$BIN" 2>/dev/null
    "$BIN" --daemon --config "$CFG" --pidfile "$PID" 2>&1
    ;;
  stop)
    if alive; then
      P=$(cat "$PID")
      kill "$P" 2>/dev/null
      I=0
      while [ $I -lt 15 ] && kill -0 "$P" 2>/dev/null; do sleep 0.2; I=$((I + 1)); done
      kill -0 "$P" 2>/dev/null && kill -9 "$P" 2>/dev/null
    fi
    rm -f "$PID"
    [ -f "$MODDIR/hw.sh" ] && sh "$MODDIR/hw.sh" 0
    exit 0
    ;;
  *)
    echo "usage: ctl.sh status|save key=value...|start|stop" >&2
    exit 2
    ;;
esac
