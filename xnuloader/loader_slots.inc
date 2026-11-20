static uint32_t
gpt_swap_ab_type_guids(uint8_t *entries, uint32_t num_entries, uint32_t entry_size, char target_slot)
{
	/* Qualcomm inactive slot PartitionTypeGUID: 77036cd4-03d5-42bb-8ed1-37e5a88baa34 */
	static const uint8_t inactive_guid[16] = {
		0xd4, 0x6c, 0x03, 0x77, 0xd5, 0x03, 0xbb, 0x42,
		0x8e, 0xd1, 0x37, 0xe5, 0xa8, 0x8b, 0xaa, 0x34
	};
	uint32_t swapped = 0;
	for (uint32_t i = 0; i < num_entries; i++) {
		uint8_t *ei = entries + i * entry_size;
		const uint8_t *pi = ei + 56;
		uint32_t len = 0;
		while (len < 35 && (pi[len * 2] != 0 || pi[len * 2 + 1] != 0)) len++;
		if (len < 3 || pi[(len - 2) * 2] != '_' || pi[(len - 2) * 2 + 1] != 0 ||
		    pi[(len - 1) * 2] != 'a' || pi[(len - 1) * 2 + 1] != 0) {
			continue;
		}
		for (uint32_t j = 0; j < num_entries; j++) {
			uint8_t *ej = entries + j * entry_size;
			const uint8_t *pj = ej + 56;
			if (pj[len * 2] != 0 || pj[len * 2 + 1] != 0 ||
			    pj[(len - 2) * 2] != '_' || pj[(len - 2) * 2 + 1] != 0 ||
			    pj[(len - 1) * 2] != 'b' || pj[(len - 1) * 2 + 1] != 0) {
				continue;
			}
			if (memcmp(pi, pj, (len - 2) * 2) == 0) {
				uint8_t *et = (target_slot == 'b') ? ej : ei;
				uint8_t *eo = (target_slot == 'b') ? ei : ej;
				/* Only swap A/B pairs where the target slot currently holds the inactive GUID. */
				if (memcmp(et, inactive_guid, 16) == 0 && memcmp(eo, inactive_guid, 16) != 0) {
					uint8_t tmp[16];
					memcpy(tmp, ei, 16);
					memcpy(ei, ej, 16);
					memcpy(ej, tmp, 16);
					swapped++;
					int is_boot = (len == 6 && pi[0] == 'b' && pi[2] == 'o' && pi[4] == 'o' && pi[6] == 't');
					uint64_t keep_t = *(uint64_t *)(et + 48) & 0x1000000000000001ULL;
					uint64_t keep_o = *(uint64_t *)(eo + 48) & 0x1000000000000001ULL;
					*(uint64_t *)(et + 48) = is_boot ? 0x006f000000000000ULL : (keep_t | 0x007f000000000000ULL);
					*(uint64_t *)(eo + 48) = is_boot ? 0x006a000000000000ULL : (keep_o | 0x00c4000000000000ULL);
				}
				break;
			}
		}
	}
	return swapped;
}

static int
gpt_write_slot_entries(efi_block_io *bio, uint8_t *header, uint64_t num_blocks,
    uint32_t entries_crc)
{
	uint32_t bs = bio->media->block_size;
	uint32_t media_id = bio->media->media_id;
	uint32_t hdr_size = *(uint32_t *)(header + 12);
	uint64_t entries_lba = *(uint64_t *)(header + 72);
	uint64_t backup_lba = *(uint64_t *)(header + 32);
	uint64_t backup_entries_lba = backup_lba - num_blocks;
	static uint8_t backup[4096] __attribute__((aligned(4096)));

	*(uint32_t *)(header + 88) = entries_crc;
	*(uint32_t *)(header + 16) = 0;
	*(uint32_t *)(header + 16) = ufs_crc32(header, hdr_size);
	memcpy(backup, header, bs);
	*(uint64_t *)(backup + 24) = backup_lba;
	*(uint64_t *)(backup + 32) = 1;
	*(uint64_t *)(backup + 72) = backup_entries_lba;
	*(uint32_t *)(backup + 16) = 0;
	*(uint32_t *)(backup + 16) = ufs_crc32(backup, hdr_size);

	/* A retry uses the primary entries and their CRC to detect an interrupted update. */
	return !EFI_ERROR(safe_write_blocks(bio, backup_entries_lba, num_blocks, bs, ufs_buffer)) &&
	    !EFI_ERROR(bio->write_blocks(bio, media_id, backup_lba, bs, backup)) &&
	    !EFI_ERROR(bio->flush_blocks(bio)) &&
	    !EFI_ERROR(safe_write_blocks(bio, entries_lba, num_blocks, bs, ufs_buffer)) &&
	    !EFI_ERROR(bio->write_blocks(bio, media_id, 1, bs, header));
}

