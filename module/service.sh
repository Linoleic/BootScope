#!/system/bin/sh
# Runs during late_start service on KernelSU / Magisk / APatch. Never block.
MODDIR=${0%/*}
ABI=$(getprop ro.product.cpu.abi 2>/dev/null)

if { [ -n "$KSU" ] || [ -d /data/adb/ksu ]; } && [ "$ABI" = "arm64-v8a" ]; then
    KSU_EARLY_DIR=/metadata/vold/ksu
    if [ -f "$MODDIR/bootscope_early" ]; then
        mkdir -p "$KSU_EARLY_DIR" 2>/dev/null
        if [ ! -f "$KSU_EARLY_DIR/bootscope_early" ] || ! cmp -s "$MODDIR/bootscope_early" "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null; then
            cp -f "$MODDIR/bootscope_early" "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null
            chmod 0755 "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null
            chcon u:object_r:ksu_file:s0 "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null || true
        fi
    fi
    RULES_SRC="$MODDIR/rules.conf"
    [ -f /data/adb/bootscope/rules.conf ] && RULES_SRC=/data/adb/bootscope/rules.conf
    if [ -f "$RULES_SRC" ]; then
        if [ ! -f "$KSU_EARLY_DIR/bootscope_rules.conf" ] || ! cmp -s "$RULES_SRC" "$KSU_EARLY_DIR/bootscope_rules.conf" 2>/dev/null; then
            cp -f "$RULES_SRC" "$KSU_EARLY_DIR/bootscope_rules.conf" 2>/dev/null
            chmod 0644 "$KSU_EARLY_DIR/bootscope_rules.conf" 2>/dev/null
            chcon u:object_r:ksu_file:s0 "$KSU_EARLY_DIR/bootscope_rules.conf" 2>/dev/null || true
        fi
    fi
    if [ -e "$MODDIR/disable_display" ] || [ -e /data/adb/bootscope/disable_display ]; then
        : > "$KSU_EARLY_DIR/bootscope_disable_display" 2>/dev/null
    else
        rm -f "$KSU_EARLY_DIR/bootscope_disable_display" 2>/dev/null
    fi
fi

if [ ! -e "$MODDIR/disable_display" ] && [ ! -e /data/adb/bootscope/disable_display ]; then
    if [ -f "$MODDIR/classes.dex" ] || [ -f "$MODDIR/bootscope_early" ]; then
        nohup /system/bin/sh "$MODDIR/display.sh" </dev/null >/dev/null 2>&1 &
    fi
fi

nohup /system/bin/sh "$MODDIR/probe.sh" </dev/null >/dev/null 2>&1 &
exit 0
