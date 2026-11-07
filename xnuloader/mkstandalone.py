#!/usr/bin/env python3
"""Build a standalone payload with the small boot volume and helper root (LZ4 compressed).

The resident boot volume is at most 64 MiB. The iOS system volume remains on UFS and is mounted
by init after the kernel starts from the boot volume.
"""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys

MAX_RESIDENT_BOOT_BYTES = 64 * 1024 * 1024
DEFAULT_IDENTITY = "v57"
DEFAULT_MEMSIZE = "300000000"
DEFAULT_CMDLINE = (
    "debug=0x10e rd=md0 rootdev=md0 root_apfs=ufs_darwin launchd_unsecure_cache=1 cpus=4 bootcpu=7 "
    "small_cores_after=0 adreno8=23 adreno8_pause=0 adreno8_fine=0 adreno8_watch=0 "
    "adreno8_keep=1 adreno8_return=0 adreno8_speed=14 adreno8_secure_preflight=4 "
    "touchscreen=2 buttons=1 battery=1 gpu_log=0 metal=1 cpufreq=1 fb_threads=2 "
    "touch_lat=0 gpu_coherent=1 fb_flip=2 fb_beat=0 fb_hz=120 fb_ca_hz=120 dl_max=1 "
    "touch_poll_us=4000 pcie=10 pcie_power=1 pcie_sleep_clock=board pcie_bdf=bd_p11.elf "
    "pcie_smps_hpm=1 pcie_bt_en=1 pcie_aop_pdc=1 pcie_rddm=1 wifi_mlo=1 "
    "wifi_mac=02:4b:39:30:00:01 wifi_station=1 ufs=2 ufs_native=1 ufs_write=1 "
    "bt=4 cam=5 torch=1 fbwidth=1206 fbheight=2622 fbpitch=460"
)


def find_default_kernel(root_dir: Path) -> Path:
    candidates = [
        root_dir / "out/xnu12377-cmd/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837",
        root_dir / "out/xnu12377-st4.bin",
        root_dir / "out/ios26-resume/k90-ready/inputs/kernel26",
        root_dir / "out/ios26-resume/k90-ready/sources/kernel",
        root_dir / "out/ios26-resume/input26-formal-inputs/kernel26",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return candidates[0]


def find_default_ramdisk(root_dir: Path) -> Path:
    candidates = [
        root_dir / "out/ios26-resume/bbmetal-device/root.apfs",
        root_dir / "userland/root26.apfs",
        root_dir / "out/ios26-resume/k90-ready/inputs/root26.apfs",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return candidates[0]


def find_default_boot_image(root_dir: Path) -> Path:
    return root_dir / "userland/flashboot.apfs"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1048576):
            h.update(chunk)
    return h.hexdigest()


def compress_lz4(input_path: Path, output_path: Path) -> Path:
    lz4_bin = shutil.which("lz4")
    if lz4_bin:
        cmd = [lz4_bin, "-f", "-9", str(input_path), str(output_path)]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"lz4 compression failed: {res.stderr}")
    else:
        # Fallback pure-python basic lz4 frame or subprocess python -m
        raise RuntimeError("lz4 command not found in PATH")
    return output_path


