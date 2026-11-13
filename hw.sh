#!/system/bin/sh
# called by the daemon: hw.sh 1|0
# 1 = hw property on (setprop debug.sf.hw 1), the old value is kept in hw.prev
# 0 = restore the old value, safe to call any time (does nothing when nothing was changed)
MODDIR=${0%/*}
P=debug.sf.hw
F=$MODDIR/hw.prev
GETPROP=/system/bin/getprop
SETPROP=/system/bin/setprop
# debug.sf.hw is a legacy, undocumented debug property. the effect is device-dependent and the
# setprop call can be denied in shell mode; hw stays opt-in and its previous value is preserved.
if [ "$1" = 1 ]; then
  [ -f "$F" ] || "$GETPROP" "$P" > "$F"
  "$SETPROP" "$P" 1
elif [ -f "$F" ]; then
  "$SETPROP" "$P" "$(cat "$F")"
  rm -f "$F"
fi