static int
gpt_apply_slot(efi_block_io *bio, char target_slot, int has_boot)
{
	static uint8_t header[4096] __attribute__((aligned(4096)));
	if (!bio || !bio->media) return 0;
	uint32_t bs = bio->media->block_size;
	if (bs < 512 || bs > sizeof(header) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, header)) ||
	    memcmp(header, "EFI PART", 8) != 0) return 0;

	uint32_t header_size = *(uint32_t *)(header + 12);
	uint64_t backup_lba = *(uint64_t *)(header + 32);
	uint64_t first_usable = *(uint64_t *)(header + 40);
	uint64_t last_usable = *(uint64_t *)(header + 48);
	uint64_t entries_lba = *(uint64_t *)(header + 72);
	uint32_t num_entries = *(uint32_t *)(header + 80);
	uint32_t entry_size = *(uint32_t *)(header + 84);
	uint64_t total_bytes = (uint64_t)num_entries * entry_size;
	if (header_size < 92 || header_size > bs || entry_size < 128 || entry_size % 8 ||
	    total_bytes == 0 || total_bytes > sizeof(ufs_buffer)) return 0;

	uint64_t num_blocks = (total_bytes + bs - 1) / bs;
	if (num_blocks * bs > sizeof(ufs_buffer) || entries_lba < 2 ||
	    entries_lba > first_usable || first_usable - entries_lba < num_blocks ||
	    last_usable < first_usable || backup_lba > bio->media->last_block ||
	    backup_lba <= last_usable || backup_lba - last_usable <= num_blocks ||
	    EFI_ERROR(safe_read_blocks(bio, entries_lba, num_blocks, bs, ufs_buffer))) return 0;

	uint32_t swapped = gpt_swap_ab_type_guids(ufs_buffer, num_entries, entry_size, target_slot);
	if (has_boot) {
		uint8_t *boot_a = gpt_find_entry(ufs_buffer, num_entries, entry_size, "boot_a");
		uint8_t *boot_b = gpt_find_entry(ufs_buffer, num_entries, entry_size, "boot_b");
		if (!boot_a || !boot_b) return 0;
		*(uint64_t *)(boot_a + 48) = target_slot == 'a' ? 0x006f000000000000ULL : 0x006a000000000000ULL;
		*(uint64_t *)(boot_b + 48) = target_slot == 'b' ? 0x006f000000000000ULL : 0x006a000000000000ULL;
	}

	uint32_t entries_crc = ufs_crc32(ufs_buffer, total_bytes);
	if (entries_crc != *(uint32_t *)(header + 88) &&
	    !gpt_write_slot_entries(bio, header, num_blocks, entries_crc)) return 0;
	if (EFI_ERROR(bio->flush_blocks(bio))) return 0;
	print("slot: LUN %d Slot %s GPT synchronized (swapped=%d)\n",
	    (uint64_t)(has_boot ? 4 : 0), target_slot == 'a' ? "a" : "b", (uint64_t)swapped);
	return 1;
}

static int
slot_apply_active(char target_slot)
{
	if (target_slot != 'a' && target_slot != 'b') return 0;
	return gpt_apply_slot(lun4_get_bio(), target_slot, 1) &&
	    gpt_apply_slot(lun_get_bio(0, "super"), target_slot, 0);
}

static void
slot_maintain_current(void)
{
	if (loader_slot && !slot_apply_active(loader_slot))
		print("slot: could not maintain loader GPT\n");
}
