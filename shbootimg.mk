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

# ramdisk.cpio is produced by shkernel.mk before zImage is compiled.
# Do not define another recipe here, and do not depend on ramdisk.img.
UNCOMPRESSED_RAMDISK ?= $(PRODUCT_OUT)/ramdisk.cpio

# For Galaxy S2/Note, boot.img is the zImage itself and is flashed directly
# to the boot partition. The ramdisk is already embedded in zImage.
$(INSTALLED_BOOTIMAGE_TARGET): $(INSTALLED_KERNEL_TARGET)
	$(ACP) -fp $< $@

# Default recovery image build script.
$(INSTALLED_RECOVERYIMAGE_TARGET): $(recoveryimage-deps)
	@echo ----- Making recovery image ------
	$(call build-recoveryimage-target, $@, $(recovery_kernel))
