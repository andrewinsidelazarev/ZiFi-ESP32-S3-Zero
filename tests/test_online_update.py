import hashlib
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def parse_manifest(text: str, filename: str) -> str | None:
    for line in text.splitlines():
        match = re.fullmatch(r"\s*([0-9a-fA-F]{64})\s+\*?(\S+)\s*", line)
        if match and match.group(2) == filename:
            return match.group(1).lower()
    return None


def parse_version(text: str) -> str | None:
    versions = []
    for line in text.splitlines():
        match = re.fullmatch(r"\s*VERSION\s+([A-Za-z0-9._+-]+)\s*", line)
        if match:
            versions.append(match.group(1))
    return versions[0] if len(versions) == 1 else None


class OnlineUpdateTest(unittest.TestCase):
    def test_published_manifest_matches_local_application_image(self) -> None:
        manifest = (ROOT / "firmware" / "firmware.sha256").read_text(
            encoding="ascii"
        )
        firmware = (ROOT / "firmware" / "firmware.bin").read_bytes()
        self.assertEqual(
            parse_manifest(manifest, "firmware.bin"),
            hashlib.sha256(firmware).hexdigest(),
        )
        self.assertIsNotNone(parse_manifest(manifest, "firmware.factory.bin"))
        platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        configured = re.search(
            r"\[env:esp32s3_zero\][\s\S]*?custom_zifi_version\s*=\s*(\S+)",
            platformio,
        )
        self.assertIsNotNone(configured)
        self.assertEqual(parse_version(manifest), configured.group(1))

    def test_manifest_parser_selects_only_exact_firmware_name(self) -> None:
        wanted = "12" * 32
        factory = "34" * 32
        manifest = (
            "VERSION s3-native-9.8.7\n"
            f"{factory}  firmware.factory.bin\r\n"
            f"{wanted} *firmware.bin\n"
            f"{'56' * 32}  path/firmware.bin\n"
        )
        self.assertEqual(parse_manifest(manifest, "firmware.bin"), wanted)
        self.assertIsNone(parse_manifest(manifest, "FIRMWARE.BIN"))
        self.assertIsNone(parse_manifest("broken  firmware.bin", "firmware.bin"))
        self.assertEqual(parse_version(manifest), "s3-native-9.8.7")
        self.assertIsNone(parse_version("VERSION one\nVERSION two\n"))

    def test_firmware_uses_existing_verified_https_and_transactional_ota(self) -> None:
        source = (ROOT / "src" / "online_updater.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include" / "zifi" / "online_updater.hpp").read_text(
            encoding="utf-8"
        )
        net = (ROOT / "src" / "net_client.cpp").read_text(encoding="utf-8")

        self.assertIn('kGithubHost[] = "raw.githubusercontent.com"', source)
        self.assertIn('"firmware.sha256"', source)
        self.assertIn('"firmware.bin"', source)
        self.assertNotIn("firmware.factory.bin", source)
        self.assertIn("NetClient& client_", header)
        self.assertIn("bool check(", header)
        self.assertIn("VERSION", source)
        self.assertIn("isEsp32S3ApplicationImage", source)
        self.assertLess(source.index("isEsp32S3ApplicationImage"), source.index("Update.begin"))
        self.assertIn("difference |= digest[index] ^ expectedSha[index]", source)
        self.assertLess(source.index("difference |= digest"), source.index("Update.end(false)"))
        self.assertIn("Update.abort()", source)
        self.assertNotIn("already current", source.lower())
        self.assertIn("setCACertBundle(kCaBundleStart)", net)
        self.assertNotIn("setInsecure", net)

    def test_protocol_and_wmf_contract_match(self) -> None:
        protocol = (ROOT / "include" / "zifi" / "protocol.hpp").read_text(
            encoding="utf-8"
        )
        z80_protocol = (ROOT / "shared" / "z80" / "proto.asm").read_text(
            encoding="utf-8"
        )
        plugin = (ROOT / "Online Update" / "src" / "updater.asm").read_text(
            encoding="utf-8"
        )

        self.assertIn("kOnlineUpdate = 0x0D", protocol)
        self.assertIn("kOnlineUpdateCheck = 0x0E", protocol)
        self.assertIn("kRespOnlineUpdate = 0x8D", protocol)
        self.assertIn("kRespOnlineUpdateCheck = 0x8E", protocol)
        self.assertIn("kEventOnlineUpdateProgress = 0x65", protocol)
        self.assertIn("CMD_ONLINE_UPDATE equ #0D", z80_protocol)
        self.assertIn("CMD_ONLINE_UPDATE_CHECK equ #0E", z80_protocol)
        self.assertIn("RESP_ONLINE_UPDATE equ #8D", z80_protocol)
        self.assertIn("RESP_ONLINE_UPDATE_CHECK equ #8E", z80_protocol)
        self.assertIn("EVT_ONLINE_UPDATE_PROGRESS equ #65", z80_protocol)
        self.assertLess(plugin.index("call Config_Load"), plugin.index("CMD_WIFI_INI"))
        self.assertLess(
            plugin.index("call Updater_StartLink"),
            plugin.index("ld a,CMD_ONLINE_UPDATE"),
        )
        self.assertIn("ld a,CMD_WIFI_INI", plugin)
        self.assertIn("ld a,CMD_ONLINE_UPDATE_CHECK", plugin)
        self.assertLess(
            plugin.index("call Updater_CheckPublished"),
            plugin.index(".confirm:"),
        )
        ui = (ROOT / "Online Update" / "src" / "ui.asm").read_text(
            encoding="utf-8"
        )
        self.assertIn('db "Current: "', ui)
        self.assertIn('db "Available: "', ui)
        self.assertIn('db "Progress: ["', ui)
        self.assertIn("UI_PROGRESS_BAR_WIDTH equ 32", ui)
        self.assertIn("Same version. ENTER = reinstall", ui)
        self.assertIn("ENTER - install or reinstall", ui)

    def test_built_wmf_has_wild_commander_menu_header(self) -> None:
        plugin = (ROOT / "Online Update" / "build" / "ZIFIUPD.WMF").read_bytes()
        self.assertLess(len(plugin), 16 * 1024)
        self.assertEqual(plugin[16:32], b"WildCommanderMDL")
        self.assertEqual(plugin[32], 0x0A)
        self.assertEqual(plugin[34:36], b"\x01\x00")
        self.assertEqual(
            plugin[165:197].rstrip(b" "), b"ZiFi Online Update v0.3"
        )
        self.assertEqual(plugin[197], 0x03)


if __name__ == "__main__":
    unittest.main()
