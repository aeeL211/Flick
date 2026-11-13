MODDIR=${0%/*}
# ctl.sh stops a live flick, restores debug.sf.hw and drops the pid file. without it only the property is restored
if [ -f "$MODDIR/ctl.sh" ]; then
  /system/bin/sh "$MODDIR/ctl.sh" stop
elif [ -f "$MODDIR/hw.sh" ]; then
  /system/bin/sh "$MODDIR/hw.sh" 0
fi
exit 0
