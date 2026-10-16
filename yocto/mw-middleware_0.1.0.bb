# Yocto recipe for the Genio middleware (Kirkstone and later: ":" overrides).
#
# Put it in your layer, e.g. meta-genio-custom/recipes-mw/mw-middleware/, and
# add   IMAGE_INSTALL:append = " mw-middleware"   to your image.
#
# The default "cpu" runtime dlopen()s libonnxruntime.so at run time: no build
# dependency, but the image must ship an ONNX Runtime package (a recipe for it
# is not part of this repository; set "onnx_library" in npu.json to its path).
# The NPU backend needs the NeuroPilot SDK of your BSP; once
# src/npu/neuron_npu_runtime.cc is implemented add its package to DEPENDS
# (see docs/PORTING.md).

SUMMARY = "Camera, audio and NPU middleware for MediaTek Genio 720"
DESCRIPTION = "MediaManager, NpuManager and optional RoboticBridge services \
sharing camera frames over zero-copy dma-bufs, controlled through D-Bus."
LICENSE = "CLOSED"

# Replace with your git repository, or ship the tree as a tarball.
SRC_URI = "file://genio-mw-${PV}.tar.gz"
S = "${WORKDIR}/genio-mw-${PV}"

DEPENDS = "systemd alsa-lib libjpeg-turbo"
RDEPENDS:${PN} = "dbus systemd"

inherit cmake pkgconfig systemd useradd

EXTRA_OECMAKE = " \
    -DMW_BUILD_TESTS=OFF \
    -DMW_SYSTEMD_UNIT_DIR=${systemd_system_unitdir} \
"

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "mw-media-manager.service mw-npu-manager.service"
SYSTEMD_AUTO_ENABLE = "enable"

# The service account and the client group (also created by sysusers.d on
# systems that run systemd-sysusers; both are idempotent).
USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "-r genio-clients"
USERADD_PARAM:${PN} = "-r -M -d /var/lib/genio-mw -s /sbin/nologin -G video,audio genio-mw"

FILES:${PN} += " \
    ${sysconfdir}/genio-mw \
    ${datadir}/dbus-1 \
    ${nonarch_libdir}/sysusers.d \
    ${nonarch_libdir}/udev/rules.d \
"
CONFFILES:${PN} += " \
    ${sysconfdir}/genio-mw/media.json \
    ${sysconfdir}/genio-mw/npu.json \
    ${sysconfdir}/genio-mw/robotic.json \
"
