#!/usr/bin/env python3
"""Plan darwin expansion from current saved LUN 0 GPT metadata.

HEAD starts at disk byte zero; TAIL ends at the actual disk size. Both must
contain their complete GPT header and entry array. Obtain these backups from
the current device; an offline parser cannot establish their capture time.
Optional stopped-filesystem RDWFS metadata supplies exact var copy ranges.
By default only JSON is emitted. --output-dir creates a new host directory
with candidate GPT copies and the plan; existing output paths are refused.
JSON output can contain device GUIDs and belongs in local ignored storage.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import struct
import sys
import uuid
import zlib


APFS_TYPE = uuid.UUID("7c3457ef-0000-11aa-aa11-00306543ecac")
HEADER = struct.Struct("<8sIIIIQQQQ16sQIII")
MAX_BACKUP_BYTES = 16 << 20

spec = importlib.util.spec_from_file_location("rdwfs_inspect", Path(__file__).with_name("rdwfs-inspect.py"))
rdwfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rdwfs)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_backup(path):
    path = Path(path)
    info = path.stat()
    require(stat.S_ISREG(info.st_mode), "GPT input must be a regular host file")
    require(info.st_size <= MAX_BACKUP_BYTES, "GPT metadata backup exceeds 16 MiB input limit")
    return path.read_bytes()


def parse_gpt(blob, disk_bytes, sector_bytes, primary):
    label = "primary" if primary else "backup"
    require(len(blob) % sector_bytes == 0 and len(blob) >= 2 * sector_bytes,
            f"{label} backup length must contain aligned GPT metadata")
    require(len(blob) <= disk_bytes, f"{label} backup exceeds disk size")
    base = 0 if primary else disk_bytes - len(blob)
    at = sector_bytes if primary else len(blob) - sector_bytes
    block = blob[at:at + sector_bytes]
    (magic, revision, size, crc, reserved, current, alternate, first, last,
     disk_guid, entries_lba, count, entry_size, entries_crc) = HEADER.unpack_from(block)
    require(magic == b"EFI PART", f"{label} GPT signature mismatch")
    require(revision == 0x10000 and reserved == 0, f"{label} GPT revision or reserved field invalid")
    require(HEADER.size <= size <= sector_bytes, f"{label} GPT header size invalid")
    header = bytearray(block[:size])
    struct.pack_into("<I", header, 16, 0)
    require(zlib.crc32(header) == crc, f"{label} GPT header CRC mismatch")
    blocks = disk_bytes // sector_bytes
    expected_current, expected_alternate = (1, blocks - 1) if primary else (blocks - 1, 1)
    require((current, alternate) == (expected_current, expected_alternate),
            f"{label} GPT location does not match actual disk size")
    require(2 <= first <= last < blocks - 1, f"{label} GPT usable LBA bounds invalid")
    require(any(disk_guid), f"{label} GPT disk GUID is zero")
    require(count > 0 and entry_size >= 128 and entry_size & (entry_size - 1) == 0,
            f"{label} GPT entry layout invalid")
    table_bytes = count * entry_size
    table_blocks = (table_bytes + sector_bytes - 1) // sector_bytes
    if primary:
        require(2 <= entries_lba and entries_lba + table_blocks <= first,
                "primary GPT entry array overlaps a header or usable space")
    else:
        require(last < entries_lba and entries_lba + table_blocks <= current,
                "backup GPT entry array overlaps a header or usable space")
    table_at = entries_lba * sector_bytes - base
    require(0 <= table_at and table_at + table_bytes <= len(blob),
            f"{label} GPT entry array is truncated in backup")
    table = bytes(blob[table_at:table_at + table_bytes])
    require(zlib.crc32(table) == entries_crc, f"{label} GPT entry array CRC mismatch")
    # Location fields and header CRC differ between the two copies.
    header[24:40] = bytes(16)
    header[72:80] = bytes(8)
    return {
        "normalized_header": bytes(header), "table": table,
        "first_usable": first, "last_usable": last,
        "disk_guid": str(uuid.UUID(bytes_le=disk_guid)),
        "entry_size": entry_size, "count": count,
        "entries_lba": entries_lba,
    }


def partitions(gpt):
    found = []
    guids = set()
    for index in range(gpt["count"]):
        entry = gpt["table"][index * gpt["entry_size"]:(index + 1) * gpt["entry_size"]]
        kind = uuid.UUID(bytes_le=entry[:16])
        if kind.int == 0:
            continue
        guid = uuid.UUID(bytes_le=entry[16:32])
        first, last, attrs = struct.unpack_from("<QQQ", entry, 32)
        require(guid.int != 0 and guid not in guids, "partition GUID is zero or duplicated")
        guids.add(guid)
        require(gpt["first_usable"] <= first <= last <= gpt["last_usable"],
                f"partition entry {index + 1} exceeds usable bounds")
        try:
            name = entry[56:128].decode("utf-16le").split("\0", 1)[0]
        except UnicodeDecodeError as error:
            raise ValueError(f"partition entry {index + 1} has invalid UTF-16 name") from error
        found.append({"entry": index + 1, "name": name, "type_guid": str(kind),
                      "guid": str(guid), "first_lba": first, "last_lba": last, "attrs": attrs})
    ordered = sorted(found, key=lambda entry: entry["first_lba"])
    for left, right in zip(ordered, ordered[1:]):
        require(left["last_lba"] < right["first_lba"], "partition entries overlap")
    return found


def copies_overlap_sources(transfer):
    """Both offset sequences have the same ascending partition-relative order."""
    source = 0
    for target in transfer:
        while source < len(transfer) and transfer[source]["source_offset"] + transfer[source]["length"] <= target["target_offset"]:
            source += 1
        if source < len(transfer) and transfer[source]["source_offset"] < target["target_offset"] + target["length"]:
            return True
    return False


def candidate_copy(blob, parsed, table, disk_bytes, sector_bytes, primary):
    result = bytearray(blob)
    base = 0 if primary else disk_bytes - len(blob)
    at = sector_bytes if primary else len(blob) - sector_bytes
    table_at = parsed["entries_lba"] * sector_bytes - base
    result[table_at:table_at + len(table)] = table
    size = struct.unpack_from("<I", result, at + 12)[0]
    struct.pack_into("<I", result, at + 88, zlib.crc32(table))
    struct.pack_into("<I", result, at + 16, 0)
    struct.pack_into("<I", result, at + 16, zlib.crc32(result[at:at + size]))
    return bytes(result)


def write_candidates(directory, head, tail, report):
    directory = Path(directory)
    # mkdir without exist_ok refuses existing directories, files and symlinks.
    directory.mkdir(mode=0o700)
    report["candidate_files"] = {"head": "gpt-head.new.bin", "tail": "gpt-tail.new.bin", "plan": "plan.json"}
    report["candidate_sha256"] = {"head": digest(head), "tail": digest(tail)}
    for name, data in (("head", head), ("tail", tail)):
        with (directory / report["candidate_files"][name]).open("xb") as output:
            output.write(data)
    with (directory / report["candidate_files"]["plan"]).open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")


def plan(head, tail, disk_bytes, sector_bytes, root_bytes=16 << 30, var_metadata=None, output_dir=None):
    require(sector_bytes in (512, 4096), "sector size must be 512 or 4096 bytes")
    require(disk_bytes > 0 and disk_bytes % sector_bytes == 0, "disk size must be sector aligned")
    require(root_bytes > 0 and root_bytes % sector_bytes == 0, "target darwin size must be sector aligned")
    primary = parse_gpt(head, disk_bytes, sector_bytes, True)
    backup = parse_gpt(tail, disk_bytes, sector_bytes, False)
    require(primary["normalized_header"] == backup["normalized_header"],
            "primary and backup GPT headers disagree")
    require(primary["table"] == backup["table"], "primary and backup GPT entry arrays disagree")
    entries = partitions(primary)

    def named(name):
        matches = [entry for entry in entries if entry["name"] == name]
        require(len(matches) == 1, f"expected exactly one {name} partition")
        return matches[0]

    root, var, userdata = named("darwin"), named("darwin_var"), named("userdata")
    require(root["type_guid"] == var["type_guid"] == str(APFS_TYPE), "iOS partition type is not APFS")
    require(root["last_lba"] + 1 == var["first_lba"], "darwin and darwin_var are not adjacent")
    require(userdata["last_lba"] < root["first_lba"], "userdata must precede the iOS partitions")
    old_root_bytes = (root["last_lba"] - root["first_lba"] + 1) * sector_bytes
    require(root_bytes >= old_root_bytes, "target darwin size would shrink the system partition")
    new_var_first = root["first_lba"] + root_bytes // sector_bytes
    require(new_var_first <= var["last_lba"], "target darwin size exhausts darwin_var")
    old_var_bytes = (var["last_lba"] - var["first_lba"] + 1) * sector_bytes
    new_var_bytes = (var["last_lba"] - new_var_first + 1) * sector_bytes
    rdwfs.check_capacity(new_var_bytes, "target darwin_var capacity")
    new_root = dict(root, last_lba=new_var_first - 1)
    new_var = dict(var, first_lba=new_var_first)
    changed = [entry["entry"] for entry, new in ((root, new_root), (var, new_var)) if entry != new]
    new_table = bytearray(primary["table"])
    struct.pack_into("<Q", new_table, (root["entry"] - 1) * primary["entry_size"] + 40, new_root["last_lba"])
    struct.pack_into("<Q", new_table, (var["entry"] - 1) * primary["entry_size"] + 32, new_var_first)

    var_report = None
    if var_metadata is not None:
        var_report = rdwfs.inspect(var_metadata, old_var_bytes, new_var_bytes)
    old_var_offset = var["first_lba"] * sector_bytes
    new_var_offset = new_var_first * sector_bytes
    var_ranges = None if var_report is None or var_report["status"] == "invalid" else var_report["backup_ranges"]
    transfer = None if var_report is None or not var_report["target_fits"] else [
        {"source_offset": old_var_offset + item["offset"],
         "target_offset": new_var_offset + item["offset"], "length": item["length"]}
        for item in var_ranges
    ]
    report = {
        "status": "planned", "disk_bytes": disk_bytes, "sector_bytes": sector_bytes,
        "disk_guid": primary["disk_guid"],
        "source_sha256": {"head": digest(head), "tail": digest(tail), "entry_array": digest(primary["table"])},
        "proposed_entry_array_sha256": digest(new_table),
        "changed_entries": changed,
        "darwin": {"before": root, "after": new_root, "before_bytes": old_root_bytes, "after_bytes": root_bytes},
        "darwin_var": {"before": var, "after": new_var, "before_bytes": old_var_bytes, "after_bytes": new_var_bytes},
        "userdata_unchanged": userdata,
        "all_other_entry_bytes_unchanged": True,
        "data_fit_checked": var_report is not None,
        "data_fits": None if var_report is None else var_report["target_fits"],
        "var_metadata_sha256": None if var_report is None else var_report["metadata_sha256"],
        "var_metadata_status": None if var_report is None else var_report["status"],
        "var_minimum_bytes": None if var_report is None else var_report["minimum_partition_bytes"],
        "var_transfer_ranges": transfer,
        "rollback": {
            "gpt_ranges": [{"offset": 0, "length": len(head)},
                           {"offset": disk_bytes - len(tail), "length": len(tail)}],
            "old_system_range": {"offset": root["first_lba"] * sector_bytes, "length": old_root_bytes},
            "old_var_ranges": None if var_ranges is None else [
                {"offset": old_var_offset + item["offset"], "length": item["length"]} for item in var_ranges
            ],
            "backup_required_before_destination_writes": True,
            "var_copy_overlaps_old_referenced_ranges": None if transfer is None else copies_overlap_sources(transfer),
            "gpt_only_rollback_after_system_write": False,
        },
    }
    if output_dir is not None:
        require(report["data_fits"] is not False, "RDWFS metadata does not fit the proposed partition")
        new_head = candidate_copy(head, primary, new_table, disk_bytes, sector_bytes, True)
        new_tail = candidate_copy(tail, backup, new_table, disk_bytes, sector_bytes, False)
        checked = plan(new_head, new_tail, disk_bytes, sector_bytes, root_bytes)
        require(checked["changed_entries"] == [], "generated GPT does not match requested capacity")
        write_candidates(output_dir, new_head, new_tail, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("head")
    parser.add_argument("tail")
    parser.add_argument("--disk-bytes", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--sector-bytes", type=int, required=True)
    parser.add_argument("--root-bytes", type=lambda value: int(value, 0), default=16 << 30)
    parser.add_argument("--var-metadata", help="saved complete 64 MiB RDWFS metadata")
    parser.add_argument("--output-dir", help="new host directory for candidate GPT copies and plan JSON")
    args = parser.parse_args(argv)
    try:
        metadata = None if args.var_metadata is None else rdwfs.read_metadata(args.var_metadata)
        report = plan(read_backup(args.head), read_backup(args.tail), args.disk_bytes,
                      args.sector_bytes, args.root_bytes, metadata, args.output_dir)
    except (OSError, ValueError) as error:
        print(json.dumps({"status": "error", "error": str(error)}))
        return 2
    print(json.dumps(report, indent=2))
    return 2 if report["data_fits"] is False else 0


if __name__ == "__main__":
    sys.exit(main())
