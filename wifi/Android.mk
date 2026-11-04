LOCAL_PATH := $(call my-dir)

# brcmfmac looks for brcm/brcmfmac4358-pcie.{bin,txt}. Point those names
# at the BCM4358 STA firmware and NVRAM that the proprietary blobs
# already install in /vendor/etc/wifi.
include $(CLEAR_VARS)
LOCAL_MODULE := nk_brcmfmac_firmware
LOCAL_MODULE_CLASS := FAKE
LOCAL_MODULE_SUFFIX := -timestamp
include $(BUILD_SYSTEM)/base_rules.mk

$(LOCAL_BUILT_MODULE): BRCM_DIR := $(TARGET_OUT_VENDOR)/firmware/brcm
$(LOCAL_BUILT_MODULE): $(LOCAL_PATH)/Android.mk
	$(hide) mkdir -p $(BRCM_DIR) $(dir $@)
	$(hide) ln -sf /vendor/etc/wifi/bcmdhd_sta.bin $(BRCM_DIR)/brcmfmac4358-pcie.bin
	$(hide) ln -sf /vendor/etc/wifi/nvram_net.txt $(BRCM_DIR)/brcmfmac4358-pcie.txt
	$(hide) touch $@
