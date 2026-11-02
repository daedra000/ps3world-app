#!/bin/sh
# build-mbedtls.sh <dir> <cc> <ar> <cflags>: Mbed TLS 3.6.7 as the app uses it.
#
# The same configuration for the PPU build and the host HTTPS test, so the test
# exercises what ships: 3.6 LTS pinned by commit, TLS 1.2 only (TLS 1.3 needs
# PSA crypto, which wants a platform entropy source; ours is lv2's, added per
# connection), no sockets/timing/file layers (http_ps3.c does the I/O).
set -e
dir=$1 cc=$2 ar=$3 cflags=$4
git clone -q --depth 1 -b v3.6.7 --recurse-submodules --shallow-submodules https://github.com/Mbed-TLS/mbedtls.git "$dir"
cd "$dir"
test "$(git rev-parse HEAD)" = 068ff080b369adfac81509f9b57b2afabaf82dc5
for o in MBEDTLS_NET_C MBEDTLS_TIMING_C MBEDTLS_FS_IO MBEDTLS_PSA_CRYPTO_C \
         MBEDTLS_PSA_CRYPTO_STORAGE_C MBEDTLS_PSA_ITS_FILE_C MBEDTLS_SELF_TEST \
         MBEDTLS_HAVE_ASM MBEDTLS_AESNI_C MBEDTLS_PADLOCK_C MBEDTLS_AESCE_C MBEDTLS_LMS_C \
         MBEDTLS_SSL_PROTO_TLS1_3 MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE \
         MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED \
         MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED \
         MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED; do
  python3 scripts/config.py unset $o
done
python3 scripts/config.py set MBEDTLS_NO_PLATFORM_ENTROPY
python3 scripts/config.py set MBEDTLS_PLATFORM_MS_TIME_ALT   # mbedtls_ms_time() is in http_ps3.c (lv2 clock)
make -C library -j"$(nproc)" CC="$cc" AR="$ar" CFLAGS="$cflags" WARNING_CFLAGS="-w" static
