#!/sbin/busybox sh
# Adam kernel script (Root helper by Wanam)

/sbin/busybox mount -t rootfs -o remount,rw rootfs

#Disable knox
pm disable com.sec.knox.seandroid
setenforce 0

chown 0.0 /system/xbin/su
chmod 06755 /system/xbin/su
symlink /system/xbin/su /system/bin/su

chown 0.0 /system/xbin/daemonsu
chmod 06755 /system/xbin/daemonsu


chown 0.0 /system/app/Superuser.apk
chmod 0644 /system/app/Superuser.apk

if [ ! -f /system/xbin/busybox ]; then
ln -s /sbin/busybox /system/xbin/busybox
ln -s /sbin/busybox /system/xbin/pkill
fi

if [ ! -f /system/bin/busybox ]; then
ln -s /sbin/busybox /system/bin/busybox
ln -s /sbin/busybox /system/bin/pkill
fi

/system/bin/setprop pm.sleep_mode 1
/system/bin/setprop ro.ril.disable.power.collapse 0
/system/bin/setprop ro.telephony.call_ring.delay 1000

sync

/system/xbin/daemonsu --auto-daemon &

if [ -d /system/etc/init.d ]; then
  /sbin/busybox run-parts /system/etc/init.d
fi

/sbin/busybox mount -t rootfs -o remount,ro rootfs
