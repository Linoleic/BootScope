#!/system/bin/sh
case "$0" in
    */*) MODDIR=${0%/*} ;;
    *) MODDIR=/data/adb/modules/bootscope ;;
esac
[ -x "$MODDIR/bootscope" ] || MODDIR=/data/adb/modules/bootscope
if [ ! -x "$MODDIR/bootscope" ]; then
    echo "BootScope: missing native binary $MODDIR/bootscope"
    exit 1
fi

if [ "$1" != "--text-only" ]; then
    echo "Launching BootScope overlay replay..."
    "$MODDIR/bootscope" replay latest --bl --pre-sf 2>&1
fi

exec "$MODDIR/bootscope" summary latest