def build_standalone_payload(
    kernel: Path,
    ramdisk: Path,
    output: Path,
    lz4_output: Path | None = None,
    boot_image: Path | None = None,
    identity: str = DEFAULT_IDENTITY,
    memsize: str = DEFAULT_MEMSIZE,
    cmdline: str = DEFAULT_CMDLINE,
) -> tuple[Path, Path]:
    here = Path(__file__).resolve().parent
    mkpayload_py = here / "mkpayload.py"
    if not mkpayload_py.is_file():
        raise FileNotFoundError(f"mkpayload.py not found at {mkpayload_py}")

    if not kernel.is_file():
        raise FileNotFoundError(f"kernel not found at {kernel}")
    if not ramdisk.is_file():
        raise FileNotFoundError(f"ramdisk not found at {ramdisk}")
    if boot_image is None:
        boot_image = find_default_boot_image(here.parent)
    if not boot_image.is_file():
        raise FileNotFoundError(f"boot image not found at {boot_image}; build it with FLASHBOOT=1 CRYPTEXES=flash")
    if not 0 < boot_image.stat().st_size <= MAX_RESIDENT_BOOT_BYTES:
        raise ValueError(f"boot image is {boot_image.stat().st_size} bytes; the limit is {MAX_RESIDENT_BOOT_BYTES}")
    for key, value in (("rd", "md0"), ("rootdev", "md0"), ("root_apfs", "ufs_darwin"),
                       ("ufs", "2"), ("ufs_native", "1")):
        if [arg for arg in cmdline.split() if arg.split("=", 1)[0] == key] != [f"{key}={value}"]:
            raise ValueError(f"cmdline requires exactly one {key}={value}")

    output.parent.mkdir(parents=True, exist_ok=True)
    if lz4_output is None:
        lz4_output = output.with_name(output.name + ".lz4")
    lz4_output.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable,
        str(mkpayload_py),
        "-o",
        str(output),
    ]
    loader_efi = here / "loader-phone.efi"
    if loader_efi.is_file():
        cmd.append(f"loader.efi={loader_efi}")
    cmd.extend([
        f"kernel={kernel}",
        f"ramdisk={ramdisk}",
        f"rdimage={boot_image}",
        "--require",
        f"rdimage={sha256_file(boot_image)}",
        "--forbid-entry",
        "rdfile",
        "--forbid-entry",
        "rdres",
        "--require-cmd",
        "rd=md0",
        "--require-cmd",
        "rootdev=md0",
        "--require-cmd",
        "root_apfs=ufs_darwin",
        "--identity",
        identity,
        "--memsize",
        memsize,
        "--cmdline",
        cmdline,
    ])

    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"mkpayload.py failed:\n{res.stderr}\n{res.stdout}")

    compress_lz4(output, lz4_output)

    return output, lz4_output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    here = Path(__file__).resolve().parent
    kernel_root = here.parent

    parser.add_argument("-k", "--kernel", type=Path, default=find_default_kernel(kernel_root),
                        help="Path to kernel Mach-O")
    parser.add_argument("-r", "--ramdisk", type=Path, default=find_default_ramdisk(kernel_root),
                        help="Path to root ramdisk (root26.apfs)")
    parser.add_argument("-b", "--boot-image", type=Path, default=find_default_boot_image(kernel_root),
                        help="Path to the small resident boot APFS image")
    parser.add_argument("-o", "--output", type=Path,
                        default=kernel_root / "out/standalone-payload.bin",
                        help="Path to output uncompressed payload")
    parser.add_argument("--lz4-output", type=Path, default=None,
                        help="Path to output LZ4 compressed payload")
    parser.add_argument("--identity", default=DEFAULT_IDENTITY,
                        help="identity (v57 for iOS 26)")
    parser.add_argument("--memsize", default=DEFAULT_MEMSIZE,
                        help="memsize in hex")
    parser.add_argument("--cmdline", default=DEFAULT_CMDLINE,
                        help="boot arguments cmdline")
    parser.add_argument("--test", action="store_true",
                        help="Run self-test with dummy kernel/ramdisk")

    args = parser.parse_args()

    if args.test:
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            dummy_k = tmp / "kernel"
            dummy_r = tmp / "root.apfs"
            dummy_b = tmp / "flashboot.apfs"
            dummy_out = tmp / "payload.bin"
            dummy_lz4 = tmp / "payload.bin.lz4"

            # Create dummy Mach-O and apfs bytes
            dummy_k.write_bytes(b"\xcf\xfa\xed\xfe" + b"\x00" * 1024)
            dummy_r.write_bytes(b"NXSB" + b"\x00" * 1024)
            dummy_b.write_bytes(b"NXSB" + b"\x00" * 1024)

            out_bin, out_lz = build_standalone_payload(
                kernel=dummy_k,
                ramdisk=dummy_r,
                output=dummy_out,
                lz4_output=dummy_lz4,
                boot_image=dummy_b,
            )
            assert out_bin.is_file() and out_bin.stat().st_size > 0
            assert out_lz.is_file() and out_lz.stat().st_size > 0
            print("mkstandalone self-test: PASS")
            return 0

    out_bin, out_lz = build_standalone_payload(
        kernel=args.kernel,
        ramdisk=args.ramdisk,
        output=args.output,
        lz4_output=args.lz4_output,
        boot_image=args.boot_image,
        identity=args.identity,
        memsize=args.memsize,
        cmdline=args.cmdline,
    )

    bin_size = out_bin.stat().st_size
    lz_size = out_lz.stat().st_size
    bin_sha = sha256_file(out_bin)
    lz_sha = sha256_file(out_lz)

    print(f"Standalone payload generated:")
    print(f"  Raw: {out_bin} ({bin_size} bytes, {bin_size / 1048576:.2f} MiB), sha256: {bin_sha}")
    print(f"  LZ4: {out_lz} ({lz_size} bytes, {lz_size / 1048576:.2f} MiB), sha256: {lz_sha}")
    print(f"  Target partition: boot_a (96 MiB). Payload fits within limit: {lz_size < 96 * 1048576}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
