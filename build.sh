set -e

timestamp=$(date '+%s')

export LOCALVERSION="$timestamp"

#export VARIANT_DEFCONFIG=ix_jf_defconfig; make ix_jf_defconfig

make -j8

rm build_dir/system/lib/modules/* || true

find . -name *.ko -exec cp {} build_dir/system/lib/modules \;

cd ramdisk
find . | cpio -o -H newc | gzip > boot.img-ramdisk.gz

cd ../build_dir

../../packaging/mkbootimg.new --kernel ../arch/arm/boot/zImage --ramdisk ../ramdisk/boot.img-ramdisk.gz --cmdline "console=null androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x3F ehci-hcd.park=3" --base "0x80200000" --ramdisk_offset "0x82200000" --output boot.img

zip -r ~/Google\ Drive/Kernels/TW4.3/iX-TW-4.3-MJ5-jf-Kernel-"$timestamp"$1.zip *

echo $timestamp

cd ..

git tag -a $timestamp -m buildtag
