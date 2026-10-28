cmake_minimum_required(VERSION 3.20)

# Static C runtime, matching the x64-windows-static vcpkg triplet, so the
# executable runs without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The one place the version is set. src/version.h.in carries it into the
# executable's properties, the About box and the log.
project(AirWindows VERSION 0.2.0 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "This project builds only for Windows with MSVC.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# The program parses data from the network, so indirect calls are checked
# (Control Flow Guard) in the code compiled here.
add_compile_options(/guard:cf)
add_link_options(/guard:cf)

set(UXPLAY_DIR "${CMAKE_SOURCE_DIR}/third_party/UxPlay" CACHE PATH "UxPlay source checkout")

if(NOT EXISTS "${UXPLAY_DIR}/lib/raop.h")
  message(FATAL_ERROR "UxPlay sources not found at ${UXPLAY_DIR}. Run scripts/bootstrap.ps1 first.")
endif()

# OpenSSL, libplist and FFmpeg come from vcpkg (see vcpkg.json).
find_package(OpenSSL REQUIRED)
find_package(unofficial-libplist CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

# ---------------------------------------------------------------------------
# AirPlay protocol engine: UxPlay's lib/, compiled from unmodified upstream
# sources except for the Windows mDNS replacement below. compat/msvc supplies
# the few POSIX pieces MSVC lacks, and msvc_prelude.h is force-included.
# ---------------------------------------------------------------------------
file(GLOB UXPLAY_CORE_SOURCES CONFIGURE_DEPENDS
  "${UXPLAY_DIR}/lib/*.c"
  "${UXPLAY_DIR}/lib/mdnsd/*.c"
  "${UXPLAY_DIR}/lib/llhttp/*.c"
  "${UXPLAY_DIR}/lib/playfair/*.c")

# AirWindows carries a Windows-specific mDNS responder for multi-interface discovery.
list(FILTER UXPLAY_CORE_SOURCES EXCLUDE REGEX "/lib/mdnsd/mdnsd\\.c$")
list(APPEND UXPLAY_CORE_SOURCES "${CMAKE_SOURCE_DIR}/src/mdnsd_win.c")

add_library(uxplay_core STATIC ${UXPLAY_CORE_SOURCES})
target_include_directories(uxplay_core
  PUBLIC "${UXPLAY_DIR}/lib"
  PRIVATE "${CMAKE_SOURCE_DIR}/compat/msvc"
          "${UXPLAY_DIR}/lib/llhttp"
          "${UXPLAY_DIR}/lib/playfair"
          "${UXPLAY_DIR}/lib/mdnsd")
target_compile_definitions(uxplay_core
  PUBLIC OPENSSL_API_COMPAT=0x10101000L
  PRIVATE WIN32 NOHOLD PLIST_210 PLIST_230
          _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _WINSOCK_DEPRECATED_NO_WARNINGS)
# C4013 (call to an undeclared function) is an error: on x64 the implied "int"
# return type silently truncates pointers.
target_compile_options(uxplay_core PRIVATE
  /FImsvc_prelude.h /W3 /we4013 /wd4244 /wd4267 /wd4018 /wd4996 /wd5105 /wd4101 /wd4102)
target_link_libraries(uxplay_core
  PUBLIC OpenSSL::Crypto unofficial::libplist::libplist ws2_32 iphlpapi)

# ---------------------------------------------------------------------------
# FFmpeg: static libraries built by the overlay port in vcpkg-ports/ffmpeg,
# which enables only the decoders this program opens.
# ---------------------------------------------------------------------------
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(ffmpeg INTERFACE ${FFMPEG_LIBRARIES})

add_subdirectory(src)
