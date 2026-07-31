cmake_minimum_required(VERSION 3.24)
project(PodBox VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Without this an unqualified `cmake -S . -B build` produces an unoptimised
# binary, which is not what anyone typing that expects.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(APPLE)
  # The Info.plist has claimed macOS 11 since the bundle existed; this is what
  # makes the compiler agree, rather than silently emitting calls that are
  # unavailable there.
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

  # Off by default because it doubles build time and every dependency has to
  # be built twice too. Turn it on for a release: -DPODBOX_UNIVERSAL=ON
  option(PODBOX_UNIVERSAL "Build a universal arm64 + x86_64 binary" OFF)
  if(PODBOX_UNIVERSAL)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
  endif()
endif()

# Version as the code sees it, so the About text and a bug report can agree
# with the bundle.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/podbox_version.h @ONLY)

include(FetchContent)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_BINDINGS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(taglib
  GIT_REPOSITORY https://github.com/taglib/taglib.git
  GIT_TAG v1.13.1
  GIT_SHALLOW TRUE)

# stb has no releases, so it is pinned by commit: without a GIT_TAG the build
# tracks whatever master happens to be that day and stops being reproducible.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4)

FetchContent_MakeAvailable(glfw imgui taglib stb)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw)

# TagLib doesn't export in-tree include dirs for FetchContent use.
set(TAGLIB_INCLUDES
  ${taglib_SOURCE_DIR}/taglib
  ${taglib_SOURCE_DIR}/taglib/toolkit
  ${taglib_SOURCE_DIR}/taglib/mpeg
  ${taglib_SOURCE_DIR}/taglib/mpeg/id3v1
  ${taglib_SOURCE_DIR}/taglib/mpeg/id3v2
  ${taglib_SOURCE_DIR}/taglib/mpeg/id3v2/frames
  ${taglib_SOURCE_DIR}/taglib/mp4
  ${taglib_BINARY_DIR}
  ${taglib_BINARY_DIR}/taglib)

add_library(podbox_core STATIC
  src/device/device_watcher.cpp
  src/device/ipod_device.cpp
  src/itdb/itunesdb.cpp
  src/itdb/itunesdb_writer.cpp
  src/itdb/playcounts.cpp
  src/library/artwork.cpp
  src/library/dedupe.cpp
  src/library/fingerprint.cpp
  src/library/fingerprint_store.cpp
  src/library/applemusic.cpp
  src/library/host_library.cpp
  src/library/metadata.cpp
  src/library/transcode.cpp
  src/sync/sync_engine.cpp
  src/sync/sync_plan.cpp
  src/sync/verify_job.cpp)
