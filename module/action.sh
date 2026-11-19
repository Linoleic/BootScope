#!/system/bin/sh

BASE=/data/adb/bootscope

find_latest_run() {
    for d in $(ls -1dt "$BASE"/*/ 2>/dev/null); do
        d=${d%/}
        [ -f "$d/metadata.txt" ] || [ -f "$d/samples.txt" ] || [ -f "$d/display-renderer.txt" ] || continue
        echo "$d"
        return 0
    done
    return 1
}

RUN_DIR=$(find_latest_run)
if [ -z "$RUN_DIR" ]; then
    echo "BootScope: no recorded boot found in $BASE"
    exit 0
fi

echo "=== BootScope Last Boot Replay ==="
echo "dir=$RUN_DIR"

if [ -f "$RUN_DIR/metadata.txt" ]; then
    echo ""
    echo "[Metadata]"
    cat "$RUN_DIR/metadata.txt"
fi

if [ -f "$RUN_DIR/display-status.txt" ] || [ -f "$RUN_DIR/display-renderer.txt" ]; then
    echo ""
    echo "[Display]"
    [ -f "$RUN_DIR/display-status.txt" ] && cat "$RUN_DIR/display-status.txt"
    [ -f "$RUN_DIR/display-renderer.txt" ] && cat "$RUN_DIR/display-renderer.txt"
fi

if [ -f "$RUN_DIR/bootloader-kpi.tsv" ] || [ -f "$RUN_DIR/samples.txt" ]; then
    echo ""
    echo "[Bootloader]"
    if [ -f "$RUN_DIR/bootloader-kpi.tsv" ]; then
        awk -F '\t' 'NF>=2 { printf "  %-16s %s\n", $1, $2 }' "$RUN_DIR/bootloader-kpi.tsv"
    fi
    if [ -f "$RUN_DIR/samples.txt" ]; then
        grep -m 1 '^\[ro\.boot\.boottime' "$RUN_DIR/samples.txt" 2>/dev/null | sed 's/^/  /'
    fi
fi

if [ -f "$RUN_DIR/init-events.tsv" ]; then
    echo ""
    echo "[Init Milestones]"
    awk -F '\t' '$2=="phase" { printf "  %7.3fs  %-24s %s\n", $1, $3, $4 }' "$RUN_DIR/init-events.tsv" | head -n 16
    echo ""
    echo "[Top Init Services]"
    awk -F '\t' '$2=="exit" && $4 ~ /took=/ {
        tk=$4; sub(/^.*took=/, "", tk); sub(/s.*$/, "", tk);
        printf "%8.3f\t%7.3fs\t%-32s\t%s\n", tk + 0, $1, $3, $4
    }' "$RUN_DIR/init-events.tsv" | sort -rn | head -n 10 | cut -f2- | sed 's/^/  /'
fi

if [ -f "$RUN_DIR/boot-events.tsv" ]; then
    echo ""
    echo "[Framework Boot Events]"
    awk -F '\t' 'NF>=2 { printf "  %8.3fs  %s\n", $2 / 1000.0, $1 }' "$RUN_DIR/boot-events.tsv" | head -n 20
fi

if [ -f "$RUN_DIR/psi.tsv" ]; then
    echo ""
    echo "[PSI Summary]"
    tail -n 8 "$RUN_DIR/psi.tsv" | sed 's/^/  /'
fi

if [ -s "$RUN_DIR/dmesg-error.txt" ] || [ -s "$RUN_DIR/logcat-error.txt" ]; then
    echo ""
    echo "[Recent Errors]"
    [ -s "$RUN_DIR/dmesg-error.txt" ] && tail -n 5 "$RUN_DIR/dmesg-error.txt" | sed 's/^/  [dmesg] /'
    [ -s "$RUN_DIR/logcat-error.txt" ] && tail -n 5 "$RUN_DIR/logcat-error.txt" | sed 's/^/  [logcat] /'
fi

echo ""
echo "=== Replay Complete ==="
