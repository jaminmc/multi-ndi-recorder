cmake_minimum_required(VERSION 3.20)
project(MultiNdiRecorder 
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOUIC_SEARCH_PATHS ${CMAKE_SOURCE_DIR}/ui)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Platform-specific path defaults
if (WIN32)
    set(NDI_SDK_INCLUDE "C:/Program Files/NDI SDK/Include" CACHE PATH "Path to NDI SDK include")
    set(NDI_SDK_LIB "C:/Program Files/NDI SDK/Lib/x64" CACHE PATH "Path to NDI SDK lib")
    set(FFMPEG_INCLUDE "C:/ffmpeg/include" CACHE PATH "Path to FFmpeg includes")
    set(FFMPEG_LIB_ROOT "C:/ffmpeg" CACHE PATH "Path to FFmpeg root (containing lib/bin)")
elseif (APPLE)
    set(NDI_SDK_INCLUDE "/Library/NDI SDK for Apple/include" CACHE PATH "Path to NDI SDK include")
    set(NDI_SDK_LIB "/Library/NDI SDK for Apple/lib/macOS" CACHE PATH "Path to NDI SDK lib")
    set(FFMPEG_INCLUDE "/usr/local/include" CACHE PATH "Path to FFmpeg includes")
    set(FFMPEG_LIB_ROOT "/usr/local" CACHE PATH "Path to FFmpeg root (containing lib)")
endif()

include_directories(${NDI_SDK_INCLUDE} ${FFMPEG_INCLUDE} include)

set(_ffmpeg_search_paths
    ${FFMPEG_LIB_ROOT}
    ${FFMPEG_LIB_ROOT}/lib
    ${FFMPEG_LIB_ROOT}/bin
)

# Platform-specific NDI library names
if (WIN32)
    find_library(NDI_LIBRARY NAMES Processing.NDI.Lib.x64 PATHS ${NDI_SDK_LIB} PATH_SUFFIXES lib bin)
elseif (APPLE)
    find_library(NDI_LIBRARY NAMES ndi libndi libProcessingNDI ProcessingNDI PATHS ${NDI_SDK_LIB} NO_DEFAULT_PATH)
    if (NOT NDI_LIBRARY)
        # Fallback to system search
        find_library(NDI_LIBRARY NAMES ndi libndi libProcessingNDI ProcessingNDI)
    endif()
endif()

find_library(AVFORMAT_LIBRARY NAMES avformat PATHS ${_ffmpeg_search_paths} PATH_SUFFIXES lib bin)
find_library(AVCODEC_LIBRARY NAMES avcodec PATHS ${_ffmpeg_search_paths} PATH_SUFFIXES lib bin)
find_library(AVUTIL_LIBRARY NAMES avutil PATHS ${_ffmpeg_search_paths} PATH_SUFFIXES lib bin)
find_library(SWSCALE_LIBRARY NAMES swscale PATHS ${_ffmpeg_search_paths} PATH_SUFFIXES lib bin)
find_library(SWRESAMPLE_LIBRARY NAMES swresample PATHS ${_ffmpeg_search_paths} PATH_SUFFIXES lib bin)

if (NOT NDI_LIBRARY)
    if (WIN32)
        message(FATAL_ERROR "Could not find Processing.NDI.Lib.x64. Set NDI_SDK_LIB to the NDI SDK library directory (lib or bin).")
    else()
        message(FATAL_ERROR "Could not find NDI library. Expected location: ${NDI_SDK_LIB}/libndi.dylib. Set NDI_SDK_LIB to the NDI SDK library directory.")
    endif()
endif()
if (NOT AVFORMAT_LIBRARY)
    message(FATAL_ERROR "Could not find avformat library. Set FFMPEG_LIB_ROOT to the FFmpeg installation root (containing lib or bin).")
endif()

file(GLOB SRC_FILES
    src/*.cpp
)

file(GLOB HEADER_FILES
    include/*.h
)

# macOS bundle configuration
if (APPLE)
    set(MACOSX_BUNDLE_BUNDLE_NAME "Multi NDI Recorder")
    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.multindirecorder.app")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
    set(MACOSX_BUNDLE_COPYRIGHT "Copyright © 2024")
    set(MACOSX_BUNDLE_ICON_FILE "app_icon.icns")
    
    # Generate .icns file from iconset if it doesn't exist
    set(ICON_ICONSET_DIR "${CMAKE_SOURCE_DIR}/icons/app_icon.iconset")
    set(ICON_ICNS_FILE "${CMAKE_SOURCE_DIR}/icons/app_icon.icns")
    
    # Check if .icns exists, if not try to generate it
    if (NOT EXISTS "${ICON_ICNS_FILE}" AND EXISTS "${ICON_ICONSET_DIR}")
        message(STATUS "Generating app icon from iconset...")
        execute_process(
            COMMAND iconutil -c icns "${ICON_ICONSET_DIR}" -o "${ICON_ICNS_FILE}"
            RESULT_VARIABLE ICON_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if (NOT ICON_RESULT EQUAL 0)
            message(WARNING "Failed to generate .icns file. You may need to run icons/generate_icon.sh manually.")
        endif()
    endif()
    
    add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${SRC_FILES} ${HEADER_FILES} ui/MainWindow.ui ui/SourceSettingsDialog.ui ui/SourceTile.ui)
    
    # Set bundle properties
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE "${MACOSX_BUNDLE_ICON_FILE}"
    )
    
    # Copy icon to bundle
    if (EXISTS "${ICON_ICNS_FILE}")
        set_source_files_properties("${ICON_ICNS_FILE}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(${PROJECT_NAME} PRIVATE "${ICON_ICNS_FILE}")
    endif()
else()
    add_executable(${PROJECT_NAME} ${SRC_FILES} ${HEADER_FILES} ui/MainWindow.ui ui/SourceSettingsDialog.ui ui/SourceTile.ui)
endif()

target_link_libraries(${PROJECT_NAME}
    Qt6::Widgets
    # NDI SDK
    ${NDI_LIBRARY}
    # FFmpeg
    ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY} ${SWRESAMPLE_LIBRARY}
)

# Platform-specific libraries
if (WIN32)
    target_link_libraries(${PROJECT_NAME}
        mfplat mfreadwrite mfuuid strmiids ole32 oleaut32 uuid winmm
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif (APPLE)
    find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(FOUNDATION_FRAMEWORK Foundation)
    if (COREAUDIO_FRAMEWORK)
        target_link_libraries(${PROJECT_NAME} ${COREAUDIO_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
    endif()
endif()
