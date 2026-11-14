#pragma once
// HEADLESS-2026-10 (spike) -- the part of Apple's CommonCrypto the engine
// uses (SHA-256 of a LUT and of a media file, LookLutStorage.cc,
// ProjectStorage.cc), answered by OpenSSL on a host without it. Only on the
// headless build's include path: the macOS build keeps CommonCrypto.
#ifndef OPENSSL_SUPPRESS_DEPRECATED
#define OPENSSL_SUPPRESS_DEPRECATED
#endif
#include <openssl/sha.h>

#include <cstddef>
#include <cstdint>

using CC_LONG = uint32_t;
using CC_SHA256_CTX = SHA256_CTX;
inline constexpr std::size_t CC_SHA256_DIGEST_LENGTH = SHA256_DIGEST_LENGTH;

inline int CC_SHA256_Init(CC_SHA256_CTX* context) {
    return SHA256_Init(context);
}
inline int CC_SHA256_Update(CC_SHA256_CTX* context, const void* data,
                            CC_LONG length) {
    return SHA256_Update(context, data, length);
}
inline int CC_SHA256_Final(unsigned char* digest, CC_SHA256_CTX* context) {
    return SHA256_Final(digest, context);
}
inline unsigned char* CC_SHA256(const void* data, CC_LONG length,
                                unsigned char* digest) {
    return SHA256(static_cast<const unsigned char*>(data), length, digest);
}
