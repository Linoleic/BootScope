#!/system/bin/sh
# Read-only boot observations, stored in a private per-boot directory.
umask 077
BASE=/data/adb/bootscope-probe
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
case "$BOOT_ID" in
    ????????-????-????-????-????????????) ;;
    *) exit 1 ;;
esac
mkdir -p "$BASE" || exit 1
chmod 700 "$BASE" || exit 1
RUN="$BASE/$BOOT_ID"
# The atomic mkdir also prevents two copies from collecting the same boot.
mkdir "$RUN" 2>/dev/null || exit 0
chmod 700 "$RUN" || exit 1
{
    echo "format=2"
    echo "boot_id=$BOOT_ID"
    echo "started_uptime=$(cut -d ' ' -f 1 /proc/uptime 2>/dev/null)"
    echo "android_version=$(getprop ro.build.version.release)"
    echo "sdk=$(getprop ro.build.version.sdk)"
} > "$RUN/metadata.txt"

COUNT=0
COMPLETE_COUNT=0
while [ "$COUNT" -lt 60 ]; do
    {
        echo "--- sample=$COUNT uptime=$(cut -d ' ' -f 1 /proc/uptime 2>/dev/null) ---"
        getprop | grep -E '^\[(init\.svc\.|ro\.boottime\.|sys\.boot_completed\]|dev\.bootcomplete\]|service\.bootanim\.exit\])'
    } >> "$RUN/samples.txt" || exit 1
    COUNT=$((COUNT + 1))
    if [ "$(getprop sys.boot_completed)" = 1 ]; then
        COMPLETE_COUNT=$((COMPLETE_COUNT + 1))
        [ "$COMPLETE_COUNT" -ge 3 ] && break
    else
        COMPLETE_COUNT=0
    fi
    sleep 1
done
# Scan the available events buffer. A tail limit could discard startup entries
# before logcat applies its tag filters. Persist only tags with numeric payloads.
(
    timeout -k 1 3 logcat -b events -d -v tag -s \
        boot_progress_start:I boot_progress_preload_start:I boot_progress_preload_end:I \
        boot_progress_system_run:I boot_progress_pms_start:I boot_progress_pms_system_scan_start:I \
        boot_progress_pms_data_scan_start:I boot_progress_pms_scan_end:I boot_progress_pms_ready:I \
        boot_progress_ams_ready:I boot_progress_enable_screen:I sf_stop_bootanim:I \
        wm_boot_animation_done:I 2>"$RUN/logcat-error.txt"
    echo "$?" > "$RUN/logcat-exit.txt"
) | awk -F ':' '
    NF == 2 && $1 ~ /^[IWEF]\/(boot_progress_[a-z_]+|sf_stop_bootanim|wm_boot_animation_done)[[:space:]]*$/ &&
        $2 ~ /^[[:space:]]*[0-9]+[[:space:]]*$/ && count < 128 {
        sub(/^[IWEF]\//, "", $1)
        sub(/[[:space:]]+$/, "", $1)
        gsub(/[[:space:]]/, "", $2)
        print $1 "\t" $2
        count++
    }
' > "$RUN/boot-events.tsv"
{
    echo "finished_uptime=$(cut -d ' ' -f 1 /proc/uptime 2>/dev/null)"
    echo "samples=$COUNT"
    echo "boot_completed=$(getprop sys.boot_completed)"
    echo "events_logcat_exit=$(cat "$RUN/logcat-exit.txt" 2>/dev/null)"
    echo "events_count=$(wc -l < "$RUN/boot-events.tsv")"
} >> "$RUN/metadata.txt"
