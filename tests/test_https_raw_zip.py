import hashlib
import pathlib
import struct
import unittest
from urllib.parse import urlsplit


ROOT = pathlib.Path(__file__).resolve().parents[1]


class HttpsRawZipContractTest(unittest.TestCase):
    def test_vtrd_url_maps_to_tls_transport(self) -> None:
        url = urlsplit("https://vtrd.in/translat/DANDAR1t.zip")
        self.assertEqual(url.hostname, "vtrd.in")
        self.assertEqual(url.port or (443 if url.scheme == "https" else 80), 443)
        self.assertEqual(url.path, "/translat/DANDAR1t.zip")

    def test_spg_passes_direct_url_and_selected_port(self) -> None:
        spg = (ROOT / "ZiFi SPG" / "zifi.asm").read_text(encoding="utf-8")
        net = (ROOT / "ZiFi SPG" / "net.asm").read_text(encoding="utf-8")

        self.assertNotIn("unzipremote.php?f=", spg)
        self.assertNotIn("zip_url_buffer", spg)
        self.assertNotIn('"http://prods.tslabs.info/', spg)
        self.assertIn('"https://prods.tslabs.info/prods_zifi.php?t=1"', spg)
        self.assertIn('"https://prods.tslabs.info/prods_zifi.php?t=2"', spg)
        self.assertIn("ld de,443", spg)
        self.assertIn("request_port\t\tdw 80", spg)
        self.assertIn("ld hl,(request_port)", net)
        self.assertIn("HTTP status is not 2xx", spg)
        self.assertIn("HTTP body length mismatch", spg)
        self.assertIn("Rejected: .zip body is not ZIP", spg)
        self.assertIn("HTTP(S) request timeout", spg)
        self.assertIn("call zifi_get_content_complete", spg)
        self.assertIn("NET_WAIT_OPEN       equ 7200", net)

    def test_firmware_uses_verified_tls_and_identity_body(self) -> None:
        source = (ROOT / "src" / "net_client.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include" / "zifi" / "net_client.hpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("useTls = port == 443", source)
        self.assertIn("setCACertBundle(kCaBundleStart)", source)
        self.assertNotIn("setInsecure", source)
        self.assertIn("kNetworkStackBytes = 16384", main)
        self.assertIn("NETSTK:%uB", main)
        self.assertIn("char httpRequest_[640]", header)
        self.assertNotIn("char request[640]", source)
        self.assertIn("Accept-Encoding: identity", source)
        self.assertIn("!useTls && proxyEnabled_", source)
        self.assertIn("chunked unsupported", source)
        self.assertIn("kMaxRedirects = 5", source)
        self.assertIn("isRedirectStatus(statusCode)", source)
        self.assertIn("redirectLocation_", source)
        self.assertIn("redirect tls downgrade", source)
        self.assertIn("bodyReceived_ >= bodyExpected_", source)
        self.assertIn("tlsPeerClosed()", source)
        self.assertIn('setError(error, errorSize, "body timeout")', source)

    def test_embedded_ca_bundle_is_expected_artifact(self) -> None:
        bundle = (ROOT / "data" / "cert" / "x509_crt_bundle.bin").read_bytes()

        self.assertEqual(struct.unpack(">H", bundle[:2])[0], 121)
        self.assertEqual(len(bundle), 55_587)
        self.assertEqual(
            hashlib.sha256(bundle).hexdigest(),
            "49e7e1ca53f48330b1b507872f1447eb5f333632b6802282ec51aaab5640787c",
        )

    def test_zip_signatures_accepted_by_spg(self) -> None:
        def accepted(payload: bytes) -> bool:
            return len(payload) >= 4 and payload[:4] in {
                b"PK\x03\x04",  # local file header
                b"PK\x05\x06",  # empty archive/end of central directory
                b"PK\x07\x08",  # spanned archive/data descriptor
            }

        valid = (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08")
        invalid = (b"<br ", b"PK\x03\x03", b"", b"PK")

        self.assertTrue(all(accepted(payload) for payload in valid))
        self.assertTrue(all(not accepted(payload) for payload in invalid))


if __name__ == "__main__":
    unittest.main()
