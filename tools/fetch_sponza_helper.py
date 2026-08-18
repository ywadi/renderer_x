#!/usr/bin/env python3
"""fetch_sponza_helper.py -- downloads every file in a GitHub Contents API
directory listing and verifies each one against the git BLOB sha1 the API
itself reports (the same value `git hash-object` computes locally: sha1
of b"blob " + str(len(content)).encode() + b"\\0" + content). See
tools/fetch_assets.sh's own comment on why Sponza (71 files) uses this
dynamically-fetched-and-verified scheme instead of hand-pinned checksums.
"""
import hashlib
import json
import sys
import urllib.request


def git_blob_sha1(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode()
    return hashlib.sha1(header + data).hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <github-contents-api-url> <dest-dir>", file=sys.stderr)
        return 1
    api_url, dest_dir = sys.argv[1], sys.argv[2]

    with urllib.request.urlopen(api_url) as resp:
        entries = json.load(resp)

    for entry in entries:
        if entry.get("type") != "file":
            continue
        name = entry["name"]
        download_url = entry["download_url"]
        expected_sha = entry["sha"]

        print(f"[fetch_sponza] downloading {name}")
        with urllib.request.urlopen(download_url) as resp:
            data = resp.read()

        actual_sha = git_blob_sha1(data)
        if actual_sha != expected_sha:
            print(f"[fetch_sponza] CHECKSUM MISMATCH for {name}: expected {expected_sha}, got {actual_sha}",
                  file=sys.stderr)
            return 1

        with open(f"{dest_dir}/{name}", "wb") as f:
            f.write(data)

    print(f"[fetch_sponza] {len(entries)} entries verified and written to {dest_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
