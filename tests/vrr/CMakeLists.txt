cmake_minimum_required(VERSION 3.21)
project(MoonlightVrrTools LANGUAGES C CXX)

# Standalone developer tools: compile the production controller and existing
# tests directly, without configuring the full Moonlight application.
include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MACOSX_BUNDLE OFF)

get_filename_component(MOONLIGHT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(RENDERERS "${MOONLIGHT_ROOT}/app/streaming/video/ffmpeg-renderers")
set(VRR "${RENDERERS}/pacer/vrr")
option(VRR_BUILD_OVERLAY_TEST "Build the overlay regression (requires SDL2_ttf)" ON)
option(VRR_BUILD_PLATFORM_TESTS "Build available Vulkan/Wayland regressions on Linux" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Test Qml)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_AVUTIL QUIET libavutil)
    pkg_check_modules(PC_SDL2 QUIET sdl2)
    pkg_check_modules(PC_SDL2_TTF QUIET SDL2_ttf)
endif()

# Normal SDK installations are found through CMAKE_PREFIX_PATH/pkg-config.
# The repository's optional prebuilt dependencies are another search hint.
# Every include/library cache entry can also name an existing SDK explicitly.
if(WIN32)
    set(VRR_TARGET_PROCESSOR "${CMAKE_GENERATOR_PLATFORM}")
    if(NOT VRR_TARGET_PROCESSOR)
        set(VRR_TARGET_PROCESSOR "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
    endif()
    if(NOT VRR_TARGET_PROCESSOR)
        set(VRR_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(VRR_TARGET_PROCESSOR MATCHES "^([Aa][Rr][Mm]64|aarch64)$")
        set(VRR_DEPENDENCY_ARCH arm64)
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(VRR_DEPENDENCY_ARCH x64)
    else()
        message(FATAL_ERROR "The bundled Windows dependencies support x64 and ARM64 only")
    endif()
    set(VRR_INCLUDE_HINT "${MOONLIGHT_ROOT}/libs/windows/include/${VRR_DEPENDENCY_ARCH}")
    set(VRR_LIBRARY_HINT "${MOONLIGHT_ROOT}/libs/windows/lib/${VRR_DEPENDENCY_ARCH}")
elseif(APPLE)
    set(VRR_INCLUDE_HINT "${MOONLIGHT_ROOT}/libs/mac/include")
    set(VRR_LIBRARY_HINT "${MOONLIGHT_ROOT}/libs/mac/lib")
endif()

find_path(VRR_AVUTIL_INCLUDE_DIR libavutil/frame.h
    HINTS ${PC_AVUTIL_INCLUDE_DIRS} "${VRR_INCLUDE_HINT}")
find_library(VRR_AVUTIL_LIBRARY NAMES avutil avutil.60 avutil.59
    HINTS ${PC_AVUTIL_LIBRARY_DIRS} "${VRR_LIBRARY_HINT}")
find_path(VRR_SDL2_INCLUDE_DIR SDL.h
    HINTS ${PC_SDL2_INCLUDE_DIRS} "${VRR_INCLUDE_HINT}"
    PATH_SUFFIXES SDL2 SDL2.framework/Headers)
find_library(VRR_SDL2_LIBRARY NAMES SDL2
    HINTS ${PC_SDL2_LIBRARY_DIRS} "${VRR_LIBRARY_HINT}")
find_path(VRR_COMMON_INCLUDE_DIR Limelight.h
    HINTS "${MOONLIGHT_ROOT}/moonlight-common-c/moonlight-common-c/src")
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(VrrDependencies REQUIRED_VARS
    VRR_AVUTIL_INCLUDE_DIR VRR_AVUTIL_LIBRARY VRR_SDL2_INCLUDE_DIR
    VRR_SDL2_LIBRARY VRR_COMMON_INCLUDE_DIR)
if(NOT VrrDependencies_FOUND)
    message(FATAL_ERROR "Missing real VRR dependencies; see BUILDING-cmake.md for SDK overrides")
endif()

add_library(vrr_avutil INTERFACE)
target_include_directories(vrr_avutil SYSTEM INTERFACE "${VRR_AVUTIL_INCLUDE_DIR}")
target_link_libraries(vrr_avutil INTERFACE "${VRR_AVUTIL_LIBRARY}")
add_library(vrr_sdl2 INTERFACE)
target_include_directories(vrr_sdl2 SYSTEM INTERFACE "${VRR_SDL2_INCLUDE_DIR}")
target_link_libraries(vrr_sdl2 INTERFACE "${VRR_SDL2_LIBRARY}")
target_compile_definitions(vrr_sdl2 INTERFACE SDL_MAIN_HANDLED)

add_library(vrr_controller STATIC "${VRR}/vrrtimingcontroller.cpp")
target_link_libraries(vrr_controller PUBLIC vrr_avutil)
add_library(vrr_waiter STATIC "${VRR}/vrrtargetwaiter.cpp")
target_link_libraries(vrr_waiter PUBLIC vrr_sdl2 Threads::Threads)
add_library(vrr_replay_config STATIC vrrreplayconfig.cpp vrrreplaymodel.cpp)
target_link_libraries(vrr_replay_config PUBLIC Qt6::Core vrr_avutil)

function(vrr_test name)
    cmake_parse_arguments(TEST "" "" "ARGS" ${ARGN})
    add_executable(${name} ${TEST_UNPARSED_ARGUMENTS})
    # Some existing tests use assert() for setup as well as verification.
    if(MSVC)
        target_compile_options(${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -UNDEBUG)
    endif()
    if(BUILD_TESTING)
        add_test(NAME ${name} COMMAND ${name} ${TEST_ARGS})
    endif()
endfunction()

vrr_test(tst_dxgipresent tst_dxgipresent.cpp)
vrr_test(tst_vrrpresentationfeedback tst_vrrpresentationfeedback.cpp)
vrr_test(tst_incomingframetiming tst_incomingframetiming.cpp)
vrr_test(tst_vrrtimingcontroller tst_vrrtimingcontroller.cpp)
target_link_libraries(tst_vrrtimingcontroller PRIVATE vrr_controller vrr_waiter)

vrr_test(tst_vrrratepolicy tst_vrrratepolicy.cpp
    "${MOONLIGHT_ROOT}/app/streaming/vrrratepolicy.cpp")
target_link_libraries(tst_vrrratepolicy PRIVATE Qt6::Core Qt6::Test)
set_target_properties(tst_vrrratepolicy PROPERTIES AUTOMOC ON)
vrr_test(tst_vrrreplayconfig tst_vrrreplayconfig.cpp)
target_link_libraries(tst_vrrreplayconfig PRIVATE vrr_replay_config Qt6::Test)
set_target_properties(tst_vrrreplayconfig PROPERTIES AUTOMOC ON)

vrr_test(tst_vrrpacingworker tst_vrrpacingworker.cpp
    "${RENDERERS}/pacer/vrrpacingworker.cpp" "${VRR}/profile.cpp")
target_include_directories(tst_vrrpacingworker PRIVATE
    "${MOONLIGHT_ROOT}/app" "${VRR_COMMON_INCLUDE_DIR}")
target_link_libraries(tst_vrrpacingworker PRIVATE
    vrr_controller vrr_waiter Qt6::Core Qt6::Qml)
vrr_test(tst_vrrprofile tst_vrrprofile.cpp "${VRR}/profile.cpp")
target_link_libraries(tst_vrrprofile PRIVATE Qt6::Core Threads::Threads)

add_executable(vrrreplay vrrreplay.cpp)
target_link_libraries(vrrreplay PRIVATE vrr_replay_config vrr_controller Threads::Threads)
# The replay-config suite also exercises the sibling replay CLI.
add_dependencies(tst_vrrreplayconfig vrrreplay)
add_executable(vrrqueuesim vrrqueuesim.cpp)
target_link_libraries(vrrqueuesim PRIVATE vrr_replay_config vrr_controller)
if(BUILD_TESTING)
    add_test(NAME vrrreplay_help COMMAND vrrreplay --help)
    add_test(NAME vrrqueuesim_help COMMAND vrrqueuesim --help)
    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME playback_review_runner COMMAND "${Python3_EXECUTABLE}"
            -B "${CMAKE_CURRENT_LIST_DIR}/test_playback_review.py")
        set_tests_properties(playback_review_runner PROPERTIES
            WORKING_DIRECTORY "${MOONLIGHT_ROOT}")
    else()
        message(STATUS "Python 3.9+ not found; skipping playback_review_runner")
    endif()
endif()

if(VRR_BUILD_OVERLAY_TEST)
    find_path(VRR_SDL2_TTF_INCLUDE_DIR SDL_ttf.h
        HINTS ${PC_SDL2_TTF_INCLUDE_DIRS} "${VRR_INCLUDE_HINT}"
        PATH_SUFFIXES SDL2 SDL2_ttf.framework/Headers)
    find_library(VRR_SDL2_TTF_LIBRARY NAMES SDL2_ttf
        HINTS ${PC_SDL2_TTF_LIBRARY_DIRS} "${VRR_LIBRARY_HINT}")
    find_package_handle_standard_args(VrrOverlayDependencies REQUIRED_VARS
        VRR_SDL2_TTF_INCLUDE_DIR VRR_SDL2_TTF_LIBRARY)
    if(NOT VrrOverlayDependencies_FOUND)
        message(FATAL_ERROR "SDL2_ttf is required unless VRR_BUILD_OVERLAY_TEST=OFF")
    endif()
    vrr_test(tst_overlay tst_overlay.cpp
        "${MOONLIGHT_ROOT}/app/streaming/video/overlaymanager.cpp"
        "${MOONLIGHT_ROOT}/app/path.cpp"
        ARGS "${MOONLIGHT_ROOT}/app")
    target_include_directories(tst_overlay PRIVATE "${MOONLIGHT_ROOT}/app"
        "${MOONLIGHT_ROOT}/app/streaming/video" "${VRR_SDL2_TTF_INCLUDE_DIR}")
    target_link_libraries(tst_overlay PRIVATE vrr_sdl2 Qt6::Core
        Threads::Threads "${VRR_SDL2_TTF_LIBRARY}")
    if(WIN32)
        target_link_libraries(tst_overlay PRIVATE shell32)
    endif()
endif()

if(VRR_BUILD_PLATFORM_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        vrr_test(tst_vulkantiming tst_vulkantiming.cpp "${RENDERERS}/vulkantiming.cpp")
        target_include_directories(tst_vulkantiming PRIVATE "${VRR_COMMON_INCLUDE_DIR}")
        target_link_libraries(tst_vulkantiming PRIVATE Qt6::Core Vulkan::Vulkan)
    else()
        message(STATUS "Vulkan SDK not found; skipping tst_vulkantiming")
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND QUIET IMPORTED_TARGET wayland-client wayland-server)
    endif()
    if(WAYLAND_FOUND)
        vrr_test(tst_waylandfeedback tst_waylandfeedback.cpp
            "${RENDERERS}/waylandfeedback/wayland.cpp"
            "${RENDERERS}/protocols/presentation-time-protocol.c")
        target_include_directories(tst_waylandfeedback PRIVATE
            "${MOONLIGHT_ROOT}/app" "${VRR_COMMON_INCLUDE_DIR}")
        target_link_libraries(tst_waylandfeedback PRIVATE
            Qt6::Core vrr_sdl2 PkgConfig::WAYLAND Threads::Threads)
    else()
        message(STATUS "Wayland development packages not found; skipping tst_waylandfeedback")
    endif()
endif()
