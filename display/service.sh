#!/system/bin/sh
umask 077
MODDIR=$(dirname "$0")
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
case "$BOOT_ID" in
    ????????-????-????-????-????????????) ;;
    *) exit 1 ;;
esac
BASE=/data/adb/bootscope-display
mkdir -p "$BASE" && chmod 700 "$BASE" || exit 1
RUN="$BASE/$BOOT_ID"
mkdir "$RUN" || exit 1
{
    echo "service_uptime=$(cut -d ' ' -f 1 /proc/uptime)"
    echo "bootanim=$(getprop init.svc.bootanim)"
    echo "bootanim_exit=$(getprop service.bootanim.exit)"
} > "$RUN/status.txt"

# Keep the diagnostic read separate so it cannot hold up frame submission.
(
    for attempt in 1 2 3 4 5 6 7 8; do
        if grep -q '^first_frame_submitted_ms=' "$RUN/renderer.txt" 2>/dev/null; then
            sleep 0.4
            timeout -k 1 2 /system/bin/dumpsys SurfaceFlinger | awk '
                /Layer \[[0-9]+\] BootScopeDisplay#[0-9]+$/ {
                    print
                    if (getline > 0 && $0 ~ /^[[:space:]]+(visible|invisible) reason=/) print
                    if (getline > 0 && $0 ~ /^[[:space:]]+bounds=/) print
                    exit
                }
            ' > "$RUN/compositor.txt"
            exit
        fi
        sleep 0.25
    done
) &
observer=$!
# ART requires a read-only DEX. Runtime cleanup is also bounded by timeout.
chmod 444 "$MODDIR/classes.dex" || exit 1
CLASSPATH="$MODDIR/classes.dex" timeout -k 1 15 /system/bin/app_process \
    /system/bin --nice-name=bootscope-display bootscope.DisplayProbe \
    > "$RUN/renderer.txt" 2>&1
echo "renderer_exit=$?" >> "$RUN/status.txt"
wait "$observer"
