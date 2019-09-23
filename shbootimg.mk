#
# Copyright (C) 2012 The CyanogenMod Project
#
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
#

LOCAL_PATH := $(call my-dir)

prebuilts_folder := device/samsung/galaxys2-common/prebuilts

# Uncompressed boot ramdisk (make ramdisk)
uncompressed_ramdisk := $(PRODUCT_OUT)/../../../../$(prebuilts_folder)/ramdisk.cpio
$(uncompressed_ramdisk): $(INSTALLED_RAMDISK_TARGET)
	zcat $< > $@
	@echo "Made uncompressed ramdisk boot image: $@"

.PHONY: ramdisk-galaxys2-common
ramdisk-galaxys2-common: $(uncompressed_ramdisk)

# Uncompressed recovery ramdisk (make recoveryimage)
recovery_uncompressed_ramdisk_device := $(PRODUCT_OUT)/../../../../$(prebuilts_folder)/recovery-ramdisk-device.cpio
$(recovery_uncompressed_ramdisk_device): $(MKBOOTFS) $(ADBD) \
	    $(INTERNAL_ROOT_FILES) \
	    $(INSTALLED_RAMDISK_TARGET) \
            $(INSTALLED_BOOTIMAGE_TARGET) \
	    $(INTERNAL_RECOVERYIMAGE_FILES) \
	    $(recovery_initrc) $(recovery_sepolicy) \
	    $(INSTALLED_2NDBOOTLOADER_TARGET) \
	    $(INSTALLED_RECOVERY_BUILD_PROP_TARGET) \
	    $(recovery_resource_deps) \
	    $(recovery_fstab) \
	    $(RECOVERY_INSTALL_OTA_KEYS) \
	    $(BOARD_RECOVERY_KERNEL_MODULES) \
	    $(DEPMOD)
	$(call build-recoveryramdisk)
	@echo ----- Making uncompressed recovery ramdisk ------
	$(hide) $(MKBOOTFS) $(TARGET_RECOVERY_ROOT_OUT) > $@

.PHONY: recoveryimage-galaxys2-common
recoveryimage-galaxys2-common: $(recovery_uncompressed_ramdisk_device)

# Take zImage as boot.img
$(INSTALLED_BOOTIMAGE_TARGET): $(INSTALLED_KERNEL_TARGET)
	$(ACP) -fp $< $@

$(INSTALLED_RECOVERYIMAGE_TARGET): $(MKBOOTIMG) $(recovery_uncompressed_ramdisk) $(recovery_kernel)
	lzop -f9 -o $@ $<
