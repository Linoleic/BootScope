#!/system/bin/sh
umask 077
case "$0" in
    */*) MODDIR=${0%/*} ;;
    *) MODDIR=/data/adb/modules/bootscope ;;
esac
[ -x "$MODDIR/bootscope" ] || MODDIR=/data/adb/modules/bootscope

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

EARLY_PID=$(cat /dev/bootscope_early.pid 2>/dev/null)
if [ -n "$EARLY_PID" ] && kill -0 "$EARLY_PID" 2>/dev/null; then
    WAIT_FIRST=0
    while kill -0 "$EARLY_PID" 2>/dev/null \
            && ! grep -q '^first_frame_submitted_ms=' /dev/bootscope_early.log 2>/dev/null \
            && [ "$WAIT_FIRST" -lt 8 ]; do
        sleep 0.25
        WAIT_FIRST=$((WAIT_FIRST + 1))
    done
    if grep -q '^first_frame_submitted_ms=' /dev/bootscope_early.log 2>/dev/null; then
        capture_compositor
        WAITED=0
        while kill -0 "$EARLY_PID" 2>/dev/null && [ "$WAITED" -lt 80 ]; do
            sleep 0.25
            WAITED=$((WAITED + 1))
        done
    fi
fi

if [ -s /dev/bootscope_early_trace.tsv ]; then
    cp -f /dev/bootscope_early_trace.tsv "$RUN/early-trace.tsv" 2>/dev/null
fi

if grep -q '^first_frame_submitted_ms=' /dev/bootscope_early.log 2>/dev/null; then
    cp -f /dev/bootscope_early.log "$RUN/display-renderer.txt" 2>/dev/null
    echo "mode=early_native" >> "$RUN/display-status.txt"
    echo "renderer_exit=0" >> "$RUN/display-status.txt"
    exit 0
fi

# Non-KSU / late-native fallback if early initrc was absent and bootanim is still active.
if [ -x "$MODDIR/bootscope" ] && [ "$(getprop init.svc.bootanim)" = "running" ] \
        && [ "$(getprop service.bootanim.exit)" != "1" ] \
        && [ "$(getprop sys.boot_completed)" != "1" ]; then
    "$MODDIR/bootscope" early >/dev/null 2>&1 &
    LATE_PID=$!
    WAIT_FIRST=0
    while kill -0 "$LATE_PID" 2>/dev/null \
            && ! grep -q '^first_frame_submitted_ms=' /dev/bootscope_early.log 2>/dev/null \
            && [ "$WAIT_FIRST" -lt 12 ]; do
        sleep 0.25
        WAIT_FIRST=$((WAIT_FIRST + 1))
    done
    if grep -q '^first_frame_submitted_ms=' /dev/bootscope_early.log 2>/dev/null; then
        capture_compositor
        wait "$LATE_PID" 2>/dev/null
        [ -s /dev/bootscope_early_trace.tsv ] && cp -f /dev/bootscope_early_trace.tsv "$RUN/early-trace.tsv" 2>/dev/null
        cp -f /dev/bootscope_early.log "$RUN/display-renderer.txt" 2>/dev/null
        echo "mode=late_native" >> "$RUN/display-status.txt"
        echo "renderer_exit=0" >> "$RUN/display-status.txt"
        exit 0
    fi
    wait "$LATE_PID" 2>/dev/null
fi

if [ -s /dev/bootscope_early.log ]; then
    cp -f /dev/bootscope_early.log "$RUN/display-early-error.txt" 2>/dev/null
fi
echo "mode=none" >> "$RUN/display-status.txt"
exit 0
