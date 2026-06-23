#! /bin/bash

TOP_PATH=$(pwd)
KERNEL_PATH=$(pwd)/../kernel
PX3SE_PATH=$(pwd)/../device/rockchip/px3-se
ROOTFS_BASE=$(pwd)/rootfs
ROOTFS_PATH=$(pwd)/rootfs/target
FSOVERLAY_PATH=$(pwd)/rootfs/rockchip/px3se/fs-overlay
RECOVERY_OUT=$(pwd)/recoveryImg
RAMDISK_TOOL_PATH=$(pwd)/tools/ramdisk_tool/
ROCKCHIP_GCC=$(pwd)/../buildroot/output/host/usr/bin/arm-rockchip-linux-gnueabihf-gcc
UPDATER_PATH=$(pwd)/updater-1.0

case "$1" in
	[sS][lL][cC])
	DTS=px3se-recovery-slc-sdk.img
	RECOVERY_NAME=recovery_slc.img
	echo "make slc recovery img"
	;;
	*)
	DTS=px3se-recovery-sdk.img
	RECOVERY_NAME=recovery_emmc.img
	echo "make emmc recovery img"
	;;
esac

export PATH=$PATH:${RAMDISK_TOOL_PATH}

echo "make recovery start..."

#初始化操作（清理旧文件）
rm -rf $RECOVERY_OUT
mkdir -p $RECOVERY_OUT
rm -rf $ROOTFS_BASE

echo "make recovery rootfs..."

echo -n "tar xf resource/rootfs.tar.gz..."
tar zxf resource/rootfs.tar.gz
echo "done."

#拷贝升级相关脚本。
cp -f $FSOVERLAY_PATH/busybox $ROOTFS_PATH/bin/
cp -f $FSOVERLAY_PATH/S50_updater_init $ROOTFS_PATH/etc/init.d/
cp -f $FSOVERLAY_PATH/RkUpdater.sh $ROOTFS_PATH/etc/profile.d/
cp -f $FSOVERLAY_PATH/init $ROOTFS_PATH/init

if [ "$1"x = "slc"x -o "$1"x = "SLC"x ]; then
	touch $ROOTFS_PATH/proc/rknand
fi

#编译Updater
if [ ! -f "$ROCKCHIP_GCC" ]; then
	echo "ERROR:Please make buildroot first!"
	exit 0
fi
echo "make updater programe..."
cd $UPDATER_PATH && rm -f updater
$ROCKCHIP_GCC -g2 -o updater updater.c recovery_display.c
if [ ! -f "$UPDATER_PATH/updater" ]; then
	echo "ERROR:updater make failed! Please check updater code."
	exit 0
fi
cp -f $UPDATER_PATH/updater $ROOTFS_PATH/usr/bin/

echo "make recovery kernel..."
cd $KERNEL_PATH
make ARCH=arm clean -j4 && make ARCH=arm px3se_recovery_emmc_defconfig -j8 && make ARCH=arm $DTS -j12

echo "cp kernel.img..."
cp $KERNEL_PATH/kernel.img $RECOVERY_OUT

echo "cp resource.img..."
cp $KERNEL_PATH/resource.img $RECOVERY_OUT

echo "revert kernel defconfig"
make ARCH=arm clean -j4 && make ARCH=arm px3se_linux_defconfig && make ARCH=arm px3se-sdk.img -j12

echo "create recovery.img with kernel..."

mkbootfs $ROOTFS_PATH | minigzip > $RECOVERY_OUT/ramdisk-recovery.img && \
	truncate -s "%4" $RECOVERY_OUT/ramdisk-recovery.img && \
mkbootimg --kernel $RECOVERY_OUT/kernel.img --ramdisk $RECOVERY_OUT/ramdisk-recovery.img --second $RECOVERY_OUT/resource.img --output $RECOVERY_OUT/recovery.img

cp $RECOVERY_OUT/recovery.img $PX3SE_PATH/rockimg/$RECOVERY_NAME
rm -rf $RECOVERY_OUT/
cd $TOP_PATH

echo "make recovery image ok !"
