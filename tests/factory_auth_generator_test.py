import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "generate_factory_auth.py"
SPEC = importlib.util.spec_from_file_location("factory_generator", SCRIPT)
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


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

    def test_force_replaces_both_outputs_and_leaves_no_temps(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.generate(directory, "001")
            old_header = (directory / "factory001.h").read_text()
            result = subprocess.run([sys.executable, str(SCRIPT), "--device-id", "K10-001",
                "--header", str(directory / "factory001.h"), "--label", str(directory / "label001.txt"),
                "--iterations", "120000", "--force"])
            self.assertEqual(0, result.returncode)
            self.assertNotEqual(old_header, (directory / "factory001.h").read_text())
            self.assertEqual([], list(directory.glob("*.tmp")))

    def test_failure_cleans_temporary_files(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            real_replace = GENERATOR.os.replace
            calls = 0
            def fail_second(source, target):
                nonlocal calls
                calls += 1
                if calls == 2: raise OSError("injected")
                return real_replace(source, target)
            with mock.patch.object(GENERATOR.os, "replace", side_effect=fail_second):
                with self.assertRaises(OSError):
                    GENERATOR.generate_files("K10-FAIL", directory / "factory.h",
                                             directory / "label.txt", 120000, False)
            self.assertEqual([], list(directory.glob("*.tmp")))
            self.assertFalse((directory / "factory.h").exists())
            self.assertFalse((directory / "label.txt").exists())

    def test_force_failure_restores_preexisting_pair(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            header, label = directory / "factory.h", directory / "label.txt"
            header.write_bytes(b"original-header\x00")
            label.write_bytes(b"original-label\x00")
            real_replace = GENERATOR.os.replace
            publications = 0
            def fail_second_publication(source, target):
                nonlocal publications
                if Path(target) in (header, label):
                    publications += 1
                    if publications == 2: raise OSError("injected publication failure")
                return real_replace(source, target)
            with mock.patch.object(GENERATOR.os, "replace", side_effect=fail_second_publication):
                with self.assertRaisesRegex(OSError, "injected publication failure"):
                    GENERATOR.generate_files("K10-FORCE", header, label, 120000, True)
            self.assertEqual(b"original-header\x00", header.read_bytes())
            self.assertEqual(b"original-label\x00", label.read_bytes())
            self.assertEqual([], list(directory.glob("*.tmp")))
            self.assertEqual([], list(directory.glob("*.bak")))

    def test_secure_temp_write_failure_is_not_masked_and_cleans_file(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            with mock.patch.object(GENERATOR.os, "fdopen", side_effect=RuntimeError("write failed")):
                with self.assertRaisesRegex(RuntimeError, "write failed"):
                    GENERATOR._secure_temp(directory / "factory.h", "contents")
            self.assertEqual([], list(directory.iterdir()))

    def test_second_temp_creation_failure_cleans_first_temp(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            real_temp = GENERATOR._secure_temp
            calls = 0
            def fail_second(target, content):
                nonlocal calls
                calls += 1
                if calls == 2: raise OSError("second temp failed")
                return real_temp(target, content)
            with mock.patch.object(GENERATOR, "_secure_temp", side_effect=fail_second):
                with self.assertRaisesRegex(OSError, "second temp failed"):
                    GENERATOR.generate_files("K10-TEMP", directory / "factory.h",
                                             directory / "label.txt", 120000, False)
            self.assertEqual([], list(directory.iterdir()))

    def test_second_backup_failure_cleans_artifacts_and_preserves_originals(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            header, label = directory / "factory.h", directory / "label.txt"
            header.write_bytes(b"original-header")
            label.write_bytes(b"original-label")
            real_backup = GENERATOR._backup
            calls = 0
            def fail_second(target):
                nonlocal calls
                calls += 1
                if calls == 2: raise OSError("second backup failed")
                return real_backup(target)
            with mock.patch.object(GENERATOR, "_backup", side_effect=fail_second):
                with self.assertRaisesRegex(OSError, "second backup failed"):
                    GENERATOR.generate_files("K10-BACKUP", header, label, 120000, True)
            self.assertEqual(b"original-header", header.read_bytes())
            self.assertEqual(b"original-label", label.read_bytes())
            self.assertEqual([], list(directory.glob("*.tmp")))
            self.assertEqual([], list(directory.glob("*.bak")))


if __name__ == "__main__":
    unittest.main()
