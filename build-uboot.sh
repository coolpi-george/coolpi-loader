#!/bin/bash

BOARD=$1
RV1106="0"

case "$BOARD" in
  cp4b)
    cfg="rk3588s_coolpi4b" 
    ;;
  cm5)
    cfg="rk3588_coolpicm5" 
    ;;
  cm5-notebook)
    cfg="rk3588_coolpicm5_notebook"
    ;;
  nano)
    cfg="rv1106_coolpinano"
    RV1106="1"
    ;;
  cp1b)
    cfg="rv1106_coolpicp1b"
    RV1106="1"
    ;;
  cm3-zxfz)
    cfg="rk3566_coolpicm3_zxfz"
    ;;
  *)
    echo "Usage: $0 {cp4b|cm5|cm5-notebook|nano|cp1b|cm3-zxfz}" >&2
    exit 0
    ;;
esac

if [ $RV1106 == "1" ]; then
    U_SRC=`pwd`
    ARCH=`uname -m`
    if [ "$ARCH" == "x86_64" ]; then
        export CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
        TOOLCHAIN_ARM32=$U_SRC/toolchain32uc/bin
        export PATH=$TOOLCHAIN_ARM32:$PATH
    fi
    ./make.sh $cfg CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf- --spl-new
    if [ "$?" == "0" ]; then
        rm -rf ${cfg}_out
        mkdir -p ${cfg}_out
        cp -f rv1106_download*.bin ${cfg}_out/download.bin
        cp -f rv1106_idblock* ${cfg}_out/idblock.img
        cp uboot.img ${cfg}_out/
        cp nano-package-file ${cfg}_out/package-file
		if [ "$BOARD" == "nano" ]; then
			./tools/mkenvimage-nano -s 0x10000 -p 0x0 -o ${cfg}_out/env.img $U_SRC/nano-env.txt
		fi
		if [ "$BOARD" == "cp1b" ]; then
			./tools/mkenvimage -s 0x8000 -p 0x0 -o ${cfg}_out/env.img $U_SRC/nano-env-emmc.txt
		fi
        ./tools/afptool -pack ${cfg}_out ${cfg}_out/update_tmp.img || pause
        ./tools/rkImageMaker -RK1106 ${cfg}_out/download.bin ${cfg}_out/update_tmp.img ${cfg}_out/coolpi-nano.img -os_type:androidos || pause
        rm ${cfg}_out/update_tmp.img
        echo "Compile U-boot NANO OK!"
        md5sum ${cfg}_out/*
    fi
else
    ARCH=`uname -m`
    EXT_ARGS=""
    if [ "$ARCH" == "aarch64" ]; then
        EXT_ARGS="CROSS_COMPILE=/usr/bin/aarch64-linux-gnu-"
    fi
    ./make.sh $cfg $EXT_ARGS --spl-new 
    if [ "$?" == "0" ]; then
        rm -rf ${cfg}_out
        mkdir -p ${cfg}_out
        cp -f *_spl_loader_*.bin ${cfg}_out/loader.bin
        cp uboot.img ${cfg}_out/
        echo "Compile U-boot OK!"
        md5sum ${cfg}_out/*
        ./make.sh --idblock --spl
        dd if=/dev/zero of=${BOARD}_nor_upgrade.img bs=1K count=8192
		dd if=idblock.bin of=${BOARD}_nor_upgrade.img bs=1K seek=32
        dd if=idblock.bin of=${BOARD}_nor_upgrade.img bs=1K seek=544
	    dd if=idblock.bin of=${BOARD}_nor_upgrade.img bs=1K seek=1056
		dd if=${cfg}_out/uboot.img of=${BOARD}_nor_upgrade.img bs=1K seek=2048
    fi
fi
