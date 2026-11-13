A=$ARCH
case "$A" in arm64|arm) ;; *)
  case "$(/system/bin/getprop ro.product.cpu.abi)" in
    arm64*) A=arm64 ;;
    armeabi*) A=arm ;;
    *) abort "Only arm64 and arm (32-bit) are supported" ;;
  esac
esac
BIN=$MODPATH/system/bin
[ -f "$BIN/flick.$A" ] || abort "Binary for $A is missing"

# stop an older running instance, its binary cannot be overwritten while it runs. axmanager also
# stops the processes of an updated plugin, this covers the old Vektor (id sensit) as well: its
# daemon would still hold the touchscreen. MODPATH is $AXERONDIR/plugins/<id> during customize.sh,
# so its parent is the active plugins folder in both root and shell mode. do not hardcode either
# mode's path, they differ and the server can switch between them.
PL=${MODPATH%/*}
case "$PL" in
  /*) ;;
  *) [ -n "$AXERONDIR" ] && PL=$AXERONDIR/plugins ;;
esac
[ -n "$PL" ] || abort "Cannot locate the plugins folder"
for F in "$PL/flick/flick.pid" "$PL/sensit/sensit.pid"; do
  [ -f "$F" ] && kill "$(cat "$F")" 2>/dev/null
done

mv -f "$BIN/flick.$A" "$BIN/flick"
rm -f "$BIN/flick.arm" "$BIN/flick.arm64"
chmod 755 "$BIN/flick"
ui_print "Flick installed ($A). Open its WebUI to use it."
