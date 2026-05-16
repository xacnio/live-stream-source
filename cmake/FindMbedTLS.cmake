# Shim FindMbedTLS — short-circuits the upstream search when we have
# already provided static MbedTLS targets via FetchContent in the root
# CMakeLists.txt. Without this, libdatachannel + libsrtp + usrsctp each
# call find_package(MbedTLS) and pick up the dynamic import libs sitting
# in thirdparty/lib/ (libmbedtls.lib → libmbedtls.dll), forcing the
# plugin to ship those DLLs alongside live-stream-source.dll.
#
# Our root CMakeLists.txt prepends this file's directory to
# CMAKE_MODULE_PATH, so this is the FindMbedTLS that find_package(MbedTLS)
# resolves first.

if(TARGET MbedTLS::MbedTLS)
  set(MbedTLS_FOUND TRUE)
  set(MBEDTLS_FOUND TRUE)
  set(MbedTLS_VERSION "3.6.2")
  set(MBEDTLS_LIBRARIES MbedTLS::MbedTLS)
  if(DEFINED mbedtls_SOURCE_DIR)
    set(MBEDTLS_INCLUDE_DIRS "${mbedtls_SOURCE_DIR}/include")
    set(MbedTLS_INCLUDE_DIR  "${mbedtls_SOURCE_DIR}/include")
  endif()
  # All advertised components are satisfied by our static build.
  foreach(_component IN ITEMS MbedTLS MbedCrypto MbedX509)
    set(MbedTLS_${_component}_FOUND TRUE)
  endforeach()
  return()
endif()

# Fallback — if the static MbedTLS wasn't set up (e.g. someone disabled
# WHEP) we deliberately do nothing here, so find_package(MbedTLS) just
# reports NOT FOUND. Builds that need MbedTLS must enable the WHEP path
# in the root CMakeLists.txt, which provides the static targets.
