configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

set(AIRWINDOWS_SOURCES
  main.cpp
  airplay_server.cpp
  video_pipeline.cpp
  frame_server.cpp
  audio_pipeline.cpp
  settings.cpp
  log.cpp
  native_control.cpp
  ../../native-ble-prototype/native_hid_backend.cpp
  app.rc
  app.manifest)

# SDK19041 C++/WinRT async headers rely on legacy template lookup under VS2019.
# Match the physically qualified bench compiler mode for this source only.
set_source_files_properties(../../native-ble-prototype/native_hid_backend.cpp PROPERTIES COMPILE_OPTIONS /permissive)

foreach(APP_TARGET AirWindows AirWindowsNative)
add_executable(${APP_TARGET} WIN32 ${AIRWINDOWS_SOURCES})

target_include_directories(${APP_TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/../../native-ble-prototype")
target_compile_definitions(${APP_TARGET} PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(${APP_TARGET} PRIVATE /W4 /permissive- /utf-8 /wd5105)
target_link_libraries(${APP_TARGET} PRIVATE
  uxplay_core ffmpeg
  d3d11 dxgi dxguid d3dcompiler ole32 avrt user32 gdi32 shell32 comctl32 iphlpapi ws2_32 advapi32 windowsapp)

set_target_properties(${APP_TARGET} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endforeach()

add_executable(ControlPolicyCheck EXCLUDE_FROM_ALL ../tests/control_policy_check.cpp)
target_include_directories(ControlPolicyCheck PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../native-ble-prototype")
set_target_properties(ControlPolicyCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(FrameServerCheck EXCLUDE_FROM_ALL ../tests/frame_server_check.cpp frame_server.cpp log.cpp)
target_include_directories(FrameServerCheck PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(FrameServerCheck PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(FrameServerCheck PRIVATE ffmpeg advapi32)
set_target_properties(FrameServerCheck PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
