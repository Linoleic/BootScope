#!/system/bin/sh
# KernelSU runs service.sh during boot. Never block the module runner.
MODDIR=${0%/*}
KSU_EARLY_DIR=/metadata/vold/ksu

# Keep early-boot display binary and toggle synced on /metadata for next boot.
if [ -f "$MODDIR/bootscope_early" ]; then
    mkdir -p "$KSU_EARLY_DIR" 2>/dev/null
    if [ ! -f "$KSU_EARLY_DIR/bootscope_early" ] || ! cmp -s "$MODDIR/bootscope_early" "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null; then
        cp -f "$MODDIR/bootscope_early" "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null
        chmod 0755 "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null
        chcon u:object_r:ksu_file:s0 "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null || true
    fi
fi

if [ -e "$MODDIR/disable_display" ] || [ -e /data/adb/bootscope/disable_display ]; then
    : > "$KSU_EARLY_DIR/bootscope_disable_display" 2>/dev/null
else
    rm -f "$KSU_EARLY_DIR/bootscope_disable_display" 2>/dev/null
    if [ -f "$MODDIR/classes.dex" ] || [ -f "$MODDIR/bootscope_early" ]; then
        nohup /system/bin/sh "$MODDIR/display.sh" </dev/null >/dev/null 2>&1 &
    fi
fi

nohup /system/bin/sh "$MODDIR/probe.sh" </dev/null >/dev/null 2>&1 &
exit 0
