/*
 * mc_ce_arm64.c - ARMv8/v9 Cryptography Extensions for minicrypto
 *
 * Implements hardware accelerated AES, PMULL, SHA256, SHA512, and RNDR
 * using inline assembly for Qualcomm Oryon / ARM64 processors.
 *
 * All vector and crypto instructions are executed within asm volatile blocks
 * with explicit architecture directives (.arch armv8-a+crypto+sha2+sha3) so that
 * compilation with -mgeneral-regs-only succeeds cleanly in the XNU kernel.
 */

#include "mc_ce_arm64.h"
#include <string.h>

struct mc_ce_capabilities mc_ce_caps = {
	.has_aes = false,
	.has_pmull = false,
	.has_sha1 = false,
	.has_sha256 = false,
	.has_sha512 = false,
	.has_rndr = false,
	.initialized = false,
};

void
mc_ce_init(void)
{
	if (mc_ce_caps.initialized) {
		return;
	}

#if defined(KERNEL) && (defined(__aarch64__) || defined(__arm64__))
	uint64_t isar0 = 0;

	__asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));

	uint64_t aes_val = (isar0 >> 4) & 0xf;
	uint64_t sha1_val = (isar0 >> 8) & 0xf;
	uint64_t sha2_val = (isar0 >> 12) & 0xf;
	uint64_t sha512_val = (isar0 >> 16) & 0xf;
	uint64_t rndr_val = (isar0 >> 60) & 0xf;

	mc_ce_caps.has_aes = (aes_val >= 1);
	mc_ce_caps.has_pmull = (aes_val >= 2);
	mc_ce_caps.has_sha1 = (sha1_val >= 1);
	mc_ce_caps.has_sha256 = (sha2_val >= 1);
	mc_ce_caps.has_sha512 = (sha512_val >= 1);
	mc_ce_caps.has_rndr = (rndr_val >= 1);
#else
	mc_ce_caps.has_aes = true;
	mc_ce_caps.has_pmull = true;
	mc_ce_caps.has_sha1 = true;
	mc_ce_caps.has_sha256 = true;
	mc_ce_caps.has_sha512 = true;
	mc_ce_caps.has_rndr = false;
#endif

	mc_ce_caps.initialized = true;
}

bool
mc_ce_has_aes(void)
{
	if (!mc_ce_caps.initialized) {
		mc_ce_init();
	}
	return mc_ce_caps.has_aes;
}

bool
mc_ce_has_pmull(void)
{
	if (!mc_ce_caps.initialized) {
		mc_ce_init();
	}
	return mc_ce_caps.has_pmull;
}

bool
mc_ce_has_sha1(void)
{
	if (!mc_ce_caps.initialized) {
		mc_ce_init();
	}
	return mc_ce_caps.has_sha1;
}

bool
mc_ce_has_sha256(void)
{
	if (!mc_ce_caps.initialized) {
		mc_ce_init();
	}
	return mc_ce_caps.has_sha256;
}

bool
mc_ce_has_sha512(void)
{
	if (!mc_ce_caps.initialized) {
		mc_ce_init();
	}
	return mc_ce_caps.has_sha512;
}

bool
mc_ce_has_rndr(void)
{
	if (!mc_ce_caps.initialized) {
		mc_ce_init();
	}
	return mc_ce_caps.has_rndr;
}

bool
mc_ce_rndr(uint64_t *val)
{
	if (!mc_ce_has_rndr()) {
		return false;
	}
#if defined(__aarch64__) || defined(__arm64__)
	uint64_t v = 0;
	uint32_t success = 0;
	__asm__ volatile(
		"mrs %0, s3_3_c2_c4_0\n\t"
		"cset %w1, ne\n\t"
		: "=r"(v), "=r"(success)
		:: "cc"
	);
	if (success) {
		*val = v;
		return true;
	}
#else
	(void)val;
#endif
	return false;
}

#if defined(__aarch64__) || defined(__arm64__)

void
mc_ce_aes_encrypt_block(const struct mc_aes_key *k, const uint8_t *in, uint8_t *out)
{
	const uint8_t *key_ptr = k->rk;
	uint32_t rounds = k->rounds;

	__asm__ volatile(
		".arch armv8-a+crypto\n\t"
		"ldr q0, [%2]\n\t"
		"ldr q1, [%0], #16\n\t"
		"sub %w1, %w1, #1\n\t"
		"1:\n\t"
		"aese v0.16b, v1.16b\n\t"
		"aesmc v0.16b, v0.16b\n\t"
		"ldr q1, [%0], #16\n\t"
		"subs %w1, %w1, #1\n\t"
		"b.ne 1b\n\t"
		"aese v0.16b, v1.16b\n\t"
		"ldr q1, [%0]\n\t"
		"eor v0.16b, v0.16b, v1.16b\n\t"
		"str q0, [%3]\n\t"
		: "+r"(key_ptr), "+r"(rounds)
		: "r"(in), "r"(out)
#ifdef KERNEL
		: "cc", "memory"
#else
		: "v0", "v1", "cc", "memory"
#endif
	);
}

