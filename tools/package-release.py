#!/usr/bin/env python3
"""Assemble a version-matched firmware and SD distribution from verified build inputs."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def sha(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(relative):
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or "\\" in relative:
        raise ValueError(f"Unsafe flash path {relative!r}")
    return path


def package(source, sdk, pack_directory, reference, output):
    root = Path(__file__).resolve().parents[1]
    plan = json.loads((source / "flasher_args.json").read_text())
    if plan["extra_esptool_args"]["chip"] != "esp32p4":
        raise ValueError("The build image must target ESP32-P4.")
    if set(plan["flash_files"]) != {"0x2000", "0x8000", "0x10000"}:
        raise ValueError("Unexpected Guition flash layout.")
    config = (source / "sdkconfig").read_bytes()
    if (b"CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y" not in config or b"CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y" not in config
            or b"CONFIG_FOD_BOARD_DIAGNOSTIC=y" in config or b'CONFIG_FOD_REPLAY_SCENE=""' not in config):
        raise ValueError("Use the pre-v3 16 MB game configuration without a diagnostic or replay override.")

    files = {"firmware/sdkconfig": config}
    flash_arguments = list(plan["write_flash_args"])
    for offset, relative in sorted(plan["flash_files"].items(), key=lambda pair: int(pair[0], 16)):
        path = safe_path(relative)
        files[f"firmware/{path}"] = source.joinpath(*path.parts).read_bytes()
        flash_arguments.extend([offset, str(path)])
    files["firmware/flash_args"] = (" ".join(flash_arguments) + "\n").encode()
    files["firmware/flasher_args.json"] = (source / "flasher_args.json").read_bytes()
    files["firmware/dependencies.lock"] = (root / "firmware/dependencies.lock").read_bytes()

    pack = pack_directory / "fall-of-duty.fodp"
    manifest_file = pack_directory / "fall-of-duty.fodp.json"
    asset_bytes = pack.read_bytes()
    manifest = json.loads(manifest_file.read_text())
    if manifest["sha256"] != sha(asset_bytes) or manifest["storedBytes"] != len(asset_bytes):
        raise ValueError("The SD pack and its manifest disagree.")
    files["sd/fall-of-duty.fodp"] = asset_bytes
    files["sd/fall-of-duty.fodp.json"] = manifest_file.read_bytes()
    for trace in sorted(reference.glob("*.fodtrace")):
        files[f"sd/{trace.name}"] = trace.read_bytes()
    if not all(f"sd/{name}.fodtrace" in files for name in ("long-lane", "apache")):
        raise ValueError("Capture the long-lane and Apache traces before packaging.")

    files["INSTALL.md"] = (root / "INSTALL.md").read_bytes()
    files["LICENSES/PROJECT-MIT.txt"] = (root / "LICENSE").read_bytes()
    files["LICENSES/ASSETS.md"] = (root / "THIRD_PARTY_NOTICES.md").read_bytes()
    files["LICENSES/GUITION-BSP-MIT.txt"] = (root / "firmware/GUITION-BSP-LICENSE").read_bytes()
    files["LICENSES/ESP-IDF-APACHE-2.0.txt"] = (sdk / "LICENSE").read_bytes()
    files["LICENSES/FREERTOS-MIT.md"] = (sdk / "components/freertos/FreeRTOS-Kernel/LICENSE.md").read_bytes()
    codec = root / "firmware/managed_components/espressif__esp_codec_dev/LICENSE"
    files["LICENSES/ESP-CODEC-DEV.txt"] = codec.read_bytes()
    for name in ["PROJECT-MIT.txt", "ASSETS.md"]:
        files[f"sd/{name}"] = files[f"LICENSES/{name}"]
    lines = [f"{sha(data)}  {path}" for path, data in sorted(files.items())]
    files["SHA256SUMS"] = ("\n".join(lines) + "\n").encode()

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".tmp")
    try:
        with ZipFile(temporary, "w", compression=ZIP_DEFLATED, compresslevel=9) as archive:
            for path, data in sorted(files.items()):
                member = ZipInfo(path, (2020, 1, 1, 0, 0, 0))
                member.external_attr = 0o100644 << 16
                archive.writestr(member, data, compress_type=ZIP_DEFLATED, compresslevel=9)
        with ZipFile(temporary) as archive:
            for path, data in files.items():
                if sha(archive.read(path)) != sha(data):
                    raise ValueError(f"Zip verification failed for {path}.")
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)

    digest = sha(output.read_bytes())
    output.with_suffix(output.suffix + ".sha256").write_text(f"{digest}  {output.name}\n")
    print(f"{output}: {output.stat().st_size:,} bytes, SHA-256 {digest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", required=True, type=Path, help="ESP-IDF build directory")
    parser.add_argument("--idf", required=True, type=Path, help="Pinned ESP-IDF checkout containing its license")
    parser.add_argument("--sd", required=True, type=Path, help="Pack and JSON manifest directory")
    parser.add_argument("--reference", required=True, type=Path, help="Captured browser replay inputs")
    parser.add_argument("--out", required=True, type=Path, help="Output release zip")
    args = parser.parse_args()
    package(args.firmware, args.idf, args.sd, args.reference, args.out)


if __name__ == "__main__":
    main()
