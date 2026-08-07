#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The registry host test does not need a cryptographic implementation; it
// only needs a deterministic digest with the same call shape as mbedTLS so
// it can exercise exact-match and fail-closed branches without IDF headers.
struct mbedtls_sha256_context {
    std::array<uint32_t, 8> words{};
};

inline int mbedtls_sha256_init(mbedtls_sha256_context *context) {
    context->words = {0x243F6A88u, 0x85A308D3u, 0x13198A2Eu, 0x03707344u,
                      0xA4093822u, 0x299F31D0u, 0x082EFA98u, 0xEC4E6C89u};
    return 0;
}

inline int mbedtls_sha256_starts(mbedtls_sha256_context *context, int) {
    return mbedtls_sha256_init(context);
}

inline int mbedtls_sha256_update(mbedtls_sha256_context *context,
                                 const unsigned char *data, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        const size_t word = index % context->words.size();
        context->words[word] =
            (context->words[word] * 16777619u) ^
            (static_cast<uint32_t>(data[index]) +
             static_cast<uint32_t>(index * 0x9E3779B9u));
    }
    return 0;
}

inline int mbedtls_sha256_finish(mbedtls_sha256_context *context,
                                 unsigned char output[32]) {
    for (size_t index = 0; index < context->words.size(); ++index) {
        const uint32_t word = context->words[index];
        output[index * 4] = static_cast<unsigned char>(word >> 24);
        output[index * 4 + 1] = static_cast<unsigned char>(word >> 16);
        output[index * 4 + 2] = static_cast<unsigned char>(word >> 8);
        output[index * 4 + 3] = static_cast<unsigned char>(word);
    }
    return 0;
}

inline void mbedtls_sha256_free(mbedtls_sha256_context *) {}

inline std::array<uint8_t, 32> makcm_test_digest(const uint8_t *first,
                                                 size_t first_length,
                                                 const uint8_t *second = nullptr,
                                                 size_t second_length = 0) {
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_update(&context, first, first_length);
    if (second_length != 0)
        mbedtls_sha256_update(&context, second, second_length);
    std::array<uint8_t, 32> digest{};
    mbedtls_sha256_finish(&context, digest.data());
    return digest;
}

inline std::array<uint8_t, 32> makcm_test_zero_sha() {
    return {};
}

inline constexpr std::array<uint8_t, 18> kMakcmTestDevice = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x31, 0x41, 0x19, 0x35,
    0x00, 0x01, 1, 2, 3, 1,
};

inline constexpr std::array<uint8_t, 25> kMakcmTestConfiguration = {
    9, 2, 25, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 0xFF, 0x5D, 1, 0,
    7, 5, 0x81, 3, 32, 0, 1,
};

inline constexpr std::array<uint8_t, 3> kMakcmTestHid = {
    0x05, 0x01, 0x09,
};

inline std::array<uint8_t, 32> makcm_test_descriptor_sha() {
    return makcm_test_digest(kMakcmTestDevice.data(), kMakcmTestDevice.size(),
                             kMakcmTestConfiguration.data(),
                             kMakcmTestConfiguration.size());
}

inline uint32_t makcm_test_descriptor_hash() {
    const auto digest = makcm_test_descriptor_sha();
    return (static_cast<uint32_t>(digest[0]) << 24) |
           (static_cast<uint32_t>(digest[1]) << 16) |
           (static_cast<uint32_t>(digest[2]) << 8) |
           static_cast<uint32_t>(digest[3]);
}

inline std::array<uint8_t, 32> makcm_test_hid_sha() {
    return makcm_test_digest(kMakcmTestHid.data(), kMakcmTestHid.size());
}

inline uint32_t makcm_test_hid_hash() {
    const auto digest = makcm_test_hid_sha();
    return (static_cast<uint32_t>(digest[0]) << 24) |
           (static_cast<uint32_t>(digest[1]) << 16) |
           (static_cast<uint32_t>(digest[2]) << 8) |
           static_cast<uint32_t>(digest[3]);
}