void
mc_ce_aes_decrypt_block(const struct mc_aes_key *k, const uint8_t *in, uint8_t *out)
{
	const uint8_t *key_ptr = k->rk;
	uint32_t rounds = k->rounds;

	__asm__ volatile(
		".arch armv8-a+crypto\n\t"
		"ldr q0, [%2]\n\t"
		"add x4, %0, %x1, lsl #4\n\t"
		"ldr q1, [x4], #-16\n\t"
		"aesd v0.16b, v1.16b\n\t"
		"aesimc v0.16b, v0.16b\n\t"
		"sub %w1, %w1, #2\n\t"
		"1:\n\t"
		"ldr q1, [x4], #-16\n\t"
		"aesimc v1.16b, v1.16b\n\t"
		"aesd v0.16b, v1.16b\n\t"
		"aesimc v0.16b, v0.16b\n\t"
		"subs %w1, %w1, #1\n\t"
		"b.ne 1b\n\t"
		"ldr q1, [x4], #-16\n\t"
		"aesimc v1.16b, v1.16b\n\t"
		"aesd v0.16b, v1.16b\n\t"
		"ldr q1, [x4]\n\t"
		"eor v0.16b, v0.16b, v1.16b\n\t"
		"str q0, [%3]\n\t"
		: "+r"(key_ptr), "+r"(rounds)
		: "r"(in), "r"(out)
#ifdef KERNEL
		: "x4", "cc", "memory"
#else
		: "x4", "v0", "v1", "cc", "memory"
#endif
	);
}

void
mc_ce_aes_ecb_encrypt(const struct mc_aes_key *k, size_t nblocks, const uint8_t *in, uint8_t *out)
{
	const uint8_t *key_ptr = k->rk;
	uint32_t rounds = k->rounds;

	__asm__ volatile(
		".arch armv8-a+crypto\n\t"
		"1:\n\t"
		"cbz %2, 3f\n\t"
		"ldr q0, [%3], #16\n\t"
		"mov x6, %0\n\t"
		"mov w7, %w1\n\t"
		"ldr q1, [x6], #16\n\t"
		"sub w7, w7, #1\n\t"
		"2:\n\t"
		"aese v0.16b, v1.16b\n\t"
		"aesmc v0.16b, v0.16b\n\t"
		"ldr q1, [x6], #16\n\t"
		"subs w7, w7, #1\n\t"
		"b.ne 2b\n\t"
		"aese v0.16b, v1.16b\n\t"
		"ldr q1, [x6]\n\t"
		"eor v0.16b, v0.16b, v1.16b\n\t"
		"str q0, [%4], #16\n\t"
		"sub %2, %2, #1\n\t"
		"b 1b\n\t"
		"3:\n\t"
		: "+r"(key_ptr), "+r"(rounds), "+r"(nblocks), "+r"(in), "+r"(out)
#ifdef KERNEL
		:: "x6", "x7", "cc", "memory"
#else
		:: "x6", "x7", "v0", "v1", "cc", "memory"
#endif
	);
}

void
mc_ce_aes_ecb_decrypt(const struct mc_aes_key *k, size_t nblocks, const uint8_t *in, uint8_t *out)
{
	const uint8_t *key_ptr = k->rk;
	uint32_t rounds = k->rounds;

	__asm__ volatile(
		".arch armv8-a+crypto\n\t"
		"1:\n\t"
		"cbz %2, 3f\n\t"
		"ldr q0, [%3], #16\n\t"
		"add x6, %0, %x1, lsl #4\n\t"
		"ldr q1, [x6], #-16\n\t"
		"aesd v0.16b, v1.16b\n\t"
		"aesimc v0.16b, v0.16b\n\t"
		"sub w7, %w1, #2\n\t"
		"2:\n\t"
		"ldr q1, [x6], #-16\n\t"
		"aesimc v1.16b, v1.16b\n\t"
		"aesd v0.16b, v1.16b\n\t"
		"aesimc v0.16b, v0.16b\n\t"
		"subs w7, w7, #1\n\t"
		"b.ne 2b\n\t"
		"ldr q1, [x6], #-16\n\t"
		"aesimc v1.16b, v1.16b\n\t"
		"aesd v0.16b, v1.16b\n\t"
		"ldr q1, [x6]\n\t"
		"eor v0.16b, v0.16b, v1.16b\n\t"
		"str q0, [%4], #16\n\t"
		"sub %2, %2, #1\n\t"
		"b 1b\n\t"
		"3:\n\t"
		: "+r"(key_ptr), "+r"(rounds), "+r"(nblocks), "+r"(in), "+r"(out)
#ifdef KERNEL
		:: "x6", "x7", "cc", "memory"
#else
		:: "x6", "x7", "v0", "v1", "cc", "memory"
#endif
	);
}

