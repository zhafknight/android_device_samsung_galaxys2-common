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

# The Galaxy S2/Note kernel embeds an uncompressed ramdisk.cpio directly
# inside zImage. Build the raw CPIO from TARGET_RAMDISK_OUT instead of
# depending on ramdisk.img, which itself depends on the finished kernel.
UNCOMPRESSED_RAMDISK := $(PRODUCT_OUT)/ramdisk.cpio

$(UNCOMPRESSED_RAMDISK): PRIVATE_DIRS := debug_ramdisk dev metadata mnt proc second_stage_resources sys
$(UNCOMPRESSED_RAMDISK): $(MKBOOTFS) $(RAMDISK_NODE_LIST) $(INTERNAL_RAMDISK_FILES) $(INSTALLED_FILES_FILE_RAMDISK)
	$(call pretty,"Target uncompressed ramdisk: $@")
	$(hide) mkdir -p $(dir $@)
	$(hide) mkdir -p $(addprefix $(TARGET_RAMDISK_OUT)/,$(PRIVATE_DIRS))
	$(hide) $(MKBOOTFS) -n $(RAMDISK_NODE_LIST) -d $(TARGET_OUT) $(TARGET_RAMDISK_OUT) > $@

# BOARD_CUSTOM_KERNEL_MK replaces the default KERNEL_OUT rule. Ensure the
# raw ramdisk exists before kernel configuration and compilation begin.
$(KERNEL_OUT): $(UNCOMPRESSED_RAMDISK)
	$(hide) mkdir -p $(KERNEL_OUT)
