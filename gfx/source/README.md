# Graphics prebuilts

The Mali-T760 MP8 runs on the mainline panfrost DRM driver of the 6.6
kernel. Samsung's Mali blobs need the vendor kbase driver from 3.10, so the
ROM uses this stack instead:

| File | Source |
|---|---|
| `libEGL_mesa`, `libGLESv1_CM_mesa`, `libGLESv2_mesa`, `libglapi`, `libgallium_dri` | Mesa 24.3.4, panfrost only, unmodified |
| `hw/gralloc.minigbm.so` | minigbm `250c655ac972`, gralloc0, with `patches/` |
| `hw/hwcomposer.s6kms.so` | `hwc/hwc_kms.cpp`, an HWC 1.4 module on KMS |

libdrm 2.4.123 is linked statically into the gralloc and HWC modules.
`compat/` holds the HWC 1.x headers and the `native_handle_*` link stub
that the NDK lacks.

The in-tree `external/mesa3d` is Mesa 20.3, too old for Midgard on Android,
so these are built outside the ROM with the NDK and shipped as prebuilts.

## Rebuild

```sh
WORK=<scratch dir> NDK=<path to NDK 27.1.12297006> ./build.sh
```

The script fetches any missing source, builds arm64 and arm, strips the
results and replaces `../prebuilts`. Host tools: meson, ninja, pkg-config,
python3 with mako, bison, flex, git and curl.
