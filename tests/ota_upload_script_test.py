import importlib.util
import io
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ota_upload", ROOT / "scripts" / "ota_upload.py")
OTA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OTA)


class OtaUploadTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
