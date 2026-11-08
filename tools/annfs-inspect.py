#!/usr/bin/env python3
"""Inspect saved ANNFS metadata and plan partition-relative backup ranges.

Input must be a regular host file containing both complete 32 MiB metadata
slots, captured while the filesystem is stopped. The output plans reads; it
does not acquire data pages or verify their contents. Preserve every returned
range at the same partition-relative offset in a sparse backup, and verify
the acquired ranges separately before changing a partition boundary.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import sys
import zlib


# Layout and acceptance rules: ramfs_disk.inc and ramfs_disk_restore.inc.
PAGE_SIZE = 16384
SECTOR_SIZE = 4096
SUPER_BYTES = 4096
SLOT_BYTES = 32 << 20
METADATA_BYTES = 2 * SLOT_BYTES
DATA_OFFSET = 512 << 20
MAX_BLOCKS = 8388608
MIN_PARTITION_BYTES = DATA_OFFSET + 2 * PAGE_SIZE
ROOT_INO = 2
UINT64_MAX = (1 << 64) - 1
SUPER = struct.Struct("<8sIIQQQIII")
INODE = struct.Struct("<QQQ14I8q")
XATTR = struct.Struct("<II")
DIRENT = struct.Struct("<QII")
DBLK = struct.Struct("<I")
VREG, VDIR, VLNK, VBAD = 1, 2, 5, 8


class InvalidMetadata(ValueError):
    """A checkpoint violates the production reader's format or bounds."""


def align8(size):
    return (size + 7) & ~7


def require(condition, message):
    if not condition:
        raise InvalidMetadata(message)


def check_capacity(size, label):
    if size < MIN_PARTITION_BYTES or size % SECTOR_SIZE:
        raise ValueError(
            f"{label} must be 4096-byte aligned and at least "
            f"{MIN_PARTITION_BYTES} bytes"
        )


