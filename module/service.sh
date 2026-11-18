#!/system/bin/sh
# KernelSU runs service.sh in the background. Never wait for this collector.
MODDIR=${0%/*}
nohup /system/bin/sh "$MODDIR/probe.sh" </dev/null >/dev/null 2>&1 &
exit 0
