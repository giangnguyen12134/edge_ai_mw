# Cross-compilation toolchain for x86-64 Linux from a non-x86 host (for
# example an arm64 Ubuntu machine). On an x86-64 host you do not need this file:
# a normal native build (or -DMW_TARGET_ARCH=x64) is enough.
#
#   sudo apt install g++-x86-64-linux-gnu pkg-config cmake
#   cmake -S . -B build-x64 -DMW_TARGET_ARCH=x64 -DMW_SYSROOT=/path/to/sysroot
#
# Variables / environment (same meaning as the aarch64 file):
#   MW_SYSROOT        x86-64 root file system with libsystemd/libasound/libjpeg
#                     development files
#   MW_CROSS_PREFIX   default x86_64-linux-gnu-
#   MW_TARGET_FLAGS   default empty (baseline x86-64)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT MW_CROSS_PREFIX)
  if(DEFINED ENV{MW_CROSS_PREFIX})
    set(MW_CROSS_PREFIX "$ENV{MW_CROSS_PREFIX}")
  else()
    set(MW_CROSS_PREFIX "x86_64-linux-gnu-")
  endif()
endif()
if(NOT MW_SYSROOT AND DEFINED ENV{MW_SYSROOT})
  set(MW_SYSROOT "$ENV{MW_SYSROOT}")
endif()
if(NOT MW_TARGET_FLAGS AND DEFINED ENV{MW_TARGET_FLAGS})
  set(MW_TARGET_FLAGS "$ENV{MW_TARGET_FLAGS}")
endif()

set(CMAKE_C_COMPILER   ${MW_CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${MW_CROSS_PREFIX}g++)
set(CMAKE_C_FLAGS_INIT   "${MW_TARGET_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${MW_TARGET_FLAGS}")

if(MW_SYSROOT)
  set(CMAKE_SYSROOT "${MW_SYSROOT}")
  set(CMAKE_FIND_ROOT_PATH "${MW_SYSROOT}")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${MW_SYSROOT}")
  set(ENV{PKG_CONFIG_LIBDIR}
      "${MW_SYSROOT}/usr/lib/pkgconfig:${MW_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${MW_SYSROOT}/usr/share/pkgconfig")
  set(ENV{PKG_CONFIG_PATH} "")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
