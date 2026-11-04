# Mainline brcmfmac Wi-Fi and zram for the NK 6.6 kernel. The kernel
# ZIP, not the ROM, ships the modules; nk-modules.rc loads them.
PRODUCT_PACKAGES += \
    nk-modules.rc \
    nk_brcmfmac_firmware

# wireless-regdb 2026.09.03 (ISC, regdb/LICENSE). cfg80211 checks the
# .p7s signature against its built-in key.
PRODUCT_COPY_FILES += \
    vendor/nk/wifi/regdb/regulatory.db:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db \
    vendor/nk/wifi/regdb/regulatory.db.p7s:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db.p7s
