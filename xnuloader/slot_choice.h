/*
 * Which boot partition holds this loader, decided from what the two boot partitions contain.
 *
 * The GPT active bit cannot answer it: LK's "fastboot boot" runs the loader from RAM while the
 * active slot is still Android's, and a loader that took the active slot for its own would flash
 * and maintain Android's boot partition. So the slots are told apart by their images: the loader
 * image (reseal-loader-img.py, mkxnuimg.py) is an Android boot image of header version 1 whose
 * kernel, at page_size, is a gzip stream; the K90's Android boot images are header version 4 with
 * an uncompressed kernel (a Magisk or KernelSU patched one as well).
 *
 * Pure functions with no loader state, so test_slot_choice.c builds them on the host.
 */
#ifndef SLOT_CHOICE_H
#define SLOT_CHOICE_H

/* how much of a boot partition boot_image_is_loader needs: the header and the kernel's first bytes */
#define SLOT_HEAD_BYTES 8192u

static int
boot_image_is_loader(const uint8_t *head, uint64_t len)
{
	if (len < 48 || memcmp(head, "ANDROID!", 8) != 0) {
		return 0;
	}
	uint32_t page = *(const uint32_t *)(head + 36);
	uint32_t version = *(const uint32_t *)(head + 40);
	if (version != 1 || page < 2048 || page + 2 > len) {
		return 0;
	}
	return head[page] == 0x1f && head[page + 1] == 0x8b;
}

/*
 * Exactly one boot partition holding the loader names both slots: the loader's, and Android's as
 * the other one. Otherwise both stay 0 and nothing may touch the slots: with neither (started from
 * RAM) the active slot is Android's and a plain reboot reaches it; with both there is no Android.
 */
static void
slot_choose(int loader_in_a, int loader_in_b, char *loader, char *android)
{
	if (loader_in_a && !loader_in_b) {
		*loader = 'a';
		*android = 'b';
	} else if (loader_in_b && !loader_in_a) {
		*loader = 'b';
		*android = 'a';
	} else {
		*loader = 0;
		*android = 0;
	}
}

#endif
