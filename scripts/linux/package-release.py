#!/usr/bin/env python3
"""Package a tested Linux binary without NVIDIA CUDA redistributables."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, default=Path("/workspace"))
parser.add_argument("--binary", type=Path, default=Path("/work/BRminer/brminer"))
parser.add_argument("--core", type=Path, default=Path("/work/deps/connectcoin"))
parser.add_argument("--janus", type=Path, default=Path("/work/deps/janusminer"))
parser.add_argument("--mbedtls", type=Path, default=Path("/work/build/_deps/connectcoin_mbedtls-src"))
parser.add_argument("--boost-notices", type=Path)
parser.add_argument("--output", type=Path, default=Path("/work/release"))
args = parser.parse_args()
with args.binary.open("rb") as binary:
    header = binary.read(20)
if header[:5] != b"\x7fELF\x02" or header[18:20] != b"\x3e\x00":
    raise RuntimeError("Expected a Linux x86_64 ELF executable")
if "-w ENTER-YOUR-WARTHOG-WALLET" not in (args.source / "mining-warthog.sh").read_text():
    raise RuntimeError("Release launcher must contain a wallet placeholder")
if args.boost_notices is None:
    boost_notices = sorted(Path("/usr/share/doc").glob("libboost[0-9]*-dev/copyright"))
    if len(boost_notices) != 1:
        raise RuntimeError("Specify --boost-notices for the Boost version used by the build")
    args.boost_notices = boost_notices[0]
version = re.search(r"project\(BRminer VERSION ([^ )]+)", (args.source / "CMakeLists.txt").read_text())[1]
name = f"BRminer-{version}-linux-x64"
# A new staging directory prevents stale files from entering an archive.
args.output.mkdir(parents=True, exist_ok=True)
import tempfile
with tempfile.TemporaryDirectory(prefix="brminer-package-", dir=args.output) as temporary:
    stage = Path(temporary) / name
    stage.mkdir()
    shutil.copy2(args.binary, stage / "brminer")
    (stage / "brminer").chmod(0o755)
    for filename in ("mining-warthog.sh", "benchmark-warthog.sh", "calibrate-warthog.sh", "README.md"):
        shutil.copy2(args.source / filename, stage / filename)
        if filename.endswith(".sh"):
            (stage / filename).chmod(0o755)
    (stage / "docs").mkdir()
    for document in sorted((args.source / "docs").glob("*.md")):
        shutil.copy2(document, stage / "docs" / document.name)
    notices = {
        args.source / "LICENSE": "BRminer-LICENSE.txt",
        args.core / "COPYING": "ConnectCoin-COPYING.txt",
        args.core / "src/crypto/ctaes/COPYING": "ctaes-COPYING.txt",
        args.core / "src/crc32c/LICENSE": "crc32c-LICENSE.txt",
        args.core / "src/leveldb/LICENSE": "LevelDB-LICENSE.txt",
        args.core / "src/minisketch/LICENSE": "minisketch-LICENSE.txt",
        args.core / "src/randomx/LICENSE": "RandomX-LICENSE.txt",
        args.core / "src/secp256k1/COPYING": "secp256k1-COPYING.txt",
        args.janus / "LICENSE": "JanusMiner-LICENSE.txt",
        args.mbedtls / "LICENSE": "mbedTLS-LICENSE.txt",
        args.source / "src/warthog_cuda_kernel.LICENSE": "CUDA-kernel-notices.txt",
        args.source / "licenses/JANUSMINER-LICENSES.txt": "JanusMiner-and-VerusHash-notices.txt",
        Path("/usr/share/doc/libssl3t64/copyright"): "OpenSSL-notices.txt",
        args.boost_notices: "Boost-notices.txt",
    }
    (stage / "licenses").mkdir()
    for source, filename in notices.items():
        shutil.copy2(source, stage / "licenses" / filename)
    # CUDA/NVRTC libraries are supplied separately by the end user.
    archive = args.output / (name + ".tar.gz")
    with tarfile.open(archive, "w:gz") as package:
        package.add(stage, arcname=name)
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_name(archive.name + ".sha256").write_text(f"{digest}  {archive.name}\n")
print(f"Release archive: {archive}\nSHA-256: {digest}")
