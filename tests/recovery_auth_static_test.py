import unittest
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RecoveryAuthStaticTest(unittest.TestCase):
    def test_recovery_environment_is_explicit_and_isolated(self):
        platformio = (ROOT / "platformio.ini").read_text()
        self.assertIn("[env:unihiker-auth-recovery]", platformio)
        self.assertIn("-DAUTH_USB_RECOVERY=1", platformio)

    def test_recovery_entrypoint_writes_only_admin_credential(self):
        recovery_path = ROOT / "ph_titrator" / "auth_usb_recovery.cpp"
        self.assertTrue(recovery_path.exists())
        recovery = recovery_path.read_text()
        self.assertIn("authStore.loadRecoveryAdministrator", recovery)
        self.assertIn("authStore.saveAdministrator", recovery)
        self.assertIn('Serial.println("AUTH_RECOVERY:OK")', recovery)
        self.assertNotIn("prefs.clear", recovery)

    def test_generator_keeps_password_out_of_generated_header(self):
        generator_path = ROOT / "scripts" / "generate_recovery_admin.py"
        self.assertTrue(generator_path.exists())
        generator = generator_path.read_text()
        self.assertIn('pbkdf2_hmac("sha256"', generator)
        self.assertIn('os.environ["K10_RECOVERY_PASSWORD"]', generator)
        spec = importlib.util.spec_from_file_location("recovery_generator", generator_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        header = module.header_for("safe-test-123")
        self.assertNotIn("safe-test-123", header)
        self.assertIn("#include <stdint.h>\n", header)
        self.assertNotIn("\\n", header)


if __name__ == "__main__":
    unittest.main()
