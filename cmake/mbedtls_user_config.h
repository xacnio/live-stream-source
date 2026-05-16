/*
 * MbedTLS user-config overlay — applied on top of MbedTLS's stock
 * mbedtls_config.h. Enables features libdatachannel needs for WebRTC
 * (DTLS-SRTP) that aren't on by default in MbedTLS v3.x.
 *
 * MbedTLS includes this via the MBEDTLS_USER_CONFIG_FILE preprocessor
 * macro, which the root CMakeLists.txt sets when building the static
 * MbedTLS targets.
 */

#pragma once

#ifndef MBEDTLS_SSL_DTLS_SRTP
#define MBEDTLS_SSL_DTLS_SRTP
#endif
