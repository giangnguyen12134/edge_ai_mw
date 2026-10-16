# Unit tests (one executable) and the end-to-end script.

add_executable(mw_tests
  test_main.cc
  unit/common_test.cc
  unit/media_test.cc
  unit/npu_test.cc
  unit/cpu_runtime_test.cc
  unit/dmabuf_capture_test.cc
  unit/logging_test.cc)
target_compile_definitions(mw_tests PRIVATE
  MW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(mw_tests PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(mw_tests PRIVATE mw_media mw_npu mw_build_options)

if(MW_BUILD_ROBOTIC)
  target_sources(mw_tests PRIVATE unit/robotic_test.cc)
  target_link_libraries(mw_tests PRIVATE mw_robotic)
  if(MW_ENABLE_JPEG)
    target_compile_definitions(mw_tests PRIVATE MW_HAVE_JPEG=1)
  endif()
endif()

# Optional: path of libonnxruntime.so for the CPU-runtime tests (skipped when
# the library cannot be loaded).
set(MW_ONNX_LIBRARY "" CACHE FILEPATH "libonnxruntime.so used by the tests")

add_test(NAME unit COMMAND mw_tests)
if(MW_ONNX_LIBRARY)
  set_tests_properties(unit PROPERTIES
    ENVIRONMENT "MW_ONNX_LIBRARY=${MW_ONNX_LIBRARY}")
endif()

# Cross builds cannot run target binaries on the build host.
if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME e2e
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_e2e.sh"
            "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")
  set_tests_properties(e2e PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
  if(MW_ONNX_LIBRARY)
    set_property(TEST e2e APPEND PROPERTY
      ENVIRONMENT "MW_ONNX_LIBRARY=${MW_ONNX_LIBRARY}")
  endif()
endif()
