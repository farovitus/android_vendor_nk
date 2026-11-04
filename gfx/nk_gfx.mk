# Mesa panfrost, minigbm gralloc and the KMS HWC. The device selects them
# with ro.hardware.egl=mesa, ro.hardware.gralloc=minigbm and
# ro.hardware.hwcomposer=s6kms.
PRODUCT_PACKAGES += \
    nk_libglapi \
    nk_libgallium_dri \
    nk_libEGL_mesa \
    nk_libGLESv1_CM_mesa \
    nk_libGLESv2_mesa \
    nk_gralloc.minigbm \
    nk_hwcomposer.s6kms
