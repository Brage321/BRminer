#!/usr/bin/env python3
"""Download NVIDIA NVRTC from an official, checksum-verified CUDA manifest."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

release = sys.argv[1] if len(sys.argv) > 1 else "13.4.2"
destination = Path(sys.argv[2] if len(sys.argv) > 2 else "/work")
destination.mkdir(parents=True, exist_ok=True)
base = "https://developer.download.nvidia.com/compute/cuda/redist/"
manifest = json.loads(subprocess.check_output(["curl", "-fsSL", base + "redistrib_" + release + ".json"]))
component = manifest["cuda_nvrtc"]["linux-x86_64"]
archive_name = Path(component["relative_path"]).name
archive = Path(sys.argv[3]) if len(sys.argv) > 3 else destination / archive_name

def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()

if not archive.is_file() or digest(archive) != component["sha256"]:
    subprocess.run(["curl", "-fL", base + component["relative_path"], "-o", str(archive)], check=True)
if digest(archive) != component["sha256"]:
    raise RuntimeError("NVRTC archive checksum mismatch")
folder = destination / archive_name.removesuffix(".tar.xz")
with tarfile.open(archive) as package:
    if folder.exists():
        # Verify without overwriting a compiler that a running miner may have loaded.
        for member in package:
            path = destination / member.name
            if not path.resolve().is_relative_to(destination.resolve()):
                raise RuntimeError("Unexpected archive path")
            if member.isfile():
                with package.extractfile(member) as file:
                    expected = hashlib.file_digest(file, "sha256").hexdigest()
                if not path.is_file() or digest(path) != expected:
                    raise RuntimeError(f"Existing payload differs: {path}; use another destination")
            elif member.issym() and (not path.is_symlink() or str(path.readlink()) != member.linkname):
                raise RuntimeError(f"Existing library link differs: {path}")
    else:
        with tempfile.TemporaryDirectory(prefix="nvrtc-extract-", dir=destination) as temporary:
            package.extractall(temporary, filter="data")
            (Path(temporary) / folder.name).rename(folder)
print(f"Verified NVRTC {manifest['cuda_nvrtc']['version']} from CUDA {release}: {folder}")
