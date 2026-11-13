#!/system/bin/sh
# called by the daemon on every change between idle and active. posts one system notification with a
# fixed tag, so a new one replaces the old and they never pile up. unlike a toast it does not depend on
# the webui being open. the text follows flick.state, not an argument, so two quick changes that race
# still end with the notification that matches the real state
MODDIR=${0%/*}
S=$(cat "$MODDIR/flick.state" 2>/dev/null)
H=1.0
V=1.0
if [ -r "$MODDIR/config" ]; then
  while IFS='=' read -r K X; do
    case $K in
      sens_h) H=$X ;;
      sens_v) V=$X ;;
    esac
  done < "$MODDIR/config"
fi
case $S in
  active) T="Flick diterapkan - X ${H}x, Y ${V}x" ;;
  idle) T="Flick tidak diterapkan" ;;
  *) exit 0 ;;
esac
/system/bin/cmd notification post -t Flick flick "$T"
