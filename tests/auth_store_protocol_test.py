import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / "ph_titrator" / "auth_store.cpp").read_text()

class AuthStoreProtocolTest(unittest.TestCase):
    def test_load_requires_committed_marker(self):
        self.assertIn('getUChar("admin_valid", 0)', SOURCE)
        self.assertIn('validMarker == 1', SOURCE)

    def test_save_invalidates_before_data_and_commits_last(self):
        invalidate = SOURCE.index('putUChar("admin_valid", 0)')
        salt = SOURCE.index('putBytes("admin_salt"')
        commit = SOURCE.index('putUChar("admin_valid", 1)')
        self.assertLess(invalidate, salt)
        self.assertLess(salt, commit)

if __name__ == "__main__": unittest.main()