def parse_slot(slot, max_blocks):
    """Return a complete checkpoint and its nonzero block references."""
    require(len(slot) == SLOT_BYTES, "truncated metadata slot")
    (magic, version, page_size, generation, payload_bytes, next_ino,
     node_count, payload_crc, super_crc) = SUPER.unpack_from(slot)
    require(magic == b"ANNFS001", "invalid magic")
    require(version == 3, "unsupported version")
    require(page_size == PAGE_SIZE, "invalid page size")
    require(0 < generation < UINT64_MAX, "invalid generation")
    require(node_count > 0, "missing inode records")
    require(payload_bytes <= SLOT_BYTES - SUPER_BYTES, "payload exceeds slot")
    require(node_count <= payload_bytes // INODE.size, "inode count exceeds payload")
    require(zlib.crc32(slot[:48]) == super_crc, "superblock CRC mismatch")
    payload = slot[SUPER_BYTES:SUPER_BYTES + payload_bytes]
    require(zlib.crc32(payload) == payload_crc, "payload CRC mismatch")
    at = 0
    nodes = {}
    parents = []
    children = []
    blocks = set()

    def take(size):
        nonlocal at
        require(at + size <= payload_bytes, "record exceeds payload")
        start = at
        at += size
        return start

    for _ in range(node_count):
        rec = INODE.unpack_from(payload, take(INODE.size))
        ino, size, parent_ino = rec[:3]
        kind = rec[3]
        nlink, target_len, nblocks, nxattrs, ndirents = rec[9:14]
        require(ROOT_INO <= ino < UINT64_MAX, "invalid inode number")
        require(0 < kind < VBAD and nlink > 0, "invalid inode type or link count")
        require(kind == VLNK or target_len == 0, "target on non-symlink")
        require(kind != VLNK or target_len == size, "symlink size mismatch")
        require(kind == VREG or nblocks == 0, "blocks on non-regular inode")
        require(kind == VDIR or ndirents == 0, "entries on non-directory")
        require(ino not in nodes, "duplicate inode number")
        require(ino != ROOT_INO or kind == VDIR, "root is not a directory")
        nodes[ino] = kind
        if kind == VDIR:
            parents.append(parent_ino)
        take(align8(target_len))
        block_at = take(align8(nblocks * DBLK.size))
        for (block,) in DBLK.iter_unpack(payload[block_at:block_at + nblocks * DBLK.size]):
            require(block < max_blocks, "data block exceeds source capacity")
            if block:
                blocks.add(block)
        for _ in range(nxattrs):
            namelen, value_size = XATTR.unpack_from(payload, take(XATTR.size))
            require(namelen <= 127 and value_size <= 16 << 20, "invalid xattr size")
            take(align8(namelen + value_size))
        for _ in range(ndirents):
            child, namelen, _ = DIRENT.unpack_from(payload, take(DIRENT.size))
            require(0 < namelen <= 255, "invalid directory entry name size")
            take(align8(namelen))
            children.append(child)

    require(ROOT_INO in nodes, "missing root inode")
    require(at == payload_bytes, "trailing payload bytes")
    require(all(nodes.get(parent) == VDIR for parent in parents), "invalid directory parent")
    require(all(child in nodes for child in children), "missing directory entry inode")
    return {
        "generation": generation,
        "node_count": node_count,
        "payload_bytes": payload_bytes,
        "next_ino": max(next_ino, max(nodes) + 1),
        "referenced_blocks": len(blocks),
    }, blocks


def data_ranges(blocks):
    """Coalesce adjacent physical pages without filling unreferenced gaps."""
    ranges = []
    for block in sorted(blocks):
        offset = DATA_OFFSET + block * PAGE_SIZE
        if ranges and ranges[-1]["offset"] + ranges[-1]["length"] == offset:
            ranges[-1]["length"] += PAGE_SIZE
        else:
            ranges.append({"offset": offset, "length": PAGE_SIZE})
    return ranges


def inspect(metadata, partition_bytes, target_bytes=None):
    """Plan reads only if a production-compatible checkpoint can be restored."""
    check_capacity(partition_bytes, "source capacity")
    if target_bytes is None:
        target_bytes = partition_bytes
    check_capacity(target_bytes, "target capacity")
    if len(metadata) != METADATA_BYTES:
        raise ValueError(f"metadata must contain exactly {METADATA_BYTES} bytes")
    metadata = memoryview(metadata)
    max_blocks = min((partition_bytes - DATA_OFFSET) // PAGE_SIZE, MAX_BLOCKS)
    slots = []
    all_blocks = set()
    valid_slots = []
    for index in range(2):
        slot = metadata[index * SLOT_BYTES:(index + 1) * SLOT_BYTES]
        summary = {"slot": index, "offset": index * SLOT_BYTES}
        try:
            parsed, blocks = parse_slot(slot, max_blocks)
        except InvalidMetadata as error:
            summary.update(status="invalid", error=str(error))
        else:
            summary.update(status="valid", **parsed)
            valid_slots.append(index)
            all_blocks.update(blocks)
        slots.append(summary)

    selected = None
    status = "checkpoint"
    if valid_slots:
        selected = max(valid_slots, key=lambda i: (slots[i]["generation"], -i))
    elif not any(metadata):
        status = "empty"
        for slot in slots:
            slot["status"] = "empty"
            del slot["error"]
    else:
        status = "invalid"

    minimum = max(MIN_PARTITION_BYTES,
                  DATA_OFFSET + (max(all_blocks, default=0) + 1) * PAGE_SIZE)
    ranges = data_ranges(all_blocks) if status != "invalid" else []
    backup = [{"offset": 0, "length": DATA_OFFSET}] + ranges if status != "invalid" else []
    return {
        "format": "ANNFS001",
        "status": status,
        "page_size": PAGE_SIZE,
        "metadata_sha256": hashlib.sha256(metadata).hexdigest(),
        "source_partition_bytes": partition_bytes,
        "target_partition_bytes": target_bytes,
        "selected_slot": selected,
        "valid_slots": valid_slots,
        "slots": slots,
        "minimum_partition_bytes": minimum if status != "invalid" else None,
        "target_fits": status != "invalid" and target_bytes >= minimum,
        "referenced_data_bytes": len(all_blocks) * PAGE_SIZE,
        "data_ranges": ranges,
        "backup_ranges": backup,
        "backup_bytes": sum(item["length"] for item in backup),
    }


def read_metadata(path):
    path = Path(path)
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("metadata input must be a regular host file")
    with path.open("rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ValueError("metadata input must be a regular host file")
        metadata = source.read(METADATA_BYTES)
    if len(metadata) != METADATA_BYTES:
        raise ValueError(f"metadata input is shorter than {METADATA_BYTES} bytes")
    return metadata


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", help="host file containing both 32 MiB metadata slots")
    parser.add_argument("--partition-bytes", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--target-bytes", type=lambda value: int(value, 0))
    args = parser.parse_args(argv)
    try:
        report = inspect(read_metadata(args.metadata), args.partition_bytes, args.target_bytes)
    except (OSError, ValueError) as error:
        print(json.dumps({"status": "error", "error": str(error)}))
        return 2
    print(json.dumps(report, indent=2))
    return 0 if report["target_fits"] else 2


if __name__ == "__main__":
    sys.exit(main())
