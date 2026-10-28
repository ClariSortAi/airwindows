configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

add_executable(AirWindows WIN32
  main.cpp
  airplay_server.cpp
  video_pipeline.cpp
  audio_pipeline.cpp
  settings.cpp
  log.cpp
  app.rc
  app.manifest)

target_include_directories(AirWindows PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(AirWindows PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
target_compile_options(AirWindows PRIVATE /W4 /permissive- /utf-8 /wd5105)
target_link_libraries(AirWindows PRIVATE
  uxplay_core ffmpeg
  d3d11 dxgi dxguid d3dcompiler ole32 avrt user32 gdi32 shell32 comctl32 iphlpapi ws2_32)

set_target_properties(AirWindows PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
