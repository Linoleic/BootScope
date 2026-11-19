# Sourced by KernelSU / Magisk module installer; do not call exit.
chmod 0755 "$MODPATH/service.sh" "$MODPATH/probe.sh" 2>/dev/null || true
[ -f "$MODPATH/display.sh" ] && chmod 0755 "$MODPATH/display.sh" 2>/dev/null || true
[ -f "$MODPATH/uninstall.sh" ] && chmod 0755 "$MODPATH/uninstall.sh" 2>/dev/null || true
[ -f "$MODPATH/rules.conf" ] && chmod 0644 "$MODPATH/rules.conf" 2>/dev/null || true
[ -f "$MODPATH/bootscope_early" ] && chmod 0755 "$MODPATH/bootscope_early" 2>/dev/null || true

# Early initrc binary injection is KernelSU-specific and arm64-only.
ABI=$(getprop ro.product.cpu.abi 2>/dev/null)
if [ -n "$KSU" ] && [ "$ABI" = "arm64-v8a" ] && [ -f "$MODPATH/bootscope_early" ]; then
    KSU_EARLY_DIR=/metadata/vold/ksu
    mkdir -p "$KSU_EARLY_DIR"
    cp -f "$MODPATH/bootscope_early" "$KSU_EARLY_DIR/bootscope_early"
    chmod 0755 "$KSU_EARLY_DIR/bootscope_early"
    chcon u:object_r:ksu_file:s0 "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null || true
    RULES_SRC="$MODPATH/rules.conf"
    [ -f /data/adb/bootscope/rules.conf ] && RULES_SRC=/data/adb/bootscope/rules.conf
    if [ -f "$RULES_SRC" ]; then
        cp -f "$RULES_SRC" "$KSU_EARLY_DIR/bootscope_rules.conf"
        chmod 0644 "$KSU_EARLY_DIR/bootscope_rules.conf"
        chcon u:object_r:ksu_file:s0 "$KSU_EARLY_DIR/bootscope_rules.conf" 2>/dev/null || true
    fi
    if [ -e /data/adb/bootscope/disable_display ] || [ -e "$MODPATH/disable_display" ]; then
        : > "$KSU_EARLY_DIR/bootscope_disable_display"
    else
        rm -f "$KSU_EARLY_DIR/bootscope_disable_display"
    fi
fi
