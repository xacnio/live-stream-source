cmake_minimum_required(VERSION 3.16)

project(live-stream-source VERSION 1.0.0 LANGUAGES C CXX)

get_filename_component(_cmake_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Qt6 Configuration ────────────────────────────────────────
set(CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/thirdparty" "${CMAKE_SOURCE_DIR}/thirdparty/qt6")
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
find_package(Python REQUIRED COMPONENTS Interpreter)
message(STATUS "Using Qt6: ${Qt6_DIR}")

# ── Optional features ────────────────────────────────────────
option(ENABLE_WHEP "Enable WHEP (WebRTC) receiver support" ON)

# ──────────────────────────────────────────────────────────────
# Configuration — adjust these paths for your system
# ──────────────────────────────────────────────────────────────
# OBS_SOURCE_DIR : OBS Studio source tree with headers
# OBS_BIN_DIR    : Directory containing obs.dll
# FFMPEG_DIR     : FFmpeg dev build (with include/ and lib/)
# ──────────────────────────────────────────────────────────────

set(PLUGIN_VERSION ${PROJECT_VERSION})
add_compile_definitions(PLUGIN_VERSION="${PROJECT_VERSION}")

if(WIN32)
    # ──────────────────────────────────────────────────────────────
    # Windows: Generate import library from obs.dll
    # ──────────────────────────────────────────────────────────────
    if(NOT DEFINED OBS_SOURCE_DIR)
        set(OBS_SOURCE_DIR "C:/Program Files/obs-studio/obs-studio-32.0.1-sources" CACHE PATH "Path to OBS Studio source tree")
    endif()
    if(NOT DEFINED OBS_BIN_DIR)
        set(OBS_BIN_DIR "C:/Program Files/obs-studio/bin/64bit" CACHE PATH "Path to obs.dll directory")
    endif()
    if(NOT DEFINED FFMPEG_DIR)
        set(FFMPEG_DIR "C:/ffmpeg-master-latest-win64-gpl-shared" CACHE PATH "Path to FFmpeg dev build")
    endif()

    set(OBS_DLL "${OBS_BIN_DIR}/obs.dll")
    set(OBS_IMPLIB "${CMAKE_BINARY_DIR}/obs.lib")
    set(OBS_DEF    "${CMAKE_BINARY_DIR}/obs.def")

    if(NOT EXISTS "${OBS_IMPLIB}")
        message(STATUS "Generating import library for obs.dll...")
        get_filename_component(_cmake_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        execute_process(
            COMMAND "${_cmake_cxx_dir}/dumpbin.exe" /EXPORTS "${OBS_DLL}"
            OUTPUT_VARIABLE _dumpbin_output
            RESULT_VARIABLE _dumpbin_result
        )
        message(STATUS "dumpbin output: ${_dumpbin_output}")
        if(NOT _dumpbin_result EQUAL 0)
            message(FATAL_ERROR "dumpbin failed. Make sure MSVC is properly configured.")
        endif()
        set(_def_content "LIBRARY obs\nEXPORTS\n")
        string(REPLACE "\n" ";" _lines "${_dumpbin_output}")
        foreach(_line IN LISTS _lines)
            string(REGEX MATCH "^[ ]+[0-9]+[ ]+[0-9A-Fa-f]+[ ]+[0-9A-Fa-f]+[ ]+([^ ]+)" _match "${_line}")
            if(_match)
                string(REGEX REPLACE "^[ ]+[0-9]+[ ]+[0-9A-Fa-f]+[ ]+[0-9A-Fa-f]+[ ]+" "" _symbol "${_line}")
                string(STRIP "${_symbol}" _symbol)
                if(_symbol)
                    string(APPEND _def_content "    ${_symbol}\n")
                endif()
            endif()
        endforeach()
        file(WRITE "${OBS_DEF}" "${_def_content}")
        execute_process(
            COMMAND "${_cmake_cxx_dir}/lib.exe" "/DEF:${OBS_DEF}" "/OUT:${OBS_IMPLIB}" /MACHINE:X64
            RESULT_VARIABLE _lib_result
        )
        if(NOT _lib_result EQUAL 0)
            message(FATAL_ERROR "lib.exe failed to create import library")
        endif()
    endif()

    set(OBS_FRONTEND_DLL "${OBS_BIN_DIR}/obs-frontend-api.dll")
    set(OBS_FRONTEND_IMPLIB "${CMAKE_BINARY_DIR}/obs-frontend-api.lib")
    set(OBS_FRONTEND_DEF    "${CMAKE_BINARY_DIR}/obs-frontend-api.def")

    if(NOT EXISTS "${OBS_FRONTEND_IMPLIB}")
        message(STATUS "Generating import library for obs-frontend-api.dll...")
        execute_process(
            COMMAND "${_cmake_cxx_dir}/dumpbin.exe" /EXPORTS "${OBS_FRONTEND_DLL}"
            OUTPUT_VARIABLE _dumpbin_frontend_output
            RESULT_VARIABLE _dumpbin_frontend_result
        )
        if(NOT _dumpbin_frontend_result EQUAL 0)
            message(FATAL_ERROR "dumpbin failed for frontend api.")
        endif()
        set(_frontend_def_content "LIBRARY obs-frontend-api\nEXPORTS\n")
        string(REPLACE "\n" ";" _frontend_lines "${_dumpbin_frontend_output}")
        foreach(_line IN LISTS _frontend_lines)
            string(REGEX MATCH "^[ ]+[0-9]+[ ]+[0-9A-Fa-f]+[ ]+[0-9A-Fa-f]+[ ]+([^ ]+)" _match "${_line}")
            if(_match)
                set(_symbol "${CMAKE_MATCH_1}")
                string(STRIP "${_symbol}" _symbol)
                if(_symbol)
                    string(APPEND _frontend_def_content "    ${_symbol}\n")
                endif()
            endif()
        endforeach()
        file(WRITE "${OBS_FRONTEND_DEF}" "${_frontend_def_content}")
        execute_process(
            COMMAND "${_cmake_cxx_dir}/lib.exe" "/DEF:${OBS_FRONTEND_DEF}" "/OUT:${OBS_FRONTEND_IMPLIB}" /MACHINE:X64
            RESULT_VARIABLE _lib_frontend_result
        )
        if(NOT _lib_frontend_result EQUAL 0)
            message(FATAL_ERROR "lib.exe failed to create frontend import library")
        endif()
    endif()

    add_library(libobs SHARED IMPORTED)
    set_target_properties(libobs PROPERTIES
        IMPORTED_LOCATION "${OBS_DLL}"
        IMPORTED_IMPLIB   "${OBS_IMPLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs"
    )

    add_library(libobs-frontend-api SHARED IMPORTED)
    set_target_properties(libobs-frontend-api PROPERTIES
        IMPORTED_LOCATION "${OBS_FRONTEND_DLL}"
        IMPORTED_IMPLIB   "${OBS_FRONTEND_IMPLIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/frontend/api"
    )

    set(FFMPEG_INCLUDE_DIR "${FFMPEG_DIR}/include")
    set(FFMPEG_LIB_DIR     "${FFMPEG_DIR}/lib")
    function(add_ffmpeg_lib name)
        add_library(ffmpeg::${name} SHARED IMPORTED)
        set_target_properties(ffmpeg::${name} PROPERTIES
            IMPORTED_IMPLIB "${FFMPEG_LIB_DIR}/${name}.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
        )
    endfunction()

    add_ffmpeg_lib(avformat)
    add_ffmpeg_lib(avcodec)
    add_ffmpeg_lib(avutil)
    add_ffmpeg_lib(swscale)
    add_ffmpeg_lib(swresample)

else()
    # ──────────────────────────────────────────────────────────────
    # macOS / Linux: Use PkgConfig and standard paths
    # ──────────────────────────────────────────────────────────────
    find_package(PkgConfig REQUIRED)
    
    if(APPLE)
        # Add obs-deps to the find_package prefix path so we pick up its mbedtls/openssl instead of brew's
        list(APPEND CMAKE_PREFIX_PATH "${FFMPEG_DIR}")

        # For macOS, we link directly against the .dylib files from the OBS.app bundle
        add_library(libobs SHARED IMPORTED)
        set_target_properties(libobs PROPERTIES
            IMPORTED_LOCATION "${OBS_BIN_DIR}/libobs.framework/libobs"
            INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs"
        )
        add_library(libobs-frontend-api SHARED IMPORTED)
        set_target_properties(libobs-frontend-api PROPERTIES
            IMPORTED_LOCATION "${OBS_BIN_DIR}/libobs-frontend-api.1.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/frontend/api"
        )

        set(FFMPEG_INCLUDE_DIR "${FFMPEG_DIR}/include")
        set(FFMPEG_LIB_DIR     "${FFMPEG_DIR}/lib")
        function(add_ffmpeg_lib name)
            add_library(ffmpeg::${name} SHARED IMPORTED)
            set_target_properties(ffmpeg::${name} PROPERTIES
                IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/lib${name}.dylib"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
            )
        endfunction()
        
        add_ffmpeg_lib(avformat)
        add_ffmpeg_lib(avcodec)
        add_ffmpeg_lib(avutil)
        add_ffmpeg_lib(swscale)
        add_ffmpeg_lib(swresample)

    else()
        # OBS dependencies for Linux
        if(OBS_SOURCE_DIR AND OBS_BIN_DIR)
            add_library(libobs SHARED IMPORTED)
            find_library(OBS_LIB_PATH obs PATHS "${OBS_BIN_DIR}" "${OBS_BIN_DIR}/lib" "${OBS_BIN_DIR}/lib64" "${OBS_BIN_DIR}/lib/x86_64-linux-gnu" NO_DEFAULT_PATH)
            if(OBS_LIB_PATH)
                set_target_properties(libobs PROPERTIES
                    IMPORTED_LOCATION "${OBS_LIB_PATH}"
                    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs"
                )
            else()
                message(FATAL_ERROR "Could not find libobs.so in ${OBS_BIN_DIR} or its lib/lib64 subdirectories")
            endif()

            add_library(libobs-frontend-api SHARED IMPORTED)
            find_library(OBS_FRONTEND_LIB_PATH obs-frontend-api PATHS "${OBS_BIN_DIR}" "${OBS_BIN_DIR}/lib" "${OBS_BIN_DIR}/lib64" "${OBS_BIN_DIR}/lib/x86_64-linux-gnu" NO_DEFAULT_PATH)
            if(OBS_FRONTEND_LIB_PATH)
                set_target_properties(libobs-frontend-api PROPERTIES
                    IMPORTED_LOCATION "${OBS_FRONTEND_LIB_PATH}"
                    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/frontend/api"
                )
            else()
                message(FATAL_ERROR "Could not find libobs-frontend-api.so in ${OBS_BIN_DIR} or its lib/lib64 subdirectories")
            endif()
        else()
            pkg_check_modules(OBS REQUIRED libobs)
            
            add_library(libobs INTERFACE IMPORTED)
            target_link_libraries(libobs INTERFACE ${OBS_LIBRARIES})
            target_include_directories(libobs INTERFACE ${OBS_INCLUDE_DIRS})
            target_link_directories(libobs INTERFACE ${OBS_LIBRARY_DIRS})

            add_library(libobs-frontend-api INTERFACE IMPORTED)
            target_link_libraries(libobs-frontend-api INTERFACE obs-frontend-api)
        endif()
        
        # FFmpeg dependencies for Linux
        pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale libswresample)
        
        function(add_ffmpeg_lib name lib_name)
            add_library(ffmpeg::${name} INTERFACE IMPORTED)
            target_link_libraries(ffmpeg::${name} INTERFACE ${lib_name})
            target_include_directories(ffmpeg::${name} INTERFACE ${FFMPEG_INCLUDE_DIRS})
            target_link_directories(ffmpeg::${name} INTERFACE ${FFMPEG_LIBRARY_DIRS})
        endfunction()

        add_ffmpeg_lib(avformat avformat)
        add_ffmpeg_lib(avcodec avcodec)
        add_ffmpeg_lib(avutil avutil)
        add_ffmpeg_lib(swscale swscale)
        add_ffmpeg_lib(swresample swresample)
    endif()
endif()

# ──────────────────────────────────────────────────────────────
# Plugin target
# ──────────────────────────────────────────────────────────────

# ──────────────────────────────────────────────────────────────
# Generate HTML assets header
# ──────────────────────────────────────────────────────────────
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/html-assets.h"
    COMMAND Python::Interpreter "${CMAKE_SOURCE_DIR}/cmake/embed_html.py" "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS 
        "${CMAKE_SOURCE_DIR}/docs/dashboard.html"
        "${CMAKE_SOURCE_DIR}/docs/stats-overlay.html"
        "${CMAKE_SOURCE_DIR}/cmake/embed_html.py"
    COMMENT "Embedding HTML assets into C++ header using Python"
)

set(PLUGIN_SOURCES
    src/core/plugin-main.cpp
    src/core/live-stream-source.cpp
    src/media/stream-demuxer.cpp
    src/media/video-decoder.cpp
    src/media/audio-decoder.cpp
    src/media/frame-queue.cpp
    src/utils/bitrate-monitor.cpp
    src/utils/catchup-controller.cpp
    src/utils/reconnect-manager.cpp
    src/network/ws-stats-server.cpp
    src/protocols/ll-hls/ll-hls-parser.cpp
    src/protocols/ll-hls/ll-hls-fetcher.cpp
    src/protocols/ll-hls/ll-hls-client.cpp
    src/protocols/whep/whep-signaling.cpp
    src/protocols/whep/whep-client.cpp
    src/core/plugin-settings.cpp
    src/ui/dashboard-dialog.cpp
)

set(PLUGIN_HEADERS
    ${CMAKE_CURRENT_BINARY_DIR}/html-assets.h
    src/core/plugin-main.h
    src/core/live-stream-source.h
    src/media/stream-demuxer.h
    src/media/video-decoder.h
    src/media/audio-decoder.h
    src/media/frame-queue.h
    src/utils/bitrate-monitor.h
    src/utils/catchup-controller.h
    src/utils/reconnect-manager.h
    src/network/ws-stats-server.h
    src/core/common.h
    src/protocols/ll-hls/ll-hls-parser.h
    src/protocols/ll-hls/ll-hls-fetcher.h
    src/protocols/ll-hls/ll-hls-client.h
    src/protocols/whep/whep-signaling.h
    src/protocols/whep/whep-client.h
    src/core/plugin-settings.h
    src/ui/dashboard-dialog.h
)

add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

target_compile_definitions(${PROJECT_NAME} PRIVATE "PLUGIN_VERSION=\"${PROJECT_VERSION}\"")
configure_file(installer.iss.in ${CMAKE_CURRENT_BINARY_DIR}/installer.iss @ONLY)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/core"     # For obsconfig.h (must come BEFORE OBS headers)
    "${CMAKE_CURRENT_BINARY_DIR}"      # For html-assets.h
)

