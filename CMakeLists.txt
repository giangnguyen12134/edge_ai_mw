cmake_minimum_required(VERSION 3.8)

# ---------------------------------------------------------------------------
# Target architecture: native (default), x64 or arm64.
#   cmake -S . -B build-x64   -DMW_TARGET_ARCH=x64
#   cmake -S . -B build-arm64 -DMW_TARGET_ARCH=arm64 -DMW_SYSROOT=/path/sysroot
# This must run before project() so the matching cross toolchain file can be
# selected. Passing your own -DCMAKE_TOOLCHAIN_FILE always wins.
# ---------------------------------------------------------------------------
set(MW_TARGET_ARCH "native" CACHE STRING
    "Target architecture: native, x64 (x86-64 Ubuntu) or arm64 (Genio 720)")
set_property(CACHE MW_TARGET_ARCH PROPERTY STRINGS native x64 arm64)
if(NOT MW_TARGET_ARCH MATCHES "^(native|x64|arm64)$")
  message(FATAL_ERROR "MW_TARGET_ARCH must be native, x64 or arm64")
endif()
execute_process(COMMAND uname -m OUTPUT_VARIABLE MW_HOST_MACHINE
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT CMAKE_TOOLCHAIN_FILE)
  if(MW_TARGET_ARCH STREQUAL "arm64" AND NOT MW_HOST_MACHINE MATCHES "aarch64|arm64")
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain-aarch64-linux-gnu.cmake")
  elseif(MW_TARGET_ARCH STREQUAL "x64" AND NOT MW_HOST_MACHINE MATCHES "x86_64|amd64")
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain-x86_64-linux-gnu.cmake")
  endif()
endif()

project(genio_middleware VERSION 0.5.0 LANGUAGES CXX)
message(STATUS "Target: ${MW_TARGET_ARCH} (${CMAKE_SYSTEM_PROCESSOR}), "
               "host: ${MW_HOST_MACHINE}, "
               "cross compiling: ${CMAKE_CROSSCOMPILING}")

# ---------------------------------------------------------------------------
# Language and build type
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MW_ENABLE_JPEG "Build the libjpeg MJPEG decoder and JPEG encoder" ON)
option(MW_ENABLE_ALSA "Build the ALSA microphone backend" ON)
option(MW_BUILD_ROBOTIC "Build the optional RoboticBridge (Jetson link)" ON)
option(MW_BUILD_VIEWER "Build the GTK3 camera viewer (mw-viewer)" ON)
option(MW_BUILD_TESTS "Build the unit tests" ON)
option(MW_WERROR "Treat compiler warnings as errors" OFF)
set(MW_SANITIZE "" CACHE STRING
    "Sanitizers for development builds, e.g. address,undefined or thread")
set(MW_SYSTEMD_UNIT_DIR "lib/systemd/system" CACHE STRING
    "Where systemd units are installed (relative to the install prefix)")
set(MW_DBUS_POLICY_DIR "share/dbus-1/system.d" CACHE STRING
    "Where the D-Bus policy is installed (relative to the install prefix)")

# ---------------------------------------------------------------------------
# Dependencies (found through pkg-config so cross builds use the sysroot)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

if(MW_ENABLE_JPEG)
  pkg_check_modules(JPEG IMPORTED_TARGET libjpeg)
  if(NOT JPEG_FOUND)
    message(FATAL_ERROR
      "libjpeg not found. Install libjpeg-turbo development files or "
      "configure with -DMW_ENABLE_JPEG=OFF (then MJPEG cameras and the "
      "Jetson video link are unavailable).")
  endif()
endif()

if(MW_ENABLE_ALSA)
  pkg_check_modules(ALSA IMPORTED_TARGET alsa)
  if(NOT ALSA_FOUND)
    message(FATAL_ERROR
      "ALSA not found. Install libasound2 development files or configure "
      "with -DMW_ENABLE_ALSA=OFF (then only the 'sine' audio source works).")
  endif()
endif()

# The viewer is optional: without cairo + gdk-pixbuf (display code and MJPG
# decoding) it is skipped, without GTK3 only its library and tests are built.
set(MW_HAVE_VIEWER_LIB OFF)
set(MW_HAVE_GTK OFF)
if(MW_BUILD_VIEWER)
  pkg_check_modules(CAIRO IMPORTED_TARGET cairo)
  pkg_check_modules(GDKPIXBUF IMPORTED_TARGET gdk-pixbuf-2.0)
  pkg_check_modules(GTK3 IMPORTED_TARGET gtk+-3.0)
  if(CAIRO_FOUND AND GDKPIXBUF_FOUND)
    set(MW_HAVE_VIEWER_LIB ON)
    if(GTK3_FOUND)
      set(MW_HAVE_GTK ON)
    else()
      message(STATUS "GTK3 not found: mw-viewer window not built")
    endif()
  else()
    message(STATUS "cairo/gdk-pixbuf not found: viewer not built")
  endif()
endif()

# ---------------------------------------------------------------------------
# Warnings and sanitizers shared by all targets
# ---------------------------------------------------------------------------
add_library(mw_build_options INTERFACE)
target_compile_options(mw_build_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
  -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2)
if(MW_WERROR)
  target_compile_options(mw_build_options INTERFACE -Werror)
endif()
if(MW_SANITIZE)
  target_compile_options(mw_build_options INTERFACE
    -fsanitize=${MW_SANITIZE} -fno-omit-frame-pointer)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${MW_SANITIZE}")
endif()

include(GNUInstallDirs)
include(EmbedText)

add_subdirectory(src)
add_subdirectory(apps)

if(MW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation: binaries, configuration, D-Bus policy, systemd, udev, sysusers
# ---------------------------------------------------------------------------
install(FILES
  config/media.json config/npu.json config/robotic.json config/logging.json
  DESTINATION /etc/genio-mw)
install(FILES config/logging.production-udp.example.json
  DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES packaging/genio-mw.logrotate
  DESTINATION /etc/logrotate.d RENAME genio-mw)
install(FILES dbus/org.genio.MiddleWare.conf
  DESTINATION ${MW_DBUS_POLICY_DIR})
install(FILES
  systemd/mw-media-manager.service
  systemd/mw-npu-manager.service
  systemd/mw-robotic-bridge.service
  DESTINATION ${MW_SYSTEMD_UNIT_DIR})
install(FILES packaging/genio-mw.sysusers.conf
  DESTINATION lib/sysusers.d RENAME genio-mw.conf)
install(FILES packaging/70-genio-mw.rules
  DESTINATION lib/udev/rules.d)
install(FILES dbus/org.vmx_mw.MediaManager1.xml dbus/org.vmx_mw.NpuManager1.xml
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)
# Public headers of the client SDK and what it needs.
install(DIRECTORY include/mw/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mw
  FILES_MATCHING PATTERN "*.h")

# ---------------------------------------------------------------------------
# Packaging (Ubuntu .deb). On Yocto use yocto/mw-middleware.bb instead.
# ---------------------------------------------------------------------------
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "genio-mw")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "maintainers@example.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Camera, audio and AI inference middleware (Genio 720 / Ubuntu x64)")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libsystemd0, libasound2t64 | libasound2, libjpeg-turbo8 | libjpeg62-turbo, dbus")
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/packaging/postinst;${CMAKE_SOURCE_DIR}/packaging/conffiles")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