target_include_directories(podbox_core PUBLIC src
  ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Our code only: the fetched dependencies are not ours to clean up, and
# warnings we cannot act on are warnings everyone learns to ignore.
if(NOT MSVC)
  target_compile_options(podbox_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_include_directories(podbox_core PRIVATE ${TAGLIB_INCLUDES}
  ${stb_SOURCE_DIR})
target_link_libraries(podbox_core PUBLIC tag)

if(APPLE)
  set(PODBOX_AUDIO_SRC src/audio/player_avf.mm)
  set_source_files_properties(src/audio/player_avf.mm PROPERTIES
    COMPILE_FLAGS "-fobjc-arc")
else()
  set(PODBOX_AUDIO_SRC src/audio/player_null.cpp)
endif()

# On macOS this builds PodBox.app rather than a bare binary: a bundle is what
# lets it carry an icon, its Info.plist usage strings (without which macOS
# refuses to let it ask Music.app for anything), and be dragged to
# /Applications like any other app.
set(PODBOX_ICON ${CMAKE_CURRENT_SOURCE_DIR}/packaging/PodBox.icns)

add_executable(podbox MACOSX_BUNDLE
  src/main.cpp
  src/app/app.cpp
  src/app/app_chrome.cpp
  src/app/app_modals.cpp
  src/app/app_util.cpp
  src/ui/aqua.cpp
  src/ui/theme.cpp
  ${PODBOX_AUDIO_SRC}
  $<$<BOOL:${APPLE}>:${PODBOX_ICON}>)
target_link_libraries(podbox PRIVATE podbox_core imgui_lib)
if(NOT MSVC)
  target_compile_options(podbox PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
  set_target_properties(podbox PROPERTIES
    OUTPUT_NAME "PodBox"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER com.milanmarocchi.podbox
    MACOSX_BUNDLE_BUNDLE_NAME PodBox
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_COPYRIGHT "© 2026 Milan Marocchi. MIT licensed.")
  # Icons belong in Contents/Resources.
  set_source_files_properties(${PODBOX_ICON} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources)
endif()

if(APPLE)
  target_link_libraries(podbox PRIVATE
    "-framework AVFoundation" "-framework Foundation")
endif()

add_executable(itdb_dump tools/itdb_dump.cpp)
target_link_libraries(itdb_dump PRIVATE podbox_core)

add_executable(podbox_add tools/podbox_add.cpp)
target_link_libraries(podbox_add PRIVATE podbox_core)

add_executable(playcounts_test tools/playcounts_test.cpp)
target_link_libraries(playcounts_test PRIVATE podbox_core)

add_executable(dedupe_test tools/dedupe_test.cpp)
target_link_libraries(dedupe_test PRIVATE podbox_core)

add_executable(library_test tools/library_test.cpp)
target_link_libraries(library_test PRIVATE podbox_core)

add_executable(applemusic_test tools/applemusic_test.cpp)
target_link_libraries(applemusic_test PRIVATE podbox_core)

add_executable(sync_test tools/sync_test.cpp)
target_link_libraries(sync_test PRIVATE podbox_core)

if(APPLE)
  add_executable(audio_test tools/audio_test.cpp ${PODBOX_AUDIO_SRC})
  target_include_directories(audio_test PRIVATE src)
  target_link_libraries(audio_test PRIVATE
    "-framework AVFoundation" "-framework Foundation")
endif()

if(APPLE)
  target_link_libraries(podbox PRIVATE "-framework OpenGL")
elseif(UNIX)
  find_package(OpenGL REQUIRED)
  target_link_libraries(podbox PRIVATE OpenGL::GL)
endif()

# Installation. A .app goes to Applications/ so `nix profile install` and
# `cmake --install` both drop something Finder can launch; elsewhere it is an
# ordinary binary. The command-line tools are developer aids, so they are only
# installed when asked for.
include(GNUInstallDirs)
option(PODBOX_INSTALL_TOOLS "Install the command-line test tools" OFF)

# Where the bundle lands inside the install prefix. The DMG wants it at the
# root, beside the /Applications symlink CPack adds for the drag; package
# managers want it under Applications/. Defaults to the DMG layout because
# that is what a release is; Nix passes Applications explicitly.
set(PODBOX_BUNDLE_DEST "." CACHE STRING
    "Install destination for PodBox.app inside the prefix")

if(APPLE)
  install(TARGETS podbox BUNDLE DESTINATION ${PODBOX_BUNDLE_DEST})
else()
  install(TARGETS podbox RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(PODBOX_INSTALL_TOOLS)
  install(TARGETS itdb_dump podbox_add playcounts_test dedupe_test
                  library_test applemusic_test sync_test
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# `dedupe_test` is the only tool that asserts rather than just printing, so it
# is the one worth wiring into CTest. Run with: ctest --test-dir build
enable_testing()
add_test(NAME dedupe COMMAND dedupe_test)

# A DMG for releases: `cpack -G DragNDrop --config build/CPackConfig.cmake`,
# or `cmake --build build --target package`. Unsigned, like the app itself,
# so it still needs a right-click Open on the other side.
set(CPACK_PACKAGE_NAME "PodBox")
set(CPACK_PACKAGE_VENDOR "Milan Marocchi")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "PodBox-${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_DMG_VOLUME_NAME "PodBox ${PROJECT_VERSION}")
endif()
include(CPack)
