import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class SmbWifiSignalTest(unittest.TestCase):
    def test_signal_uses_a_dedicated_event_not_sys_info(self) -> None:
        protocol = (ROOT / "include" / "zifi" / "protocol.hpp").read_text(
            encoding="utf-8"
        )
        z80_protocol = (ROOT / "shared" / "z80" / "proto.asm").read_text(
            encoding="utf-8"
        )
        firmware = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        network_loop = firmware[
            firmware.index("void Application::networkTaskLoop()") :
            firmware.index("bool Application::connectWifi(")
        ]

        self.assertIn("kEventWifiSignal = 0x66", protocol)
        self.assertIn("EVT_WIFI_SIGNAL equ #66", z80_protocol)
        self.assertIn("WiFi.RSSI()", network_loop)
        self.assertIn("enqueueNetworkEvent(kEventWifiSignal", network_loop)
        self.assertNotIn("processSysInfo", network_loop)
        self.assertNotIn("kSysInfo", network_loop)

    def test_plugin_replaces_listening_with_signal_bar(self) -> None:
        plugin = (ROOT / "SMB Server" / "src" / "smb_server.asm").read_text(
            encoding="utf-8"
        )
        ui = (ROOT / "SMB Server" / "src" / "ui.asm").read_text(
            encoding="utf-8"
        )
        handler = plugin[
            plugin.index("SmbEvent_WifiSignal:") :
            plugin.index("; --- данные")
        ]

        self.assertIn("ld hl,UiWifiWaiting", plugin)
        self.assertIn("cp EVT_WIFI_SIGNAL", plugin)
        self.assertIn("call Ui_SetStatus", handler)
        self.assertNotIn("CMD_SYS_INFO", handler)
        self.assertIn(
            'UiWifiWaiting:     db "Wi-Fi [................]   0%",0', ui
        )
        self.assertNotIn("UiStageListening", ui)

    def test_bar_and_exact_percent_fit_status_field(self) -> None:
        width = 16
        for percent in (0, 1, 25, 50, 75, 99, 100):
            filled = (percent * width + 50) // 100
            text = (
                "Wi-Fi ["
                + "#" * filled
                + "." * (width - filled)
                + f"] {percent:3d}%"
            )
            self.assertEqual(len(text), 29)
            self.assertLessEqual(len(text), 30)


if __name__ == "__main__":
    unittest.main()
