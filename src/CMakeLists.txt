# One static library per module. Dependencies point downwards only:
#
#   robotic -> sdk -> ipc -> common
#   npu     -> sdk -> ipc -> common
#   media   ->        ipc -> common
#
# Sources are listed explicitly (no globbing) so that adding a file is always
# a visible, reviewable change.

# --- D-Bus introspection XML embedded from dbus/*.xml ----------------------
set(MW_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MW_GENERATED_DIR}")
mw_embed_text(kMediaIntrospectionXml
  "${CMAKE_SOURCE_DIR}/dbus/org.vmx_mw.MediaManager1.xml"
  "${MW_GENERATED_DIR}/media_introspection.cc")
mw_embed_text(kNpuIntrospectionXml
  "${CMAKE_SOURCE_DIR}/dbus/org.vmx_mw.NpuManager1.xml"
  "${MW_GENERATED_DIR}/npu_introspection.cc")

# --- common: status, logging, fds, buffers, json, audio ring ---------------
add_library(mw_common STATIC
  common/audio_ring.cc
  common/buffer_allocator.cc
  common/dma_buf_mapping.cc
  common/frame_types.cc
  common/json.cc
  common/logging.cc
  common/scoped_fd.cc
  common/status.cc
  common/systemd_notify.cc
  common/termination_watcher.cc
  common/time_util.cc)
target_include_directories(mw_common PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(mw_common
  PUBLIC Threads::Threads PkgConfig::SYSTEMD
  PRIVATE mw_build_options)

# --- ipc: sd-bus wrapper and wire helpers ----------------------------------
add_library(mw_ipc STATIC
  ipc/bus.cc
  ipc/result_codec.cc
  "${MW_GENERATED_DIR}/media_introspection.cc"
  "${MW_GENERATED_DIR}/npu_introspection.cc")
target_link_libraries(mw_ipc
  PUBLIC mw_common
  PRIVATE mw_build_options)

# --- media: camera, microphone, frame pool, MediaManager service -----------
add_library(mw_media STATIC
  media/alsa_audio_source.cc
  media/audio_source.cc
  media/synthetic_v4l2_device.cc
  media/v4l2_device.cc
  media/v4l2_dmabuf_source.cc
  media/frame_ingest.cc
  media/frame_pool.cc
  media/frame_source.cc
  media/jpeg_decoder.cc
  media/media_config.cc
  media/media_service.cc
  media/sine_audio_source.cc
  media/test_pattern_source.cc
  media/v4l2_frame_source.cc
  media/video_hub.cc)
target_link_libraries(mw_media
  PUBLIC mw_ipc
  PRIVATE mw_build_options)
if(MW_ENABLE_JPEG)
  target_compile_definitions(mw_media PRIVATE MW_HAVE_JPEG=1)
  target_link_libraries(mw_media PUBLIC PkgConfig::JPEG)
endif()
if(MW_ENABLE_ALSA)
  target_compile_definitions(mw_media PRIVATE MW_HAVE_ALSA=1)
  target_link_libraries(mw_media PUBLIC PkgConfig::ALSA)
endif()

# --- sdk: client libraries for applications --------------------------------
add_library(mw_sdk STATIC
  sdk/media_client.cc
  sdk/npu_client.cc)
target_link_libraries(mw_sdk
  PUBLIC mw_ipc
  PRIVATE mw_build_options)

if(MW_HAVE_VIEWER_LIB)
  add_library(mw_viewer STATIC
    viewer/frame_convert.cc
    viewer/overlay.cc
    viewer/viewer_core.cc)
  target_link_libraries(mw_viewer
    PUBLIC mw_sdk PkgConfig::CAIRO PkgConfig::GDKPIXBUF
    PRIVATE mw_build_options)
endif()

# --- npu: runtime abstraction, models, scheduler, NpuManager service -------
add_library(mw_npu STATIC
  npu/cpu_runtime.cc
  npu/fake_npu_runtime.cc
  npu/image_scaler.cc
  npu/neuron_npu_runtime.cc
  npu/npu_config.cc
  npu/npu_runtime.cc
  npu/npu_service.cc
  npu/postprocess.cc
  npu/speaker_db.cc
  npu/tensor_buffer.cc
  npu/thermal_monitor.cc)
# ONNX Runtime is loaded with dlopen() at run time; only its C header is
# vendored (third_party/onnxruntime, MIT). SYSTEM silences its warnings.
target_include_directories(mw_npu SYSTEM PRIVATE
  "${CMAKE_SOURCE_DIR}/3rdparty/onnxruntime-x64/include")
target_link_libraries(mw_npu
  PUBLIC mw_sdk ${CMAKE_DL_LIBS}
  PRIVATE mw_build_options)

# --- robotic: optional relay to the Jetson Orin Nano -----------------------
if(MW_BUILD_ROBOTIC)
  add_library(mw_robotic STATIC
    robotic/jpeg_encoder.cc
    robotic/result_json.cc
    robotic/robotic_bridge.cc
    robotic/robotic_config.cc
    robotic/tcp_sender.cc)
  target_link_libraries(mw_robotic
    PUBLIC mw_sdk
    PRIVATE mw_build_options)
  if(MW_ENABLE_JPEG)
    target_compile_definitions(mw_robotic PRIVATE MW_HAVE_JPEG=1)
    target_link_libraries(mw_robotic PUBLIC PkgConfig::JPEG)
  endif()
endif()

install(TARGETS mw_common mw_ipc mw_media mw_sdk mw_npu
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(MW_BUILD_ROBOTIC)
  install(TARGETS mw_robotic ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
