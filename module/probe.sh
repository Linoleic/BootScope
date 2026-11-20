#!/system/bin/sh
case "$0" in
    */*) MODDIR=${0%/*} ;;
    *) MODDIR=/data/adb/modules/bootscope ;;
esac
[ -x "$MODDIR/bootscope" ] || MODDIR=/data/adb/modules/bootscope
[ -x "$MODDIR/bootscope" ] || exit 1
exec "$MODDIR/bootscope" collect "$@"
