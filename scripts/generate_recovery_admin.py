import argparse
import hashlib
import os
import secrets
import tempfile
from pathlib import Path


ITERATIONS = 120000


def header_for(password: str) -> str:
    encoded = password.encode("utf-8")
    if not 10 <= len(encoded) <= 64:
        raise ValueError("administrator password must be 10 to 64 UTF-8 bytes")
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", encoded, salt, ITERATIONS)
    def array(name: str, value: bytes) -> str:
        return f"static constexpr uint8_t {name}[{len(value)}] = {{" + ", ".join(f"0x{x:02x}" for x in value) + "};\n"
    return ("#pragma once\n#include <stdint.h>\n"
            "static constexpr uint8_t RECOVERY_ADMIN_VERSION = 1;\n"
            f"static constexpr uint32_t RECOVERY_ADMIN_ITERATIONS = {ITERATIONS};\n"
            + array("RECOVERY_ADMIN_SALT", salt)
            + array("RECOVERY_ADMIN_HASH", digest))


def write_private(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        try:
            os.chmod(temporary, 0o600)
        except OSError:
            pass
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(content)
        os.replace(temporary, path)
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
    except Exception:
        Path(temporary).unlink(missing_ok=True)
        raise


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", required=True, type=Path)
    args = parser.parse_args()
    password = os.environ["K10_RECOVERY_PASSWORD"]
    try:
        write_private(args.header, header_for(password))
    finally:
        password = ""


if __name__ == "__main__":
    main()