void
mc_ce_aes_cbc_encrypt(const struct mc_aes_key *k, uint8_t *iv, size_t nblocks, const uint8_t *in, uint8_t *out)
{
	const uint8_t *key_ptr = k->rk;
	uint32_t rounds = k->rounds;

	__asm__ volatile(
		".arch armv8-a+crypto\n\t"
		"ldr q0, [%2]\n\t"

		"1:\n\t"
		"cbz %3, 3f\n\t"
		"ldr q2, [%4], #16\n\t"
		"eor v0.16b, v0.16b, v2.16b\n\t"

		"mov x6, %0\n\t"
		"mov w7, %w1\n\t"
		"ldr q1, [x6], #16\n\t"
		"sub w7, w7, #1\n\t"
		"2:\n\t"
		"aese v0.16b, v1.16b\n\t"
		"aesmc v0.16b, v0.16b\n\t"
		"ldr q1, [x6], #16\n\t"
		"subs w7, w7, #1\n\t"
		"b.ne 2b\n\t"
		"aese v0.16b, v1.16b\n\t"
		"ldr q1, [x6]\n\t"
		"eor v0.16b, v0.16b, v1.16b\n\t"

		"str q0, [%5], #16\n\t"
		"sub %3, %3, #1\n\t"
		"b 1b\n\t"

		"3:\n\t"
		"str q0, [%2]\n\t"
		: "+r"(key_ptr), "+r"(rounds), "+r"(iv), "+r"(nblocks), "+r"(in), "+r"(out)
#ifdef KERNEL
		:: "x6", "x7", "cc", "memory"
#else
		:: "x6", "x7", "v0", "v1", "v2", "cc", "memory"
#endif
	);
}

void
mc_ce_aes_cbc_decrypt(const struct mc_aes_key *k, uint8_t *iv, size_t nblocks, const uint8_t *in, uint8_t *out)
{
	const uint8_t *key_ptr = k->rk;
	uint32_t rounds = k->rounds;

	__asm__ volatile(
		".arch armv8-a+crypto\n\t"
		"ldr q0, [%2]\n\t"

		"1:\n\t"
		"cbz %3, 3f\n\t"
		"ldr q2, [%4], #16\n\t"
		"mov v3.16b, v2.16b\n\t"

		"add x6, %0, %x1, lsl #4\n\t"
		"ldr q1, [x6], #-16\n\t"
		"aesd v3.16b, v1.16b\n\t"
		"aesimc v3.16b, v3.16b\n\t"
		"sub w7, %w1, #2\n\t"
		"2:\n\t"
		"ldr q1, [x6], #-16\n\t"
		"aesimc v1.16b, v1.16b\n\t"
		"aesd v3.16b, v1.16b\n\t"
		"aesimc v3.16b, v3.16b\n\t"
		"subs w7, w7, #1\n\t"
		"b.ne 2b\n\t"
		"ldr q1, [x6], #-16\n\t"
		"aesimc v1.16b, v1.16b\n\t"
		"aesd v3.16b, v1.16b\n\t"
		"ldr q1, [x6]\n\t"
		"eor v3.16b, v3.16b, v1.16b\n\t"

		"eor v3.16b, v3.16b, v0.16b\n\t"
		"str q3, [%5], #16\n\t"
		"mov v0.16b, v2.16b\n\t"

		"sub %3, %3, #1\n\t"
		"b 1b\n\t"

		"3:\n\t"
		"str q0, [%2]\n\t"
		: "+r"(key_ptr), "+r"(rounds), "+r"(iv), "+r"(nblocks), "+r"(in), "+r"(out)
#ifdef KERNEL
		:: "x6", "x7", "cc", "memory"
#else
		:: "x6", "x7", "v0", "v1", "v2", "v3", "cc", "memory"
#endif
	);
}

