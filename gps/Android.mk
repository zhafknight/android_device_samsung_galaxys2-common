LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := gps.conf
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH  := $(TARGET_OUT_VENDOR)/etc/
LOCAL_SRC_FILES := gps_debug.conf

include $(BUILD_PREBUILT)

include $(CLEAR_VARS)

LOCAL_MODULE := sirfgps.conf
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH  := $(TARGET_OUT_VENDOR)/etc/
LOCAL_SRC_FILES := sirfgps.conf

include $(BUILD_PREBUILT)
