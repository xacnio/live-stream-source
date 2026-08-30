cmake_minimum_required(VERSION 3.16)

project(live-stream-source VERSION 1.3.0 LANGUAGES C CXX)

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
    add_ffmpeg_lib(avfilter)

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
        add_ffmpeg_lib(avfilter)

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
        pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswscale libswresample libavfilter)
        
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
        add_ffmpeg_lib(avfilter avfilter)
    endif()
endif()

# ──────────────────────────────────────────────────────────────
# SoundTouch for professional audio time-stretching
# ──────────────────────────────────────────────────────────────
include(FetchContent)

# Set minimum CMake policy version for SoundTouch compatibility
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum CMake policy version" FORCE)
endif()

FetchContent_Declare(
    SoundTouch
    GIT_REPOSITORY https://codeberg.org/soundtouch/soundtouch.git
    GIT_TAG        2.3.3
    GIT_SHALLOW    TRUE
)

# Configure SoundTouch build options
# BUILD_SHARED_LIBS OFF forces add_library(SoundTouch ...) to produce a static lib.
# SOUNDTOUCH_DLL controls only the separate C wrapper (SoundTouchDLL), unrelated.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SOUNDSTRETCH OFF CACHE BOOL "Disable soundstretch utility" FORCE)

FetchContent_MakeAvailable(SoundTouch)

message(STATUS "SoundTouch library configured successfully")

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
    src/core/catchup-orchestrator.cpp
    src/media/stream-demuxer.cpp
    src/media/video-decoder.cpp
    src/media/audio-decoder.cpp
    src/media/audio-time-stretcher.cpp
    src/media/frame-queue.cpp
    src/utils/bitrate-monitor.cpp
    src/utils/reconnect-manager.cpp
    src/utils/buffer-manager.cpp
    src/utils/tempo-ramper.cpp
    src/network/ws-stats-server.cpp
    src/network/preview-encoder.cpp
    src/protocols/ll-hls/ll-hls-parser.cpp
    src/protocols/ll-hls/ll-hls-fetcher.cpp
    src/protocols/ll-hls/ll-hls-client.cpp
    src/protocols/whep/whep-signaling.cpp
    src/protocols/whep/whep-client.cpp
    src/core/plugin-settings.cpp
    src/ui/dashboard-dialog.cpp
    src/ui/update-checker.cpp
)

set(PLUGIN_HEADERS
    ${CMAKE_CURRENT_BINARY_DIR}/html-assets.h
    src/core/plugin-main.h
    src/core/live-stream-source.h
    src/core/catchup-orchestrator.h
    src/media/stream-demuxer.h
    src/media/video-decoder.h
    src/media/audio-decoder.h
    src/media/frame-queue.h
    src/utils/bitrate-monitor.h
    src/utils/reconnect-manager.h
    src/network/ws-stats-server.h
    src/network/preview-encoder.h
    src/core/common.h
    src/protocols/ll-hls/ll-hls-parser.h
    src/protocols/ll-hls/ll-hls-fetcher.h
    src/protocols/ll-hls/ll-hls-client.h
    src/protocols/whep/whep-signaling.h
    src/protocols/whep/whep-client.h
    src/core/plugin-settings.h
    src/ui/dashboard-dialog.h
    src/ui/update-checker.h
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
    ffmpeg::avfilter
    Qt6::Widgets
    Qt6::Core
    Qt6::Network
    SoundTouch
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

    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
    # Use MbedTLS instead of OpenSSL — and build it statically ourselves
    # below — so the plugin DLL ships with no runtime OpenSSL dependency
    # (no libcrypto-3-x64.dll / libssl-3-x64.dll alongside the plugin).
    # NOTE: this only affects the libdatachannel we statically embed in
    # our plugin. It does NOT touch OBS's own datachannel.dll in
    # obs-plugins/64bit, which is unrelated and stays as-is.
    set(USE_OPENSSL OFF CACHE BOOL "Force MbedTLS — avoids OpenSSL DLL runtime dep" FORCE)
    set(USE_MBEDTLS ON CACHE BOOL "Use mbedTLS (static, no runtime DLLs)" FORCE)

    # Fix for CMake 4.x: old deps (plog) use cmake_minimum_required < 3.5
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

    # Build MbedTLS ourselves as a static library BEFORE libdatachannel.
    # The thirdparty/lib/libmbedtls.lib is an IMPORT lib pointing at
    # obs-deps' MbedTLS DLLs — using it would just swap libcrypto.dll for
    # libmbedtls.dll. We fetch and statically build MbedTLS, then expose
    # it as MbedTLS::* alias targets, and prepend our shim FindMbedTLS.cmake
    # so libdatachannel + libsrtp + usrsctp all resolve to OUR static libs
    # instead of finding the dynamic ones in thirdparty/.
    list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_SOURCE_DIR}/cmake")
    if(NOT TARGET MbedTLS::MbedTLS)
        FetchContent_Declare(
            mbedtls
            GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
            GIT_TAG        v3.6.2
            GIT_SHALLOW    TRUE
        )
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
        # Force STATIC — must be set before MakeAvailable so MbedTLS's own
        # add_library() calls pick STATIC, not SHARED.
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(mbedtls)
        if(TARGET mbedtls)
            # MbedTLS v3.x ships with DTLS-SRTP disabled in its stock config.
            # libdatachannel needs it for WebRTC key exchange, so we layer
            # an additional user-config header on top. Public so the define
            # propagates into libdatachannel's own translation units.
            set(_mbedtls_user_cfg "${CMAKE_SOURCE_DIR}/cmake/mbedtls_user_config.h")
            foreach(_t IN ITEMS mbedtls mbedcrypto mbedx509)
                target_compile_definitions(${_t} PUBLIC
                    "MBEDTLS_USER_CONFIG_FILE=\"${_mbedtls_user_cfg}\"")
            endforeach()
            unset(_mbedtls_user_cfg)

            add_library(MbedTLS::MbedTLS    ALIAS mbedtls)
            add_library(MbedTLS::MbedCrypto ALIAS mbedcrypto)
            add_library(MbedTLS::MbedX509   ALIAS mbedx509)
            message(STATUS "Static MbedTLS built — plugin will be self-contained")
        endif()
    endif()

    FetchContent_Declare(
        libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG        v0.22.2
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    # Skip install rules while pulling in libdatachannel + libsrtp + usrsctp.
    # libsrtp tries to register `srtp2` in its install export set, but srtp2
    # privately links our FetchContent `mbedtls` target which isn't in any
    # export set → generate-step error. We don't run `cmake --install` for
    # those deps anyway (they're statically linked into our plugin), so
    # silencing their install() registrations is harmless. The flag is
    # re-enabled below before our own install(TARGETS ${PROJECT_NAME}).
    set(CMAKE_SKIP_INSTALL_RULES ON)
    FetchContent_MakeAvailable(libdatachannel)
    set(CMAKE_SKIP_INSTALL_RULES OFF)

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
    # Add /FS flag to allow parallel compilation with PDB files
    target_compile_options(${PROJECT_NAME} PRIVATE /Zi /FS)
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

