# BCM4358 Bluetooth on the NK 6.6 kernel: hci_bcm (serdev) plus the btlinux
# HAL. nk-bluetooth.rc loads hci_uart and installs the firmware link.
PRODUCT_PACKAGES += \
    nk-bluetooth.rc
