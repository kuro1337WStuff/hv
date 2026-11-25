#ifndef HV_SHA256_H
#define HV_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* FIPS 180-4 sections 4.1.2 and 6.2. All rotations here are by 1..31. */
static uint32_t hv_sha256_ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t hv_sha256_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* The caller supplies W[0..15]; later schedule words replace them in place. */
static void hv_sha256_compress(uint32_t h[8], uint32_t w[16])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], z = h[7];
    for (unsigned i = 0; i < 64; ++i) {
        unsigned j = i & 15;
        if (i >= 16) {
            uint32_t x = w[(i - 15) & 15], y = w[(i - 2) & 15];
            uint32_t s0 = hv_sha256_ror(x, 7) ^ hv_sha256_ror(x, 18) ^ (x >> 3);
            uint32_t s1 = hv_sha256_ror(y, 17) ^ hv_sha256_ror(y, 19) ^ (y >> 10);
            w[j] += s0 + w[(i - 7) & 15] + s1;
        }
        uint32_t sum1 = hv_sha256_ror(e, 6) ^ hv_sha256_ror(e, 11) ^ hv_sha256_ror(e, 25);
        uint32_t sum0 = hv_sha256_ror(a, 2) ^ hv_sha256_ror(a, 13) ^ hv_sha256_ror(a, 22);
        uint32_t t1 = z + sum1 + ((e & f) ^ (~e & g)) + k[i] + w[j];
        uint32_t t2 = sum0 + ((a & b) ^ (a & c) ^ (b & c));
        z = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += z;
}

/* data is readable for bytes bytes (NULL is valid for zero); bytes < 2^61. */
static void hv_sha256(const uint8_t *data, size_t bytes, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint32_t w[16];
    uint64_t bits = (uint64_t)bytes << 3;
    while (bytes >= 64) {
        for (unsigned i = 0; i < 16; ++i) w[i] = hv_sha256_be32(data + 4 * i);
        hv_sha256_compress(h, w);
        data += 64;
        bytes -= 64;
    }
    /* Construct the padded schedule directly, reserving its last 64 bits for
     * the original length. No unaligned loads or reads beyond the input. */
    unsigned blocks = bytes < 56 ? 1u : 2u;
    for (unsigned block = 0; block < blocks; ++block) {
        for (unsigned i = 0; i < 16; ++i) {
            uint32_t word = 0;
            for (unsigned j = 0; j < 4; ++j) {
                unsigned pos = 64 * block + 4 * i + j;
                word = (word << 8) | (pos < bytes ? data[pos] : pos == bytes ? 0x80u : 0u);
            }
            w[i] = word;
        }
        if (block + 1 == blocks) {
            w[14] = (uint32_t)(bits >> 32);
            w[15] = (uint32_t)bits;
        }
        hv_sha256_compress(h, w);
    }
    for (unsigned i = 0; i < 8; ++i) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

#endif