static inline void
mc_ctr_inc(uint8_t counter[16])
{
	for (int i = 15; i >= 0; i--) {
		if (++counter[i] != 0) {
			break;
		}
	}
}

void
mc_ce_aes_ctr_crypt(const struct mc_aes_key *k, uint8_t *counter, size_t nbytes, const uint8_t *in, uint8_t *out)
{
	uint8_t ks[16];
	while (nbytes >= 16) {
		mc_ce_aes_encrypt_block(k, counter, ks);
		mc_ctr_inc(counter);
		for (int i = 0; i < 16; i++) {
			out[i] = (uint8_t)(in[i] ^ ks[i]);
		}
		in += 16;
		out += 16;
		nbytes -= 16;
	}
	if (nbytes > 0) {
		mc_ce_aes_encrypt_block(k, counter, ks);
		mc_ctr_inc(counter);
		for (size_t i = 0; i < nbytes; i++) {
			out[i] = (uint8_t)(in[i] ^ ks[i]);
		}
	}
}

void
mc_ce_pmull64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
	uint64_t l, h;
	__asm__ volatile(
		".arch armv8-a+crypto\n\t"
		"fmov d0, %2\n\t"
		"fmov d1, %3\n\t"
		"pmull v2.1q, v0.1d, v1.1d\n\t"
		"fmov %0, d2\n\t"
		"mov %1, v2.d[1]\n\t"
		: "=r"(l), "=r"(h)
		: "r"(a), "r"(b)
#ifdef KERNEL
		: "memory"
#else
		: "v0", "v1", "v2", "memory"
#endif
	);
	*lo = l;
	*hi = h;
}

#endif /* defined(__aarch64__) || defined(__arm64__) */

void
mc_pmull64_scalar(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
	uint64_t h = 0;
	uint64_t l = 0;

	for (int i = 0; i < 64; i++) {
		if ((b >> i) & 1) {
			l ^= (a << i);
			if (i > 0) {
				h ^= (a >> (64 - i));
			}
		}
	}
	*hi = h;
	*lo = l;
}

