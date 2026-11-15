/*
 * mc_ce_arm64.h - ARMv8/v9 Cryptography Extensions for minicrypto
 *
 * Hardware accelerated AES, PMULL, SHA256, SHA512, and RNDR for
 * Oryon CPUs (Snapdragon 8 Elite / SM8750) and ARM64 processors.
 */

#ifndef _MC_CE_ARM64_H_
#define _MC_CE_ARM64_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef KERNEL
#include <mach/boolean.h>
#include <machine/machine_routines.h>
#include <kern/cpu_data.h>
#ifndef disable_preemption
extern void _disable_preemption(void);
extern void _enable_preemption(void);
#define disable_preemption() _disable_preemption()
#define enable_preemption() _enable_preemption()
#endif
#endif

/* AES key structure */
#ifndef _MC_AES_KEY_DEFINED
#define _MC_AES_KEY_DEFINED
struct mc_aes_key {
	uint32_t rounds;
	uint8_t  rk[240];           /* (rounds + 1) round keys of 16 bytes */
};
#endif

/*
 * CPU Cryptographic Capabilities
 */
struct mc_ce_capabilities {
	bool has_aes;       /* ARMv8 AES instructions (aese, aesmc, aesd, aesimc) */
	bool has_pmull;     /* Polynomial multiply (pmull, pmull2) */
	bool has_sha1;      /* SHA-1 instructions */
	bool has_sha256;    /* SHA-256 instructions (sha256h, sha256h2, sha256su*) */
	bool has_sha512;    /* SHA-512 instructions (sha512h, sha512h2, sha512su*) */
	bool has_rndr;      /* Hardware random number generation (RNDR / FEAT_RNG) */
	bool initialized;
};

extern struct mc_ce_capabilities mc_ce_caps;

/* Initialization and feature inquiries */
void mc_ce_init(void);
bool mc_ce_has_aes(void);
bool mc_ce_has_pmull(void);
bool mc_ce_has_sha1(void);
bool mc_ce_has_sha256(void);
bool mc_ce_has_sha512(void);
bool mc_ce_has_rndr(void);

/* Hardware RNDR entropy generation */
bool mc_ce_rndr(uint64_t *val);

/*
 * FPU / NEON register protection for kernel mode.
 * In non-interrupt context with buffer length >= 64 bytes,
 * saves/restores q0..q7, q16..q19, fpcr, and fpsr on a 16-byte aligned stack frame
 * (total 12 * 16 = 192 bytes, avoiding AAPCS64 callee-saved q8..q15),
 * and manages CPACR_EL1.FPEN to prevent corrupting userland SIMD state.
 */
struct mc_fpu_state {
	uint8_t  q[12 * 16] __attribute__((aligned(16)));
	uint64_t fpcr;
	uint64_t fpsr;
	uint64_t cpacr;
};

static inline void
kernel_fpu_begin(struct mc_fpu_state *state)
{
#ifdef KERNEL
	disable_preemption();
#if defined(__aarch64__) || defined(__arm64__)
	if (state) {
		uint64_t cpacr;
		__asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
		state->cpacr = cpacr;
		uint64_t new_cpacr = cpacr | (3ULL << 20);
		if (new_cpacr != cpacr) {
			__asm__ volatile("msr cpacr_el1, %0\n\tisb" :: "r"(new_cpacr));
		}
		uint64_t fpcr, fpsr;
		__asm__ volatile(
			".arch armv8-a+crypto\n\t"
			"mrs %0, fpcr\n\t"
			"mrs %1, fpsr\n\t"
			"stp q0, q1, [%2, #0]\n\t"
			"stp q2, q3, [%2, #32]\n\t"
			"stp q4, q5, [%2, #64]\n\t"
			"stp q6, q7, [%2, #96]\n\t"
			"stp q16, q17, [%2, #128]\n\t"
			"stp q18, q19, [%2, #160]\n\t"
			: "=r"(fpcr), "=r"(fpsr)
			: "r"(state->q)
			: "memory"
		);
		state->fpcr = fpcr;
		state->fpsr = fpsr;
	}
#endif
#else
	(void)state;
#endif
}

static inline void
kernel_fpu_end(struct mc_fpu_state *state)
{
#ifdef KERNEL
#if defined(__aarch64__) || defined(__arm64__)
	if (state) {
		__asm__ volatile(
			".arch armv8-a+crypto\n\t"
			"ldp q0, q1, [%0, #0]\n\t"
			"ldp q2, q3, [%0, #32]\n\t"
			"ldp q4, q5, [%0, #64]\n\t"
			"ldp q6, q7, [%0, #96]\n\t"
			"ldp q16, q17, [%0, #128]\n\t"
			"ldp q18, q19, [%0, #160]\n\t"
			"msr fpcr, %1\n\t"
			"msr fpsr, %2\n\t"
			:: "r"(state->q), "r"(state->fpcr), "r"(state->fpsr)
			: "memory"
		);
		if ((state->cpacr & (3ULL << 20)) != (3ULL << 20)) {
			__asm__ volatile("msr cpacr_el1, %0\n\tisb" :: "r"(state->cpacr));
		}
	}
#endif
	enable_preemption();
#else
	(void)state;
#endif
}

/*
 * Hardware accelerated AES operations
 */
void mc_ce_aes_encrypt_block(const struct mc_aes_key *k, const uint8_t *in, uint8_t *out);
void mc_ce_aes_decrypt_block(const struct mc_aes_key *k, const uint8_t *in, uint8_t *out);

void mc_ce_aes_ecb_encrypt(const struct mc_aes_key *k, size_t nblocks, const uint8_t *in, uint8_t *out);
void mc_ce_aes_ecb_decrypt(const struct mc_aes_key *k, size_t nblocks, const uint8_t *in, uint8_t *out);

void mc_ce_aes_cbc_encrypt(const struct mc_aes_key *k, uint8_t *iv, size_t nblocks, const uint8_t *in, uint8_t *out);
void mc_ce_aes_cbc_decrypt(const struct mc_aes_key *k, uint8_t *iv, size_t nblocks, const uint8_t *in, uint8_t *out);

void mc_ce_aes_ctr_crypt(const struct mc_aes_key *k, uint8_t *counter, size_t nbytes, const uint8_t *in, uint8_t *out);

/*
 * Hardware PMULL operation (64-bit polynomial multiply -> 128-bit)
 */
void mc_ce_pmull64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo);
void mc_pmull64_scalar(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo);

/*
 * Hardware accelerated digest compression functions
 */
void mc_ce_sha256_compress(uint32_t state[8], size_t nblocks, const void *in);
void mc_ce_sha512_compress(uint64_t state[8], size_t nblocks, const void *in);

#endif /* _MC_CE_ARM64_H_ */
