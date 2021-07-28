#!/bin/bash
#
# Cronos Build Script - TWRP
# AnanJaser1211 @2021
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software

# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Main Dir
CR_DIR=$(pwd)
CR_OUT=$CR_DIR/releases
# Define toolchan path
CR_TC=~/Android/Toolchains/arm-eabi-4.8/bin/arm-eabi-
# Define proper arch and dir for dts files
CR_DTS=arch/arm/boot/dts
# Compiled image name and location (Image/zImage)
CR_KERNEL=$CR_DIR/arch/arm/boot/zImage
# Kernel Name and Version
CR_VERSION=twrp-7.1
CR_NAME=universal5433
# Thread count
CR_JOBS=$((`nproc`-1))
# Target android version and platform (7/n/8/o)
CR_ANDROID=7
CR_PLATFORM=7.0.0
# Target ARCH
CR_ARCH=arm
# Current Date
CR_DATE=$(date +%Y%m%d)
# Init build
export CROSS_COMPILE=$CR_TC
# General init
export ANDROID_MAJOR_VERSION=$CR_ANDROID
export PLATFORM_VERSION=$CR_PLATFORM
export $CR_ARCH
##########################################
# Device specific Variables [SM-N910C/H]
CR_DTSFILES_N910CH="exynos5433-tre_eur_open_07.dtb exynos5433-tre_eur_open_08.dtb exynos5433-tre_eur_open_09.dtb exynos5433-tre_eur_open_10.dtb exynos5433-tre_eur_open_12.dtb exynos5433-tre_eur_open_13.dtb exynos5433-tre_eur_open_14.dtb exynos5433-tre_eur_open_16.dtb"
CR_CONFG_N910CH=twrp_trelte_defconfig
CR_VARIANT_N910CH=treltexx
# Device specific Variables [SM-N910U]
CR_DTSFILES_N910U="exynos5433-trlte_eur_open_00.dtb exynos5433-trlte_eur_open_09.dtb exynos5433-trlte_eur_open_10.dtb exynos5433-trlte_eur_open_11.dtb exynos5433-trlte_eur_open_12.dtb"
CR_CONFG_N910U=twrp_trhpltexx_defconfig
CR_VARIANT_N910U=trhpltexx
# Device specific Variables [SM-N910S/L/K]
CR_DTSFILES_N910SLK="exynos5433-trelte_kor_open_06.dtb exynos5433-trelte_kor_open_07.dtb exynos5433-trelte_kor_open_09.dtb exynos5433-trelte_kor_open_11.dtb exynos5433-trelte_kor_open_12.dtb"
CR_CONFG_N910SLK=twrp_trelteskt_defconfig
CR_VARIANT_N910SLK=trelteskt
# Device specific Variables [SM-N915S/L/K]
CR_DTSFILES_N915SLK="exynos5433-tbelte_kor_open_07.dtb exynos5433-tbelte_kor_open_09.dtb exynos5433-tbelte_kor_open_11.dtb exynos5433-tbelte_kor_open_12.dtb exynos5433-tbelte_kor_open_14.dtb"
CR_CONFG_N915SLK=twrp_tbelteskt_defconfig
CR_VARIANT_N915SLK=tbelteskt
# Device specific Variables [SM-N916S/L/K]
CR_DTSFILES_N916SLK="exynos5433-tre3calte_kor_open_05.dtb exynos5433-tre3calte_kor_open_14.dtb"
CR_CONFG_N916SLK=twrp_tre3calte_defconfig
CR_VARIANT_N916SLK=tre3calte
##########################################

# Script functions

# Clean-up Function

