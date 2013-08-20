set -e

timestamp=$(date '+%s')

export LOCALVERSION="$timestamp"

#export VARIANT_DEFCONFIG=ix_jf_defconfig

make -j8

rm build_dir/system/lib/modules/* || true

find . -name *.ko -exec cp {} build_dir/system/lib/modules \;

cd ramdisk
find . | cpio -o -H newc | gzip > boot.img-ramdisk.gz

cd ../build_dir

../../packaging/mkbootimg.new --kernel ../arch/arm/boot/zImage --ramdisk ../ramdisk/boot.img-ramdisk.gz --ramdisk_offset 0x02000000 --base 0x80200000 --pagesize 2048 --board MSM8960 --cmdline "androidboot.hardware=qcom user_debug=31 zcache" --output boot.img 

zip -r ~/Google\ Drive/Kernels/CM10.2/iX-CM10.2-jf-Kernel-"$timestamp".zip *

echo $timestamp

cd ..

git tag -a $timestamp -m buildtag
