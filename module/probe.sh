#!/system/bin/sh
# Read-only boot observations, stored in a private per-boot directory.
umask 077
BASE=/data/adb/bootscope
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
case "$BOOT_ID" in
    ????????-????-????-????-????????????) ;;
    *) exit 1 ;;
esac
mkdir -p "$BASE" || exit 1
chmod 700 "$BASE" || exit 1
RUN="$BASE/$BOOT_ID"
mkdir -p "$RUN" || exit 1
chmod 700 "$RUN" || exit 1
# Atomic mkdir prevents two copies of probe.sh from collecting the same boot.
mkdir "$RUN/.probe.lock" 2>/dev/null || exit 0
{
    echo "format=3"
    echo "boot_id=$BOOT_ID"
    echo "started_uptime=$(cut -d ' ' -f 1 /proc/uptime 2>/dev/null)"
    echo "android_version=$(getprop ro.build.version.release)"
    echo "sdk=$(getprop ro.build.version.sdk)"
} > "$RUN/metadata.txt"

COUNT=0
COMPLETE_COUNT=0
while [ "$COUNT" -lt 60 ]; do
    PROPS=$(getprop | grep -E '^\[(init\.svc\.|ro\.boottime\.|ro\.boot\.boottime(_extra)?\]|sys\.boot_completed\]|dev\.bootcomplete\]|service\.bootanim\.exit\])')
    UPTIME=$(cut -d ' ' -f 1 /proc/uptime 2>/dev/null)
    {
        echo "--- sample=$COUNT uptime=$UPTIME ---"
        [ -n "$PROPS" ] && printf '%s\n' "$PROPS"
    } >> "$RUN/samples.txt" || exit 1
    if [ -r /proc/pressure/cpu ] && [ -r /proc/pressure/io ] && [ -r /proc/pressure/memory ]; then
        awk -v s="$COUNT" -v u="$UPTIME" '
            FNR == 1 { idx++ }
            $1 == "some" {
                for (i = 2; i <= NF; i++) {
                    if ($i ~ /^avg10=/) a = substr($i, 7)
                    if ($i ~ /^total=/) t = substr($i, 7)
                }
                some_t[idx] = t; some_a[idx] = a
            }
            $1 == "full" {
                for (i = 2; i <= NF; i++) {
                    if ($i ~ /^total=/) f = substr($i, 7)
                }
                full_t[idx] = f
            }
            END {
                printf "%s\t%s\t%s\t%s\t%.2f\t%s\t%s\t%.2f\t%s\t%s\t%.2f\n",
                    s, u,
                    (some_t[1] != "" ? some_t[1] : "0"), (full_t[1] != "" ? full_t[1] : "0"), some_a[1] + 0,
                    (some_t[2] != "" ? some_t[2] : "0"), (full_t[2] != "" ? full_t[2] : "0"), some_a[2] + 0,
                    (some_t[3] != "" ? some_t[3] : "0"), (full_t[3] != "" ? full_t[3] : "0"), some_a[3] + 0
            }
        ' /proc/pressure/cpu /proc/pressure/io /proc/pressure/memory >> "$RUN/psi.tsv" 2>/dev/null
    fi
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
# Extract sanitized early-boot init milestones and service lifecycles from dmesg.
(
    timeout -k 1 3 dmesg 2>"$RUN/dmesg-error.txt"
    echo "$?" > "$RUN/dmesg-exit.txt"
) | awk '
    BEGIN { sq = sprintf("%c", 39) }
    function emit(ts, kind, name, detail) {
        if (count < 1024) {
            printf "%.6f\t%s\t%s\t%s\n", ts, kind, name, detail
            count++
        }
    }
    {
        line = $0
        sub(/\r$/, "", line)
        if (!match(line, /^[[:space:]]*\[[[:space:]]*[0-9]+\.[0-9]+\][[:space:]]+init:[[:space:]]+/)) next
        prefix = substr(line, RSTART, RLENGTH)
        msg = substr(line, RSTART + RLENGTH)
        sub(/^[[:space:]]*\[[[:space:]]*/, "", prefix)
        sub(/\][[:space:]]+init:[[:space:]]+$/, "", prefix)
        ts = prefix + 0

        if (msg ~ /^Loaded [0-9]+ kernel modules took [0-9]+ ms$/) {
            split(msg, f, " ")
            emit(ts, "phase", "kernel_modules", f[2] "_modules_" f[6] "ms")
        } else if (index(msg, "Switching root to " sq) == 1) {
            target = substr(msg, 20)
            sub(sq "$", "", target)
            if (target ~ /^\/[A-Za-z0-9_.-]+$/) emit(ts, "phase", "switch_root", target)
        } else if (msg == "init second stage started!") {
            emit(ts, "phase", "second_stage", "started")
        } else if (msg ~ /^processing action \([A-Za-z0-9_.-]+\) from /) {
            act = msg
            sub(/^processing action \(/, "", act)
            sub(/\) from .*$/, "", act)
            if (!seen_act[act]++) emit(ts, "phase", act, "")
        } else if (index(msg, "starting service " sq) == 1 && msg ~ /\.\.\.$/) {
            svc = substr(msg, 19)
            sub(sq "\\.\\.\\.$", "", svc)
            if (svc ~ /^[A-Za-z0-9_.-]+$/) emit(ts, "start", svc, "")
        } else if (index(msg, "Service " sq) == 1 && msg ~ / exited with status [0-9]+ .* took [0-9.]+ seconds$/) {
            svc = substr(msg, 10)
            sub(sq " \\(pid .*$", "", svc)
            st = msg
            sub(/^.* exited with status /, "", st)
            sub(/ .*$/, "", st)
            tk = msg
            sub(/^.* took /, "", tk)
            sub(/ seconds$/, "", tk)
            if (svc ~ /^[A-Za-z0-9_.-]+$/ && st ~ /^[0-9]+$/ && tk ~ /^[0-9.]+$/) {
                emit(ts, "exit", svc, "status=" st ";took=" tk "s")
            }
        }
    }
' > "$RUN/init-events.tsv"
{
    echo "finished_uptime=$(cut -d ' ' -f 1 /proc/uptime 2>/dev/null)"
    echo "samples=$COUNT"
    echo "boot_completed=$(getprop sys.boot_completed)"
    echo "events_logcat_exit=$(cat "$RUN/logcat-exit.txt" 2>/dev/null)"
    echo "events_count=$(wc -l < "$RUN/boot-events.tsv" | tr -d ' ')"
    echo "dmesg_exit=$(cat "$RUN/dmesg-exit.txt" 2>/dev/null)"
    echo "init_events_count=$(wc -l < "$RUN/init-events.tsv" | tr -d ' ')"
    echo "psi_samples=$([ -f "$RUN/psi.tsv" ] && wc -l < "$RUN/psi.tsv" | tr -d ' ' || echo 0)"
} >> "$RUN/metadata.txt"
