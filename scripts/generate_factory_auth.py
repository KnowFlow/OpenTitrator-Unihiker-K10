import argparse, hashlib, secrets
from pathlib import Path

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--device-id", required=True)
    p.add_argument("--header", required=True, type=Path)
    p.add_argument("--label", required=True, type=Path)
    p.add_argument("--iterations", required=True, type=int)
    p.add_argument("--force", action="store_true")
    a = p.parse_args()
    if a.iterations <= 0: p.error("iterations must be positive")
    if not a.force and (a.header.exists() or a.label.exists()):
        p.error("refusing to overwrite output; use --force")
    password = "".join(secrets.choice(ALPHABET) for _ in range(16))
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, a.iterations)
    def array(name, data):
        return f"static constexpr uint8_t {name}[{len(data)}] = {{" + ", ".join(f"0x{x:02x}" for x in data) + "};\n"
    header = ("#pragma once\n#include <stdint.h>\n"
              "static constexpr uint8_t FACTORY_AUTH_VERSION = 1;\n"
              f"static constexpr uint32_t FACTORY_AUTH_ITERATIONS = {a.iterations};\n"
              + array("FACTORY_AUTH_SALT", salt) + array("FACTORY_AUTH_HASH", digest))
    a.header.parent.mkdir(parents=True, exist_ok=True)
    a.label.parent.mkdir(parents=True, exist_ok=True)
    a.header.write_text(header, encoding="utf-8")
    a.label.write_text(f"Device: {a.device_id}\nPassword: {password}\n", encoding="utf-8")

if __name__ == "__main__": main()
