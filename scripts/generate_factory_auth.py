import argparse, hashlib, os, secrets, shutil, tempfile
from pathlib import Path

ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"

def _secure_temp(target, content):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=target.name + ".", suffix=".tmp", dir=target.parent)
    owned_fd = fd
    try:
        try: os.chmod(name, 0o600)
        except OSError: pass
        stream = os.fdopen(owned_fd, "w", encoding="utf-8")
        owned_fd = None
        with stream:
            stream.write(content)
        return Path(name)
    except Exception:
        if owned_fd is not None:
            try: os.close(owned_fd)
            except OSError: pass
        Path(name).unlink(missing_ok=True)
        raise

def _backup(target):
    if not target.exists(): return None
    fd, name = tempfile.mkstemp(prefix=target.name + ".", suffix=".bak", dir=target.parent)
    os.close(fd)
    backup = Path(name)
    try:
        shutil.copyfile(target, backup)
        try: os.chmod(backup, 0o600)
        except OSError: pass
        return backup
    except Exception:
        backup.unlink(missing_ok=True)
        raise

def generate_files(device_id, header_path, label_path, iterations, force):
    header_path, label_path = Path(header_path), Path(label_path)
    if iterations <= 0: raise ValueError("iterations must be positive")
    if not force and (header_path.exists() or label_path.exists()):
        raise FileExistsError("refusing to overwrite output; use --force")
    password = "".join(secrets.choice(ALPHABET) for _ in range(16))
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, iterations)
    def array(name, data):
        return f"static constexpr uint8_t {name}[{len(data)}] = {{" + ", ".join(f"0x{x:02x}" for x in data) + "};\n"
    header = ("#pragma once\n#include <stdint.h>\n"
              "static constexpr uint8_t FACTORY_AUTH_VERSION = 1;\n"
              f"static constexpr uint32_t FACTORY_AUTH_ITERATIONS = {iterations};\n"
              + array("FACTORY_AUTH_SALT", salt) + array("FACTORY_AUTH_HASH", digest))
    label = f"Device: {device_id}\nPassword: {password}\n"
    temps = []
    backups = []
    installed = []
    try:
        for target, content in ((header_path, header), (label_path, label)):
            temps.append(_secure_temp(target, content))
        for target in (header_path, label_path):
            backups.append(_backup(target))
        # Both complete files exist before either public output is replaced.
        os.replace(temps[0], header_path); temps[0] = None; installed.append(header_path)
        os.replace(temps[1], label_path); temps[1] = None; installed.append(label_path)
        try: os.chmod(label_path, 0o600)
        except OSError: pass
    except Exception:
        # Restore the exact prior pair (or prior absence) after partial publication.
        for output, backup in zip((header_path, label_path), backups):
            if backup:
                os.replace(backup, output)
            elif output in installed:
                output.unlink(missing_ok=True)
        raise
    finally:
        for temp in temps:
            if temp: temp.unlink(missing_ok=True)
        for backup in backups:
            if backup: backup.unlink(missing_ok=True)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--device-id", required=True)
    p.add_argument("--header", required=True, type=Path)
    p.add_argument("--label", required=True, type=Path)
    p.add_argument("--iterations", required=True, type=int)
    p.add_argument("--force", action="store_true")
    a = p.parse_args()
    try: generate_files(a.device_id, a.header, a.label, a.iterations, a.force)
    except (ValueError, FileExistsError) as error: p.error(str(error))

if __name__ == "__main__": main()