if(WIN32)
    target_include_directories(${PROJECT_NAME} PRIVATE 
        "${OBS_SOURCE_DIR}/libobs"
        "${FFMPEG_INCLUDE_DIR}"
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    libobs
    libobs-frontend-api
    ffmpeg::avformat
    ffmpeg::avcodec
    ffmpeg::avutil
    ffmpeg::swscale
    ffmpeg::swresample
    Qt6::Widgets
    Qt6::Core
    Qt6::Network
)

if(NOT WIN32)
    # Target Linux/macOS standard libraries if needed
    if(APPLE)
        target_link_libraries(${PROJECT_NAME} PRIVATE "-framework CoreFoundation")
    endif()
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 winhttp)
endif()

# ──────────────────────────────────────────────────────────────
# Optional: libdatachannel for WHEP (WebRTC) support
# ──────────────────────────────────────────────────────────────
if(ENABLE_WHEP)
    message(STATUS "WHEP (WebRTC) support: ENABLED")

    include(FetchContent)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
    if(APPLE)
        set(USE_OPENSSL OFF CACHE BOOL "Disable system OpenSSL (fixes cross-compile on Mac)" FORCE)
        set(USE_MBEDTLS ON CACHE BOOL "Use mbedTLS instead" FORCE)
    endif()

    FetchContent_Declare(
        libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG        v0.22.2
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    # Fix for CMake 4.x: old deps (plog) use cmake_minimum_required < 3.5
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_MakeAvailable(libdatachannel)

    target_link_libraries(${PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannelStatic)
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_LIBDATACHANNEL)
    message(STATUS "libdatachannel linked successfully")
else()
    message(STATUS "WHEP (WebRTC) support: DISABLED (set -DENABLE_WHEP=ON to enable)")
endif()

# Windows-specific definitions
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    # Generate PDB debug info in Release builds for crash analysis
    target_compile_options(${PROJECT_NAME} PRIVATE /Zi)
    target_link_options(${PROJECT_NAME} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()

# ──────────────────────────────────────────────────────────────
# Install
# ──────────────────────────────────────────────────────────────
install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION "obs-plugins/64bit"
    RUNTIME DESTINATION "obs-plugins/64bit"
)

install(DIRECTORY "data/locale/"
    DESTINATION "data/obs-plugins/${PROJECT_NAME}/locale"
)
