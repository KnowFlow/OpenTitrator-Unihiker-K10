import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "generate_factory_auth.py"


class FactoryAuthGeneratorTest(unittest.TestCase):
    def generate(self, directory: Path, suffix: str):
        header = directory / f"factory{suffix}.h"
        label = directory / f"label{suffix}.txt"
        subprocess.run([sys.executable, str(SCRIPT), "--device-id", f"K10-{suffix}",
                        "--header", str(header), "--label", str(label),
                        "--iterations", "120000"], check=True)
        return header.read_text(), label.read_text()

    def test_generates_unique_safe_credentials_without_header_plaintext(self):
        with tempfile.TemporaryDirectory() as temp:
            first_h, first_l = self.generate(Path(temp), "001")
            second_h, second_l = self.generate(Path(temp), "002")
        passwords = [re.search(r"Password: (\S+)", x).group(1) for x in (first_l, second_l)]
        self.assertTrue(all(len(p) == 16 for p in passwords))
        self.assertTrue(all(not set(p) & set("0O1Il") for p in passwords))
        self.assertNotEqual(passwords[0], passwords[1])
        for header, label, password, device in [(first_h, first_l, passwords[0], "K10-001"), (second_h, second_l, passwords[1], "K10-002")]:
            self.assertIn("FACTORY_AUTH_VERSION = 1", header)
            self.assertIn("FACTORY_AUTH_ITERATIONS = 120000", header)
            arrays = re.findall(r"\{([^}]*)\}", header)
            self.assertEqual([16, 32], [len(re.findall(r"0x[0-9a-f]{2}", a)) for a in arrays])
            self.assertNotIn(password, header)
            self.assertIn(device, label)
        self.assertNotEqual(re.findall(r"0x[0-9a-f]{2}", first_h)[:16], re.findall(r"0x[0-9a-f]{2}", second_h)[:16])

    def test_refuses_overwrite_without_force(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.generate(directory, "001")
            result = subprocess.run([sys.executable, str(SCRIPT), "--device-id", "K10-001",
                                     "--header", str(directory / "factory001.h"),
                                     "--label", str(directory / "label001.txt"),
                                     "--iterations", "120000"], capture_output=True)
            self.assertNotEqual(0, result.returncode)


if __name__ == "__main__":
    unittest.main()
