# Sourced by KernelSU module installer; do not call exit.
KSU_EARLY_DIR=/metadata/vold/ksu
if [ -f "$MODPATH/bootscope_early" ]; then
    chmod 0755 "$MODPATH/bootscope_early"
    mkdir -p "$KSU_EARLY_DIR"
    cp -f "$MODPATH/bootscope_early" "$KSU_EARLY_DIR/bootscope_early"
    chmod 0755 "$KSU_EARLY_DIR/bootscope_early"
    chcon u:object_r:ksu_file:s0 "$KSU_EARLY_DIR/bootscope_early" 2>/dev/null || true
    if [ -e /data/adb/bootscope/disable_display ] || [ -e "$MODPATH/disable_display" ]; then
        : > "$KSU_EARLY_DIR/bootscope_disable_display"
    else
        rm -f "$KSU_EARLY_DIR/bootscope_disable_display"
    fi
fi
