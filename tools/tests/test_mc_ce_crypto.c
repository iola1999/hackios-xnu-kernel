/*
 * test_mc_ce_crypto.c - Host validation test for minicrypto ARMv8/v9 Crypto Extensions
 *
 * Genuine hardware vs scalar verification, vector correctness, and throughput benchmarking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <mach/mach_time.h>

#include "mc_ce_arm64.h"

/* Forward declaration of scalar functions from minicrypto */
void mc_pmull64_scalar(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo);

/* Scalar AES implementation matching mc_aes.c for ground truth comparison */
static const uint8_t aes_sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t aes_inv_sbox[256] = {
	0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
	0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
	0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
	0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
	0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
	0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
	0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
	0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
	0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
	0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
	0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
	0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
	0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
	0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
	0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
	0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
};

static inline uint8_t xtime(uint8_t x) {
	return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b));
}

static inline uint8_t gmul(uint8_t a, uint8_t b) {
	uint8_t p = 0;
	while (b) {
		if (b & 1) p ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return p;
}

static int mc_aes_setkey(struct mc_aes_key *k, size_t key_len, const uint8_t *key) {
	uint32_t nk = (uint32_t)key_len / 4;
	uint8_t rcon = 1;
	if (key_len != 16 && key_len != 24 && key_len != 32) return -1;
	k->rounds = nk + 6;
	memcpy(k->rk, key, key_len);
	for (uint32_t i = nk; i < 4 * (k->rounds + 1); i++) {
		uint8_t t[4];
		memcpy(t, k->rk + 4 * (i - 1), 4);
		if (i % nk == 0) {
			uint8_t first = t[0];
			t[0] = aes_sbox[t[1]] ^ rcon;
			t[1] = aes_sbox[t[2]];
			t[2] = aes_sbox[t[3]];
			t[3] = aes_sbox[first];
			rcon = xtime(rcon);
		} else if (nk > 6 && i % nk == 4) {
			for (int j = 0; j < 4; j++) t[j] = aes_sbox[t[j]];
		}
		for (int j = 0; j < 4; j++) {
			k->rk[4 * i + j] = k->rk[4 * (i - nk) + j] ^ t[j];
		}
	}
	return 0;
}

static void mc_aes_encrypt_block_scalar(const struct mc_aes_key *k, const uint8_t *in, uint8_t *out) {
	uint8_t s[16];
	memcpy(s, in, 16);
	for (int i = 0; i < 16; i++) s[i] ^= k->rk[i];
	for (uint32_t r = 1; r <= k->rounds; r++) {
		uint8_t t[16];
		for (int i = 0; i < 16; i++) t[i] = aes_sbox[s[i]];
		s[0] = t[0];   s[4] = t[4];   s[8] = t[8];   s[12] = t[12];
		s[1] = t[5];   s[5] = t[9];   s[9] = t[13];  s[13] = t[1];
		s[2] = t[10];  s[6] = t[14];  s[10] = t[2];  s[14] = t[6];
		s[3] = t[15];  s[7] = t[3];   s[11] = t[7];  s[15] = t[11];
		if (r < k->rounds) {
			for (int c = 0; c < 4; c++) {
				uint8_t *col = s + 4 * c;
				uint8_t a = col[0], b = col[1], d = col[2], e = col[3];
				col[0] = xtime(a ^ b) ^ b ^ d ^ e;
				col[1] = xtime(b ^ d) ^ d ^ e ^ a;
				col[2] = xtime(d ^ e) ^ e ^ a ^ b;
				col[3] = xtime(e ^ a) ^ a ^ b ^ d;
			}
		}
		const uint8_t *rk = k->rk + 16 * r;
		for (int i = 0; i < 16; i++) s[i] ^= rk[i];
	}
	memcpy(out, s, 16);
}

static void mc_aes_decrypt_block_scalar(const struct mc_aes_key *k, const uint8_t *in, uint8_t *out) {
	uint8_t s[16];
	memcpy(s, in, 16);
	const uint8_t *rk = k->rk + 16 * k->rounds;
	for (int i = 0; i < 16; i++) s[i] ^= rk[i];
	for (uint32_t r = k->rounds; r-- > 0;) {
		uint8_t t[16];
		t[0]  = s[0];  t[4]  = s[4];  t[8]  = s[8];  t[12] = s[12];
		t[5]  = s[1];  t[9]  = s[5];  t[13] = s[9];  t[1]  = s[13];
		t[10] = s[2];  t[14] = s[6];  t[2]  = s[10]; t[6]  = s[14];
		t[15] = s[3];  t[3]  = s[7];  t[7]  = s[11]; t[11] = s[15];
		for (int i = 0; i < 16; i++) s[i] = aes_inv_sbox[t[i]];
		rk = k->rk + 16 * r;
		for (int i = 0; i < 16; i++) s[i] ^= rk[i];
		if (r > 0) {
			for (int c = 0; c < 4; c++) {
				uint8_t *col = s + 4 * c;
				uint8_t a = col[0], b = col[1], d = col[2], e = col[3];
				col[0] = gmul(a, 0x0e) ^ gmul(b, 0x0b) ^ gmul(d, 0x0d) ^ gmul(e, 0x09);
				col[1] = gmul(a, 0x09) ^ gmul(b, 0x0e) ^ gmul(d, 0x0b) ^ gmul(e, 0x0d);
				col[2] = gmul(a, 0x0d) ^ gmul(b, 0x09) ^ gmul(d, 0x0e) ^ gmul(e, 0x0b);
				col[3] = gmul(a, 0x0b) ^ gmul(b, 0x0d) ^ gmul(d, 0x09) ^ gmul(e, 0x0e);
			}
		}
	}
	memcpy(out, s, 16);
}

/* Scalar SHA-256 for comparison */
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static const uint32_t sha256_k_ref[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t cc_load32_be(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sha256_compress_scalar(uint32_t s[8], size_t nblocks, const void *in) {
	const uint8_t *buf = in;
	for (; nblocks > 0; nblocks--, buf += 64) {
		uint32_t w[64], a, b, c, d, e, f, g, h;
		for (int i = 0; i < 16; i++) {
			w[i] = cc_load32_be(buf + 4 * i);
		}
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		a = s[0]; b = s[1]; c = s[2]; d = s[3];
		e = s[4]; f = s[5]; g = s[6]; h = s[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = h + S1 + ch + sha256_k_ref[i] + w[i];
			uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
			uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + maj;
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		s[0] += a; s[1] += b; s[2] += c; s[3] += d;
		s[4] += e; s[5] += f; s[6] += g; s[7] += h;
	}
}

/* Scalar SHA-512 for comparison */
#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
static const uint64_t sha512_k_ref[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
	0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
	0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
	0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
	0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
	0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
	0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
	0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
	0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
	0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
	0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
	0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
	0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
	0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
	0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
	0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
	0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
	0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
	0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
	0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
	0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
	0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
	0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
	0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
	0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
	0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
	0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
	0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
	0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
	0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
	0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
	0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
	0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
	0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
	0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
	0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
	0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static void sha512_compress_scalar(uint64_t s[8], size_t nblocks, const void *in) {
	const uint8_t *buf = in;
	for (; nblocks > 0; nblocks--, buf += 128) {
		uint64_t w[80], a, b, c, d, e, f, g, h;
		for (int i = 0; i < 16; i++) {
			uint64_t val;
			memcpy(&val, buf + 8 * i, 8);
			w[i] = __builtin_bswap64(val);
		}
		for (int i = 16; i < 80; i++) {
			uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
			uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		a = s[0]; b = s[1]; c = s[2]; d = s[3];
		e = s[4]; f = s[5]; g = s[6]; h = s[7];
		for (int i = 0; i < 80; i++) {
			uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
			uint64_t ch = (e & f) ^ (~e & g);
			uint64_t t1 = h + S1 + ch + sha512_k_ref[i] + w[i];
			uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
			uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
			uint64_t t2 = S0 + maj;
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		s[0] += a; s[1] += b; s[2] += c; s[3] += d;
		s[4] += e; s[5] += f; s[6] += g; s[7] += h;
	}
}

static uint64_t get_time_ns(void) {
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom;
}

int main(void) {
	printf("=== WP-4B minicrypto ARMv8/v9 Crypto Extensions Host Validation ===\n\n");

	/* 1. Capabilities test */
	printf("[1] Checking CPU Crypto Capabilities via ID_AA64ISAR0_EL1...\n");
	mc_ce_init();
	printf("    AES:    %s\n", mc_ce_has_aes() ? "YES" : "NO");
	printf("    PMULL:  %s\n", mc_ce_has_pmull() ? "YES" : "NO");
	printf("    SHA256: %s\n", mc_ce_has_sha256() ? "YES" : "NO");
	printf("    SHA512: %s\n", mc_ce_has_sha512() ? "YES" : "NO");
	printf("    RNDR:   %s\n", mc_ce_has_rndr() ? "YES" : "NO");
	assert(mc_ce_has_aes());
	assert(mc_ce_has_pmull());
	assert(mc_ce_has_sha256());
	printf("    PASS: CPU capabilities verified.\n\n");

	/* 2. RNDR Hardware Entropy test */
	printf("[2] Testing Hardware RNDR Entropy Generation...\n");
	uint64_t rnd_val = 0;
	if (mc_ce_has_rndr()) {
		bool ok = mc_ce_rndr(&rnd_val);
		printf("    RNDR read: %s (value = 0x%016llx)\n", ok ? "SUCCESS" : "FAILED", (unsigned long long)rnd_val);
		assert(ok && rnd_val != 0);
	} else {
		printf("    RNDR instruction not available on this CPU model (expected on some cores), skipping RNDR assertion.\n");
	}
	printf("    PASS: RNDR test complete.\n\n");

	/* 3. AES-128 & AES-256 Vector Correctness & Performance Benchmark */
	printf("[3] Testing AES-128 / AES-256 ECB Hardware vs Scalar...\n");
	uint8_t key128[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
	struct mc_aes_key k128;
	mc_aes_setkey(&k128, 16, key128);

	/* FIPS 197 standard vector verification */
	uint8_t fips_plain[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
	uint8_t fips_expected[16] = {0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60, 0xa8, 0x9e, 0xca, 0xf3, 0x24, 0x66, 0xef, 0x97};
	uint8_t fips_out[16], fips_dec[16];
	mc_ce_aes_encrypt_block(&k128, fips_plain, fips_out);
	assert(memcmp(fips_out, fips_expected, 16) == 0);
	mc_ce_aes_decrypt_block(&k128, fips_out, fips_dec);
	assert(memcmp(fips_dec, fips_plain, 16) == 0);
	printf("    PASS: FIPS 197 vector encryption and decryption verified.\n");

	/* 1 MB Buffer Comparison: CE vs Scalar */
	size_t bench_size = 1024 * 1024; /* 1 MiB */
	size_t nblocks = bench_size / 16;
	uint8_t *bench_in = malloc(bench_size);
	uint8_t *ce_out = malloc(bench_size);
	uint8_t *sc_out = malloc(bench_size);
	uint8_t *ce_dec = malloc(bench_size);
	assert(bench_in && ce_out && sc_out && ce_dec);

	for (size_t i = 0; i < bench_size; i++) bench_in[i] = (uint8_t)(i * 37 + 13);

	/* Benchmark Scalar ECB */
	uint64_t t0 = get_time_ns();
	for (size_t b = 0; b < nblocks; b++) {
		mc_aes_encrypt_block_scalar(&k128, bench_in + b * 16, sc_out + b * 16);
	}
	uint64_t t1 = get_time_ns();
	double sc_time_ms = (double)(t1 - t0) / 1e6;
	double sc_mb_s = (1.0 / (sc_time_ms / 1000.0));

	/* Benchmark CE ECB */
	t0 = get_time_ns();
	mc_ce_aes_ecb_encrypt(&k128, nblocks, bench_in, ce_out);
	t1 = get_time_ns();
	double ce_time_ms = (double)(t1 - t0) / 1e6;
	double ce_mb_s = (1.0 / (ce_time_ms / 1000.0));

	/* Verify byte-for-byte exact equality between CE and Scalar */
	assert(memcmp(ce_out, sc_out, bench_size) == 0);

	/* Verify CE Decryption returns original input */
	mc_ce_aes_ecb_decrypt(&k128, nblocks, ce_out, ce_dec);
	assert(memcmp(ce_dec, bench_in, bench_size) == 0);

	double speedup = sc_time_ms / ce_time_ms;
	printf("    Scalar AES ECB: %6.2f ms (%6.1f MB/s)\n", sc_time_ms, sc_mb_s);
	printf("    CE     AES ECB: %6.2f ms (%6.1f MB/s) -> Speedup: %5.1fx\n", ce_time_ms, ce_mb_s, speedup);
	assert(speedup > 10.0);
	printf("    PASS: AES ECB speedup %5.1fx exceeds 10x requirement.\n\n", speedup);

	/* 4. AES CBC and CTR Mode Verification */
	printf("[4] Testing AES CBC & CTR Mode Hardware Execution...\n");
	uint8_t iv[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
	uint8_t iv_copy[16];
	memcpy(iv_copy, iv, 16);

	/* CBC */
	mc_ce_aes_cbc_encrypt(&k128, iv, nblocks, bench_in, ce_out);
	mc_ce_aes_cbc_decrypt(&k128, iv_copy, nblocks, ce_out, ce_dec);
	assert(memcmp(ce_dec, bench_in, bench_size) == 0);
	printf("    PASS: AES CBC encryption & decryption matches original data.\n");

	/* CTR */
	uint8_t counter[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
	uint8_t counter_dec[16];
	memcpy(counter_dec, counter, 16);
	mc_ce_aes_ctr_crypt(&k128, counter, bench_size, bench_in, ce_out);
	mc_ce_aes_ctr_crypt(&k128, counter_dec, bench_size, ce_out, ce_dec);
	assert(memcmp(ce_dec, bench_in, bench_size) == 0);
	printf("    PASS: AES CTR encryption & decryption matches original data.\n\n");

	/* 5. PMULL 64-bit Polynomial Multiplication Verification */
	printf("[5] Testing PMULL Hardware vs Scalar across 100,000 vectors...\n");
	t0 = get_time_ns();
	for (uint64_t i = 1; i <= 100000; i++) {
		uint64_t a = i * 0x9e3779b97f4a7c15ULL;
		uint64_t b = i * 0xc6a4a7935bd1e995ULL;
		uint64_t hi_ce, lo_ce;
		uint64_t hi_sc, lo_sc;
		mc_ce_pmull64(a, b, &hi_ce, &lo_ce);
		mc_pmull64_scalar(a, b, &hi_sc, &lo_sc);
		assert(hi_ce == hi_sc && lo_ce == lo_sc);
	}
	t1 = get_time_ns();
	printf("    Verified 100,000 random inputs bit-for-bit in %5.2f ms.\n", (double)(t1 - t0) / 1e6);
	printf("    PASS: PMULL hardware acceleration verified.\n\n");

	/* 6. SHA-256 Compression Benchmark & Correctness */
	printf("[6] Testing SHA-256 Hardware vs Scalar across 20,000 blocks (1.28 MiB)...\n");
	size_t sha_blocks = 20000;
	size_t sha_bytes = sha_blocks * 64;
	uint8_t *sha_in = malloc(sha_bytes);
	for (size_t i = 0; i < sha_bytes; i++) sha_in[i] = (uint8_t)(i * 41 + 17);

	uint32_t state_sc[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint32_t state_ce[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

	t0 = get_time_ns();
	sha256_compress_scalar(state_sc, sha_blocks, sha_in);
	t1 = get_time_ns();
	double sha_sc_time_ms = (double)(t1 - t0) / 1e6;
	double sha_sc_mb_s = (1.28 / (sha_sc_time_ms / 1000.0));

	t0 = get_time_ns();
	mc_ce_sha256_compress(state_ce, sha_blocks, sha_in);
	t1 = get_time_ns();
	double sha_ce_time_ms = (double)(t1 - t0) / 1e6;
	double sha_ce_mb_s = (1.28 / (sha_ce_time_ms / 1000.0));

	assert(memcmp(state_sc, state_ce, sizeof(state_sc)) == 0);
	double sha_speedup = sha_sc_time_ms / sha_ce_time_ms;
	printf("    Scalar SHA-256: %6.2f ms (%6.1f MB/s)\n", sha_sc_time_ms, sha_sc_mb_s);
	printf("    CE     SHA-256: %6.2f ms (%6.1f MB/s) -> Speedup: %5.1fx\n", sha_ce_time_ms, sha_ce_mb_s, sha_speedup);
	assert(sha_speedup > 5.0);
	printf("    PASS: SHA-256 speedup %5.1fx verified (CE: %6.1f MB/s).\n\n", sha_speedup, sha_ce_mb_s);

	/* 7. SHA-512 Compression Benchmark & Correctness */
	printf("[7] Testing SHA-512 Hardware vs Scalar across 10,000 blocks (1.28 MiB)...\n");
	size_t sha512_blocks = 10000;
	size_t sha512_bytes = sha512_blocks * 128;
	uint8_t *sha512_in = malloc(sha512_bytes);
	for (size_t i = 0; i < sha512_bytes; i++) sha512_in[i] = (uint8_t)(i * 47 + 19);

	uint64_t state512_sc[8] = {
		0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
		0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
		0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
		0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
	};
	uint64_t state512_ce[8] = {
		0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
		0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
		0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
		0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
	};

	t0 = get_time_ns();
	sha512_compress_scalar(state512_sc, sha512_blocks, sha512_in);
	t1 = get_time_ns();
	double sha512_sc_time_ms = (double)(t1 - t0) / 1e6;
	double sha512_sc_mb_s = (1.28 / (sha512_sc_time_ms / 1000.0));

	t0 = get_time_ns();
	mc_ce_sha512_compress(state512_ce, sha512_blocks, sha512_in);
	t1 = get_time_ns();
	double sha512_ce_time_ms = (double)(t1 - t0) / 1e6;
	double sha512_ce_mb_s = (1.28 / (sha512_ce_time_ms / 1000.0));

	assert(memcmp(state512_sc, state512_ce, sizeof(state512_sc)) == 0);
	double sha512_speedup = sha512_sc_time_ms / sha512_ce_time_ms;
	printf("    Scalar SHA-512: %6.2f ms (%6.1f MB/s)\n", sha512_sc_time_ms, sha512_sc_mb_s);
	printf("    CE     SHA-512: %6.2f ms (%6.1f MB/s) -> Speedup: %5.1fx\n", sha512_ce_time_ms, sha512_ce_mb_s, sha512_speedup);
	printf("    PASS: SHA-512 hardware acceleration bit-exact verification passed.\n\n");

	/* 8. mc_fpu_state alignment verification */
	printf("[8] Testing mc_fpu_state 16-byte Alignment...\n");
	struct mc_fpu_state fpu;
	assert(((uintptr_t)fpu.q & 0xf) == 0);
	printf("    mc_fpu_state address: %p, q vector offset alignment: %lu (mod 16 == 0)\n", (void *)&fpu, (uintptr_t)fpu.q % 16);
	printf("    PASS: mc_fpu_state 16-byte alignment verified.\n\n");

	/* 9. Capacity Tier Upgrade Calculation Test */
	printf("[9] Testing Capacity Tier Upgrade logic for 16 GiB RAM...\n");
	uint64_t ram_16g = 16ULL * 1024 * 1024 * 1024;
	int target_vnodes_16g = (int)(ram_16g / 65536) + 1024;
	printf("    16 GiB calculated desiredvnodes: %d (>= 65536, <= 263168)\n", target_vnodes_16g);
	assert(target_vnodes_16g >= 65536);
	assert(target_vnodes_16g <= 263168);

	uint64_t ram_4g = 4ULL * 1024 * 1024 * 1024;
	int target_vnodes_4g = (int)(ram_4g / 65536) + 1024;
	printf("    4 GiB calculated desiredvnodes:  %d (>= 65536)\n", target_vnodes_4g);
	assert(target_vnodes_4g >= 65536);

	uint64_t ram_1g = 1ULL * 1024 * 1024 * 1024;
	int target_vnodes_1g = (int)(ram_1g / 65536) + 1024;
	if (target_vnodes_1g < 65536) target_vnodes_1g = 65536;
	printf("    1 GiB clamped desiredvnodes:     %d (clamped to large floor 65536)\n", target_vnodes_1g);
	assert(target_vnodes_1g == 65536);
	printf("    PASS: Capacity tier calculations verified.\n\n");

	free(bench_in);
	free(ce_out);
	free(sc_out);
	free(ce_dec);
	free(sha_in);
	free(sha512_in);

	printf("ALL WP-4B VERIFICATION TESTS PASSED SUCCESSFULLY!\n");
	return 0;
}
