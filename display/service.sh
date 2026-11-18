#!/system/bin/sh
# KernelSU runs service.sh during boot. Never wait on the display overlay.
MODDIR=${0%/*}
nohup /system/bin/sh "$MODDIR/run.sh" </dev/null >/dev/null 2>&1 &
exit 0
