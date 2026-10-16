# Executables. Each one is a thin main() over a library.

add_executable(mw-media-manager media_manager/main.cc)
target_link_libraries(mw-media-manager PRIVATE mw_media mw_build_options)

add_executable(mw-npu-manager npu_manager/main.cc)
target_link_libraries(mw-npu-manager PRIVATE mw_npu mw_build_options)

# Reference application and command line debug tool built on the client SDK.
add_executable(mw-example-client example_client/main.cc)
target_link_libraries(mw-example-client PRIVATE mw_sdk mw_build_options)

install(TARGETS mw-media-manager mw-npu-manager mw-example-client
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(MW_BUILD_ROBOTIC)
  add_executable(mw-robotic-bridge robotic_bridge/main.cc)
  target_link_libraries(mw-robotic-bridge PRIVATE mw_robotic mw_build_options)
  install(TARGETS mw-robotic-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(MW_HAVE_VIEWER_LIB AND MW_HAVE_GTK)
  add_executable(mw-viewer viewer/main.cc)
  target_include_directories(mw-viewer SYSTEM PRIVATE
                             "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(mw-viewer PRIVATE mw_viewer PkgConfig::GTK3
    mw_build_options)
  install(TARGETS mw-viewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
