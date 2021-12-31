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

# For Galaxy S2 the ramdisk.img is embedded uncompressed in zImage as ramdisk.cpio.
UNCOMPRESSED_RAMDISK := $(PRODUCT_OUT)/ramdisk.cpio
$(UNCOMPRESSED_RAMDISK): $(INSTALLED_RAMDISK_TARGET)
	$(hide) $(MKBOOTFS) -d $(TARGET_OUT) $(TARGET_RAMDISK_OUT) > $@

# For Galaxy S2 the boot.img is the zImage directly and pushed to /dev/block/mmcblk0p5
$(INSTALLED_BOOTIMAGE_TARGET): $(INSTALLED_KERNEL_TARGET) | $(UNCOMPRESSED_RAMDISK)
	$(ACP) -fp $< $@

# Default Recoveryimage build script
$(INSTALLED_RECOVERYIMAGE_TARGET): $(recoveryimage-deps)
	@echo ----- Making recovery image ------
	$(call build-recoveryimage-target, $@, $(recovery_kernel))
