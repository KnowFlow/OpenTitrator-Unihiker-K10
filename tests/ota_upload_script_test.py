import importlib.util
import io
import tempfile
import unittest
import subprocess
import sys
import urllib.error
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ota_upload", ROOT / "scripts" / "ota_upload.py")
OTA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OTA)


class OtaUploadTest(unittest.TestCase):
    def test_cli_refuses_missing_token(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "ota_upload.py"), "firmware.bin"],
            capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--token", result.stderr)

    def test_upload_sends_token_header_without_printing_it(self):
        with tempfile.TemporaryDirectory() as directory:
            firmware = Path(directory) / "firmware.bin"
            firmware.write_bytes(b"firmware")
            response = mock.MagicMock()
            response.__enter__.return_value.read.return_value = b"OK"
            output = io.StringIO()
            with mock.patch.object(OTA.urllib.request, "urlopen", return_value=response) as open_url:
                with redirect_stdout(output):
                    self.assertTrue(OTA.upload("192.0.2.1", str(firmware), "secret-token"))
        request = open_url.call_args.args[0]
        self.assertEqual(request.get_header("X-session-token"), "secret-token")
        self.assertNotIn("secret-token", output.getvalue())

    def test_http_error_does_not_print_token(self):
        with tempfile.TemporaryDirectory() as directory:
            firmware = Path(directory) / "firmware.bin"
            firmware.write_bytes(b"firmware")
            output = io.StringIO()
            error = urllib.error.HTTPError(
                "http://192.0.2.1/ota", 401, "Unauthorized secret-token", {}, None
            )
            with mock.patch.object(OTA.urllib.request, "urlopen", side_effect=error):
                with redirect_stdout(output):
                    self.assertFalse(OTA.upload("192.0.2.1", str(firmware), "secret-token"))
        self.assertNotIn("secret-token", output.getvalue())


if __name__ == "__main__":
    unittest.main()