/* SHA-256 Constants */
static const uint32_t sha256_k[64] = {
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

#if defined(__aarch64__) || defined(__arm64__)

void
mc_ce_sha256_compress(uint32_t state[8], size_t nblocks, const void *in)
{
	const uint8_t *ptr = (const uint8_t *)in;
	const uint32_t *k_table = sha256_k;

	__asm__ volatile(
		".arch armv8-a+crypto+sha2\n\t"
		"ldp q0, q1, [%0]\n\t"

		"1:\n\t"
		"mov x6, %3\n\t"
		"ldr q2, [%1, #0]\n\t"
		"ldr q3, [%1, #16]\n\t"
		"ldr q4, [%1, #32]\n\t"
		"ldr q5, [%1, #48]\n\t"
		"add %1, %1, #64\n\t"

		"rev32 v2.16b, v2.16b\n\t"
		"rev32 v3.16b, v3.16b\n\t"
		"rev32 v4.16b, v4.16b\n\t"
		"rev32 v5.16b, v5.16b\n\t"

		"mov v16.16b, v0.16b\n\t"
		"mov v17.16b, v1.16b\n\t"

		"mov w7, #16\n\t"
		"2:\n\t"
		/* Sub-round 0 */
		"ldr q18, [x6], #16\n\t"
		"add v18.4s, v2.4s, v18.4s\n\t"
		"mov v7.16b, v2.16b\n\t"
		"sha256su0 v7.4s, v3.4s\n\t"
		"mov v6.16b, v0.16b\n\t"
		"sha256h q0, q1, v18.4s\n\t"
		"sha256h2 q1, v6, v18.4s\n\t"
		"sha256su1 v7.4s, v4.4s, v5.4s\n\t"
		"mov v2.16b, v7.16b\n\t"

		/* Sub-round 1 */
		"ldr q18, [x6], #16\n\t"
		"add v18.4s, v3.4s, v18.4s\n\t"
		"mov v7.16b, v3.16b\n\t"
		"sha256su0 v7.4s, v4.4s\n\t"
		"mov v6.16b, v0.16b\n\t"
		"sha256h q0, q1, v18.4s\n\t"
		"sha256h2 q1, v6, v18.4s\n\t"
		"sha256su1 v7.4s, v5.4s, v2.4s\n\t"
		"mov v3.16b, v7.16b\n\t"

		/* Sub-round 2 */
		"ldr q18, [x6], #16\n\t"
		"add v18.4s, v4.4s, v18.4s\n\t"
		"mov v7.16b, v4.16b\n\t"
		"sha256su0 v7.4s, v5.4s\n\t"
		"mov v6.16b, v0.16b\n\t"
		"sha256h q0, q1, v18.4s\n\t"
		"sha256h2 q1, v6, v18.4s\n\t"
		"sha256su1 v7.4s, v2.4s, v3.4s\n\t"
		"mov v4.16b, v7.16b\n\t"

		/* Sub-round 3 */
		"ldr q18, [x6], #16\n\t"
		"add v18.4s, v5.4s, v18.4s\n\t"
		"mov v7.16b, v5.16b\n\t"
		"sha256su0 v7.4s, v2.4s\n\t"
		"mov v6.16b, v0.16b\n\t"
		"sha256h q0, q1, v18.4s\n\t"
		"sha256h2 q1, v6, v18.4s\n\t"
		"sha256su1 v7.4s, v3.4s, v4.4s\n\t"
		"mov v5.16b, v7.16b\n\t"

		"subs w7, w7, #4\n\t"
		"b.ne 2b\n\t"

		"add v0.4s, v0.4s, v16.4s\n\t"
		"add v1.4s, v1.4s, v17.4s\n\t"

		"subs %2, %2, #1\n\t"
		"b.ne 1b\n\t"

		"stp q0, q1, [%0]\n\t"
		: "+r"(state), "+r"(ptr), "+r"(nblocks)
		: "r"(k_table)
#ifdef KERNEL
		: "x6", "x7", "cc", "memory"
#else
		: "x6", "x7", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "cc", "memory"
#endif
	);
}

/* SHA-512 Constants */
static const uint64_t sha512_k[80] = {
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

#define MC_ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

void
mc_ce_sha512_compress(uint64_t state[8], size_t nblocks, const void *in)
{
	const uint8_t *ptr = (const uint8_t *)in;

	for (; nblocks > 0; nblocks--, ptr += 128) {
		uint64_t sched[80];
		for (int i = 0; i < 16; i++) {
			uint64_t v;
			memcpy(&v, ptr + i * 8, 8);
			sched[i] = __builtin_bswap64(v);
		}
		for (int i = 16; i < 80; i++) {
			uint64_t s0 = MC_ROR64(sched[i - 15], 1) ^ MC_ROR64(sched[i - 15], 8) ^ (sched[i - 15] >> 7);
			uint64_t s1 = MC_ROR64(sched[i - 2], 19) ^ MC_ROR64(sched[i - 2], 61) ^ (sched[i - 2] >> 6);
			sched[i] = sched[i - 16] + s0 + sched[i - 7] + s1;
		}

		uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
		uint64_t e = state[4], f = state[5], g = state[6], h = state[7];

		for (int r = 0; r < 80; r += 2) {
			uint64_t w0 = sched[r], w1 = sched[r + 1];
			uint64_t k0 = sha512_k[r], k1 = sha512_k[r + 1];

			uint64_t w_in[2] = { g + k1 + w1, h + k0 + w0 };
			uint64_t xy[8] = { f, g, d, e, c, 0, a, b };
			uint64_t t1[2], a_new[2];

			__asm__ volatile(
				".arch armv8-a+crypto+sha3\n\t"
				"ldp q0, q1, [%1]\n\t"
				"ldr q2, [%0]\n\t"
				"sha512h q2, q0, v1.2d\n\t"
				"str q2, [%2]\n\t"
				"ldp q3, q4, [%1, #32]\n\t"
				"sha512h2 q2, q3, v4.2d\n\t"
				"str q2, [%3]\n\t"
				:: "r"(w_in), "r"(xy), "r"(t1), "r"(a_new)
#ifdef KERNEL
				: "memory"
#else
				: "v0", "v1", "v2", "v3", "v4", "memory"
#endif
			);

			uint64_t t1_0 = t1[1];
			uint64_t t1_1 = t1[0];
			uint64_t a1 = a_new[1];
			uint64_t a2 = a_new[0];

			uint64_t e1 = d + t1_0;
			uint64_t e2 = c + t1_1;

			h = f;
			g = e;
			f = e1;
			e = e2;
			d = b;
			c = a;
			b = a1;
			a = a2;
		}

		state[0] += a;
		state[1] += b;
		state[2] += c;
		state[3] += d;
		state[4] += e;
		state[5] += f;
		state[6] += g;
		state[7] += h;
	}
}

#endif /* defined(__aarch64__) || defined(__arm64__) */
