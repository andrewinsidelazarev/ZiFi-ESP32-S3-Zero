"""PlatformIO post-build packaging for Waveshare ESP32-S3-Zero.

Produces an application image for OTA and a merged factory image
that can be written at address 0x0 through the native USB bootloader.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # Provided by PlatformIO/SCons.


PROJECT = Path(env.subst("$PROJECT_DIR"))
OUTPUT = PROJECT / "firmware"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def package_firmware(source, target, env) -> None:
    build_dir = Path(env.subst("$BUILD_DIR"))
    # AddPostAction получает firmware.bin как target, а firmware.elf как source.
    # ELF нельзя ни публиковать под именем .bin, ни передавать merge_bin.
    app_source = Path(target[0].get_abspath())
    platform = env.PioPlatform()
    framework_dir = Path(platform.get_package_dir("framework-arduinoespressif32"))
    esptool_dir = Path(platform.get_package_dir("tool-esptoolpy"))

    bootloader = build_dir / "bootloader.bin"
    partitions = build_dir / "partitions.bin"
    boot_app0 = framework_dir / "tools" / "partitions" / "boot_app0.bin"
    esptool = esptool_dir / "esptool.py"
    required = (app_source, bootloader, partitions, boot_app0, esptool)
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError("Firmware packaging inputs are missing: " + ", ".join(missing))

    OUTPUT.mkdir(parents=True, exist_ok=True)
    app_output = OUTPUT / "firmware.bin"
    factory_output = OUTPUT / "firmware.factory.bin"
    shutil.copy2(app_source, app_output)
    if factory_output.exists():
        factory_output.unlink()

    command = [
        env.subst("$PYTHONEXE"),
        str(esptool),
        "--chip", "esp32s3",
        "merge_bin",
        "-o", str(factory_output),
        "--flash_mode", "qio",
        "--flash_freq", "80m",
        "--flash_size", "4MB",
        "0x0", str(bootloader),
        "0x8000", str(partitions),
        "0xE000", str(boot_app0),
        "0x10000", str(app_source),
    ]
    subprocess.run(command, check=True)
    if factory_output.stat().st_size > 4 * 1024 * 1024:
        factory_output.unlink()
        raise RuntimeError("Merged factory image exceeds the 4 MB flash size")

    hashes = OUTPUT / "firmware.sha256"
    hashes.write_text(
        f"{sha256(app_output)}  firmware.bin\n"
        f"{sha256(factory_output)}  firmware.factory.bin\n",
        encoding="ascii",
        newline="\n",
    )
    (OUTPUT / "flash_args.txt").write_text(
        "0x0 firmware.factory.bin\n",
        encoding="ascii",
        newline="\n",
    )
    print(f"Packaged: {app_output}")
    print(f"Packaged: {factory_output}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", package_firmware)
