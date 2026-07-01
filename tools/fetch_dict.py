#!/usr/bin/env python3
"""Download steno dictionary with caching.

Checks local file hash against known upstream hash.
If file exists and hash matches → skip download.
If file missing or hash mismatch → download fresh.
"""

import hashlib
import json
import os
import sys
import urllib.request

DICTS = {
    "plover": {
        "url": "https://raw.githubusercontent.com/openstenoproject/plover/main/plover/assets/main.json",
        "filename": "plover-main.json",
    },
    "lapwing": {
        "url": "https://raw.githubusercontent.com/aerickt/steno-dictionaries/main/lapwing-base.json",
        "filename": "lapwing.json",
    },
}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()


def download(url, dest):
    import subprocess
    print(f"Downloading {url}...")
    try:
        urllib.request.urlretrieve(url, dest)
    except urllib.error.URLError:
        # Fallback to curl which uses system cert store
        subprocess.check_call(["curl", "-sL", "-o", dest, url])
    size = os.path.getsize(dest)
    print(f"Downloaded {size:,} bytes to {dest}")


def fetch(dict_name, dest_dir):
    if dict_name not in DICTS:
        print(f"Unknown dict: {dict_name}. Available: {', '.join(DICTS.keys())}")
        return 1

    info = DICTS[dict_name]
    dest = os.path.join(dest_dir, info["filename"])
    hash_file = dest + ".sha256"

    if os.path.exists(dest):
        local_hash = sha256_file(dest)

        if os.path.exists(hash_file):
            with open(hash_file) as f:
                cached_hash = f.read().strip()
            if local_hash == cached_hash:
                print(f"{dest} up to date (sha256={local_hash[:12]}...)")
                return 0

        try:
            json.load(open(dest))
            print(f"{dest} exists, valid JSON (sha256={local_hash[:12]}...)")
            with open(hash_file, "w") as f:
                f.write(local_hash)
            return 0
        except (json.JSONDecodeError, IOError):
            print(f"{dest} corrupted, re-downloading")

    os.makedirs(dest_dir, exist_ok=True)
    download(info["url"], dest)

    new_hash = sha256_file(dest)
    with open(hash_file, "w") as f:
        f.write(new_hash)

    try:
        with open(dest) as f:
            d = json.load(f)
        print(f"Verified: {len(d)} entries")
    except (json.JSONDecodeError, IOError) as e:
        print(f"WARNING: downloaded file invalid: {e}")
        return 1

    return 0


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <plover|lapwing> [dest_dir]")
        print(f"  dest_dir defaults to ./dicts/")
        sys.exit(1)

    dict_name = sys.argv[1]
    dest_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dicts")

    sys.exit(fetch(dict_name, dest_dir))


if __name__ == "__main__":
    main()
