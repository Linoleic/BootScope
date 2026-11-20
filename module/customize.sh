# Sourced by KernelSU / Magisk module installer; do not call exit.
chmod 0755 "$MODPATH/bootscope" "$MODPATH/service.sh" "$MODPATH/probe.sh" "$MODPATH/display.sh" "$MODPATH/action.sh" "$MODPATH/uninstall.sh" 2>/dev/null || true
[ -f "$MODPATH/rules.conf" ] && chmod 0644 "$MODPATH/rules.conf" 2>/dev/null || true

# Early initrc binary injection is KernelSU-specific and arm64-only.
ABI=$(getprop ro.product.cpu.abi 2>/dev/null)
if [ -n "$KSU" ] && [ "$ABI" = "arm64-v8a" ] && [ -f "$MODPATH/bootscope" ]; then
    KSU_EARLY_DIR=/metadata/vold/ksu
    mkdir -p "$KSU_EARLY_DIR"
    cp -f "$MODPATH/bootscope" "$KSU_EARLY_DIR/bootscope"
    chmod 0755 "$KSU_EARLY_DIR/bootscope"
    chcon u:object_r:ksu_file:s0 "$KSU_EARLY_DIR/bootscope" 2>/dev/null || true
    rm -f "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null || true

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

# Request KernelSU rebootless hot install when early initrc entry is already active.
if [ "$KSU_HAS_HOT_INSTALL" = "true" ]; then
    if [ "$ABI" != "arm64-v8a" ] || grep -Fq '/metadata/vold/ksu/bootscope early' /metadata/vold/ksu/modules.rc 2>/dev/null; then
        export MODULE_HOT_INSTALL_REQUEST=true
    fi
fi