BUILD_CLEAN()
{
if [ $CR_CLEAN = "y" ]; then
     echo "Clean Build"
     make clean && make mrproper
     rm -r -f $CR_DTB
     rm -rf $CR_DTS/.*.tmp
     rm -rf $CR_DTS/.*.cmd
     rm -rf $CR_DTS/*.dtb
     rm -rf $CR_DIR/.config
fi
if [ $CR_CLEAN = "n" ]; then
     echo " "
     echo " Skip Full cleaning"
     rm -r -f $CR_DTB
     rm -rf $CR_DTS/.*.tmp
     rm -rf $CR_DTS/.*.cmd
     rm -rf $CR_DTS/*.dtb
fi
}


BUILD_IMAGE_NAME()
{
	CR_IMAGE_NAME=$CR_NAME-$CR_VERSION-$CR_VARIANT-$CR_DATE
}

# Kernel information Function
BUILD_OUT()
{
	echo " "
	echo "----------------------------------------------"
	echo "$CR_VARIANT kernel build finished."
	echo "Compiled DTB Size = $sizdT Kb"
	echo "Kernel Image Size = $sizT Kb"
	echo "$CR_DEVICE Ready at $CR_OUT"
	echo "Press Any key to end the script"
	echo "----------------------------------------------"
}

BUILD_ZIMAGE()
{
	echo "----------------------------------------------"
	echo " "
	echo "Building zImage for $CR_VARIANT"
	export LOCALVERSION=-$CR_IMAGE_NAME
	echo "Make $CR_CONFG"
	make  $CR_CONFG
	make -j$CR_JOBS
	if [ ! -e $CR_KERNEL ]; then
	exit 0;
	echo "Image Failed to Compile"
	echo " Abort "
	fi
	du -k "$CR_KERNEL" | cut -f1 >sizT
	sizT=$(head -n 1 sizT)
	rm -rf sizT
	# Move Compiled kernel to root
	echo " "
	echo "----------------------------------------------"
}
BUILD_DTB()
{
	echo "----------------------------------------------"
	echo " "
	echo "Building DTB for $CR_VARIANT"
	# Dont use the DTS list provided in the build script.
	# This source compiles dtbs while doing Image
	#make  $CR_CONFG
	#make $CR_DTSFILES
	./tools/dtbtool -o ./boot.img-dtb -v -s 2048 -p ./scripts/dtc/ $CR_DTS/
	if [ ! -e $CR_DIR/boot.img-dtb ]; then
	exit 0;
	echo "DTB Failed to Compile"
	echo " Abort "
	fi
	du -k "$CR_DIR/boot.img-dtb" | cut -f1 >sizdT
	sizdT=$(head -n 1 sizdT)
	rm -rf sizdT
	rm -rf $CR_DTS/.*.tmp
	rm -rf $CR_DTS/.*.cmd
	rm -rf $CR_DTS/*.dtb
	echo " "
	echo "----------------------------------------------"
}

BUILD_EXPORT()
{
    echo "----------------------------------------------"
    echo " "
    if [ ! -e $CR_OUT ]; then
    echo "Creating releases Dir"
	mkdir $CR_OUT
    fi
	echo " Moving Kernel and DTB to releases "
	mv $CR_DIR/boot.img-dtb $CR_OUT/dt.img-$CR_VARIANT
	mv $CR_KERNEL $CR_OUT/kernel-$CR_VARIANT
    echo " "
    echo "----------------------------------------------"    
}

BUILD(){
	if [ "$CR_TARGET" = "1" ]; then
		echo " Treltexx (N910C/H) "
		echo " Starting $CR_VARIANT_N910CH kernel build..."
		CR_CONFG=$CR_CONFG_N910CH
		CR_DTSFILES=$CR_DTSFILES_N910CH
		CR_VARIANT=$CR_VARIANT_N910CH
	fi
	if [ "$CR_TARGET" = "2" ]; then
		echo " Trelteskt SM-N910S/L/K "
		echo " Starting $CR_VARIANT_N910SLK kernel build..."
		CR_CONFG=$CR_CONFG_N910SLK
		CR_DTSFILES=$CR_DTSFILES_N910SLK
		CR_VARIANT=$CR_VARIANT_N910SLK
	fi
	if [ "$CR_TARGET" = "3" ]; then
		echo " Tbelteskt (N915S/L/K) "
		echo " Starting $CR_VARIANT_N915SLK kernel build..."
		CR_CONFG=$CR_CONFG_N915SLK
		CR_DTSFILES=$CR_DTSFILES_N915SLK
		CR_VARIANT=$CR_VARIANT_N915SLK
	fi
	if [ "$CR_TARGET" = "4" ]; then
		echo " Tre3calteskt (N916S/L/K) "
		echo " Starting $CR_VARIANT_N916SLK kernel build..."
		CR_CONFG=$CR_CONFG_N916SLK
		CR_DTSFILES=$CR_DTSFILES_N916SLK
		CR_VARIANT=$CR_VARIANT_N916SLK
	fi
	if [ "$CR_TARGET" = "5" ]; then
		echo " Trhpltexx (N910U) "
		echo " Starting $CR_VARIANT_N910U kernel build..."
		CR_CONFG=$CR_CONFG_N910U
		CR_DTSFILES=$CR_DTSFILES_N910U
		CR_VARIANT=$CR_VARIANT_N910U
	fi
	BUILD_IMAGE_NAME
	BUILD_CLEAN
	BUILD_ZIMAGE
	BUILD_DTB
	BUILD_EXPORT
	BUILD_OUT
	# Final sanity check
	if [ ! -e $CR_OUT/dt.img-$CR_VARIANT ]; then
	echo "$CR_VARIANT DTB Failed to Compile"
	echo " Abort "
	exit 0;
	fi
	if [ ! -e $CR_OUT/kernel-$CR_VARIANT ]; then
	echo "$CR_VARIANT Kernel Failed to Compile"
	echo " Abort "
	exit 0;
	fi
}

# Multi-Target Build Function
BUILD_ALL(){
echo "----------------------------------------------"
echo " Compiling ALL targets "
CR_TARGET=1
BUILD
echo " $CR_VARIANT Compiled"
CR_TARGET=2
BUILD
echo " $CR_VARIANT Compiled"
CR_TARGET=3
BUILD
echo " $CR_VARIANT Compiled"
CR_TARGET=4
BUILD
echo " $CR_VARIANT Compiled"
CR_TARGET=5
BUILD
echo " $CR_VARIANT Compiled"
echo " "
echo " All Targets compiled"
}

# Main Menu
clear
echo "----------------------------------------------"
echo "$CR_NAME $CR_VERSION Build Script $CR_DATE"
echo " "
echo " "
echo "1) trelte" "2) trelteskt" "3) tbelteskt" "4) tre3calteskt" "5) trhpltexx" "6) All"  "7) Abort" 
echo "----------------------------------------------"
read -p "Please select your build target (1-6) > " CR_TARGET
read -p "Clean Builds? (y/n) > " CR_CLEAN
echo " "
if [ "$CR_TARGET" = "7" ]; then
echo "Build Aborted"
exit
fi
# Call functions
if [ "$CR_TARGET" = "6" ]; then
BUILD_ALL
else
BUILD
fi

