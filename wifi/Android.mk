LOCAL_PATH := $(call my-dir)

# wireless-regdb for cfg80211, installed as a module so the brcmfmac
# firmware links can hang off its install step: brcmfmac looks for
# brcm/brcmfmac4358-pcie.{bin,txt}, and those names point at the BCM4358
# STA firmware and NVRAM that the proprietary blobs install in
# /vendor/etc/wifi. A post-install command runs as part of the installed
# file's own rule, so the links exist before the image is packed.
include $(CLEAR_VARS)
LOCAL_MODULE := nk_regulatory.db
LOCAL_MODULE_STEM := regulatory.db
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/firmware
LOCAL_SRC_FILES := regdb/regulatory.db
LOCAL_REQUIRED_MODULES := nk_regulatory.db.p7s
LOCAL_POST_INSTALL_CMD := \
    mkdir -p $(TARGET_OUT_VENDOR)/firmware/brcm && \
    ln -sf /vendor/etc/wifi/bcmdhd_sta.bin \
        $(TARGET_OUT_VENDOR)/firmware/brcm/brcmfmac4358-pcie.bin && \
    ln -sf /vendor/etc/wifi/nvram_net.txt \
        $(TARGET_OUT_VENDOR)/firmware/brcm/brcmfmac4358-pcie.txt
include $(BUILD_PREBUILT)

# cfg80211 checks the .p7s signature against its built-in key.
include $(CLEAR_VARS)
LOCAL_MODULE := nk_regulatory.db.p7s
LOCAL_MODULE_STEM := regulatory.db.p7s
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/firmware
LOCAL_SRC_FILES := regdb/regulatory.db.p7s
include $(BUILD_PREBUILT)
