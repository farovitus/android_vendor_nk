# Mainline brcmfmac Wi-Fi, zram and audio for the NK 6.6 kernel. The
# device tree's prebuilt/modules ships the modules; nk-modules.rc loads
# them.
# nk_regulatory.db installs wireless-regdb 2026.09.03 (ISC,
# regdb/LICENSE) and the brcmfmac firmware links.
PRODUCT_PACKAGES += \
    nk-modules.rc \
    nk_regulatory.db
