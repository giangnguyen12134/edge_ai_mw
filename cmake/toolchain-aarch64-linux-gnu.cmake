# Cross-compilation toolchain for the MediaTek Genio 720 (AArch64, Cortex-A78
# / A55) from an Ubuntu x86-64 host.
#
#   sudo apt install g++-aarch64-linux-gnu pkg-config cmake
#   cmake -S . -B build-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux-gnu.cmake \
#         -DMW_SYSROOT=/path/to/genio-sysroot -DMW_BUILD_TESTS=OFF
#   cmake --build build-arm64 -j
#
# The sysroot must contain the target's libsystemd, libasound and libjpeg
# development files (headers + .so + pkg-config files). Get it from:
#   * the Yocto SDK:  <sdk>/sysroots/armv8a-poky-linux  (bitbake -c populate_sdk)
#   * Ubuntu:         a debootstrap/arm64 rootfs with libsystemd-dev,
#                     libasound2-dev and libjpeg-dev installed
#
# Variables (also read from the environment):
#   MW_SYSROOT        target root file system (recommended)
#   MW_CROSS_PREFIX   compiler prefix, default aarch64-linux-gnu-
#   MW_TARGET_FLAGS   CPU flags, default -march=armv8.2-a (valid for both the
#                     A78 and A55 clusters; add -mtune=cortex-a78 to taste)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT MW_CROSS_PREFIX)
  if(DEFINED ENV{MW_CROSS_PREFIX})
    set(MW_CROSS_PREFIX "$ENV{MW_CROSS_PREFIX}")
  else()
    set(MW_CROSS_PREFIX "aarch64-linux-gnu-")
  endif()
endif()
if(NOT MW_SYSROOT AND DEFINED ENV{MW_SYSROOT})
  set(MW_SYSROOT "$ENV{MW_SYSROOT}")
endif()
if(NOT MW_TARGET_FLAGS)
  if(DEFINED ENV{MW_TARGET_FLAGS})
    set(MW_TARGET_FLAGS "$ENV{MW_TARGET_FLAGS}")
  else()
    set(MW_TARGET_FLAGS "-march=armv8.2-a")
  endif()
endif()

set(CMAKE_C_COMPILER   ${MW_CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${MW_CROSS_PREFIX}g++)
set(CMAKE_C_FLAGS_INIT   "${MW_TARGET_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${MW_TARGET_FLAGS}")

if(MW_SYSROOT)
  set(CMAKE_SYSROOT "${MW_SYSROOT}")
  set(CMAKE_FIND_ROOT_PATH "${MW_SYSROOT}")
  # pkg-config must look ONLY in the target sysroot and prefix its paths.
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${MW_SYSROOT}")
  set(ENV{PKG_CONFIG_LIBDIR}
      "${MW_SYSROOT}/usr/lib/pkgconfig:${MW_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${MW_SYSROOT}/usr/share/pkgconfig")
  set(ENV{PKG_CONFIG_PATH} "")
endif()

# Programs (cmake, pkg-config, ...) come from the host; libraries, headers and
# packages only from the sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
