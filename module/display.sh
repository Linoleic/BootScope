#!/system/bin/sh
umask 077
MODDIR=${0%/*}
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
case "$BOOT_ID" in
    ????????-????-????-????-????????????) ;;
    *) exit 1 ;;
esac
BASE=/data/adb/bootscope
mkdir -p "$BASE" || exit 1
chmod 700 "$BASE" || exit 1
RUN="$BASE/$BOOT_ID"
mkdir -p "$RUN" || exit 1
chmod 700 "$RUN" || exit 1
# Atomic mkdir prevents two copies of display.sh from launching on the same boot.
mkdir "$RUN/.display.lock" 2>/dev/null || exit 0
{
    echo "service_uptime=$(cut -d ' ' -f 1 /proc/uptime 2>/dev/null)"
    echo "bootanim=$(getprop init.svc.bootanim)"
    echo "bootanim_exit=$(getprop service.bootanim.exit)"
} > "$RUN/display-status.txt"

capture_compositor() {
    timeout -k 1 2 /system/bin/dumpsys SurfaceFlinger | awk '
        /Layer \[[0-9]+\] (\[BBQ\] )?BootScopeDisplay#[0-9]+/ {
            print
            if (getline > 0 && $0 ~ /^[[:space:]]+(visible|invisible) reason=/) print
            if (getline > 0 && $0 ~ /^[[:space:]]+bounds=/) print
            exit
        }
    ' > "$RUN/display-compositor.txt"
}

# If the native early-boot display overlay already started in initrc, collect its
# compositor snapshot and log instead of spawning a duplicate Java overlay.
EARLY_PID=$(cat /dev/bootscope_early.pid 2>/dev/null)
if [ -n "$EARLY_PID" ] || [ -s /dev/bootscope_early.log ]; then
    echo "mode=early_native" >> "$RUN/display-status.txt"
    if [ -n "$EARLY_PID" ] && kill -0 "$EARLY_PID" 2>/dev/null; then
        capture_compositor
        WAITED=0
        while kill -0 "$EARLY_PID" 2>/dev/null && [ "$WAITED" -lt 80 ]; do
            sleep 0.25
            WAITED=$((WAITED + 1))
        done
    fi
    if [ -s /dev/bootscope_early.log ]; then
        cp -f /dev/bootscope_early.log "$RUN/display-renderer.txt" 2>/dev/null
    fi
    echo "renderer_exit=0" >> "$RUN/display-status.txt"
    exit 0
fi

# Fallback: ART Java DisplayProbe (only when early native probe did not run).
if [ ! -f "$MODDIR/classes.dex" ]; then
    echo "mode=none" >> "$RUN/display-status.txt"
    exit 0
fi
echo "mode=java_fallback" >> "$RUN/display-status.txt"

(
    for attempt in 1 2 3 4 5 6 7 8; do
        if grep -q '^first_frame_submitted_ms=' "$RUN/display-renderer.txt" 2>/dev/null; then
            sleep 0.4
            capture_compositor
            exit
        fi
        sleep 0.25
    done
) &
observer=$!
chmod 444 "$MODDIR/classes.dex" || exit 1
CLASSPATH="$MODDIR/classes.dex" timeout -k 1 15 /system/bin/app_process \
    /system/bin --nice-name=bootscope-display bootscope.DisplayProbe \
    > "$RUN/display-renderer.txt" 2>&1
echo "renderer_exit=$?" >> "$RUN/display-status.txt"
wait "$observer"
