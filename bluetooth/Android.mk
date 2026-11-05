LOCAL_PATH := $(call my-dir)

# btbcm names the controller BCM4358A3 (LMP subversion 0x240f) and requests
# brcm/BCM4358A3.hcd. Link that to the stock patch the proprietary blobs
# install (its murata/semco/semco3rd/wisol copies are byte-identical). The
# link is made in the init script's post-install command, which runs as
# part of an installed file's rule, so it exists before the image is packed.
include $(CLEAR_VARS)
LOCAL_MODULE := nk-bluetooth.rc
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR_ETC)/init
LOCAL_SRC_FILES := nk-bluetooth.rc
LOCAL_POST_INSTALL_CMD := \
    mkdir -p $(TARGET_OUT_VENDOR)/firmware/brcm && \
    ln -sf /vendor/firmware/bcm4358A3_V0083.0138.hcd \
        $(TARGET_OUT_VENDOR)/firmware/brcm/BCM4358A3.hcd
include $(BUILD_PREBUILT)
