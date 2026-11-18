#!/system/bin/sh
# KernelSU runs service.sh during boot. Never block the module runner.
MODDIR=${0%/*}
nohup /system/bin/sh "$MODDIR/probe.sh" </dev/null >/dev/null 2>&1 &
if [ -f "$MODDIR/classes.dex" ] \
        && [ ! -e "$MODDIR/disable_display" ] \
        && [ ! -e /data/adb/bootscope/disable_display ]; then
    nohup /system/bin/sh "$MODDIR/display.sh" </dev/null >/dev/null 2>&1 &
fi
exit 0
