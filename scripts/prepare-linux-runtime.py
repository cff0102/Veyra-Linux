#!/usr/bin/env python3
"""Fetch unmodified NVIDIA packages outside the source tree; no system install."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request
import zipfile

PACKAGES = (
    ("vfx", "https://pypi.nvidia.com/nvidia-vfx/"
     "nvidia_vfx-0.2.0.0-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl",
     "0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8"),
    ("cuda", "https://files.pythonhosted.org/packages/bc/46/"
     "a92db19b8309581092a3add7e6fceb4c301a3fd233969856a8cbf042cd3c/"
     "nvidia_cuda_runtime_cu12-12.9.79-py3-none-manylinux2014_x86_64.manylinux_2_17_x86_64.whl",
     "25bba2dfb01d48a9b59ca474a1ac43c6ebf7011f1b0b8cc44f54eb6ac48a96c3"),
)


def prepare(root: Path):
    source = Path(__file__).resolve().parents[1]
    root = root.resolve()
    if root == source or source in root.parents:
        raise ValueError("Runtime files must be outside the source repository")
    downloads = root / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    records = []
    for component, url, expected in PACKAGES:
        archive = downloads / url.rsplit("/", 1)[1]
        if not archive.exists():
            partial = archive.with_suffix(".partial")
            print(f"Downloading {archive.name}", flush=True)
            with urllib.request.urlopen(url, timeout=60) as response, partial.open("wb") as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            partial.replace(archive)
        with archive.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            raise ValueError(f"Checksum mismatch for {archive}; remove it and retry")
        target = root / "deps" / component
        target.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as package:
            for member in package.infolist():
                dest = (target / member.filename).resolve()
                if target.resolve() not in dest.parents:
                    raise ValueError("Archive contains an unsafe path")
            package.extractall(target)
        records.append({"component": component, "url": url, "sha256": actual,
                        "bytes": archive.stat().st_size})
        print(f"Verified and extracted {component}: {target}")
    (root / "runtime-provenance.json").write_text(json.dumps(records, indent=2) + "\n")
    print("NVIDIA licenses are retained in each extracted wheel's .dist-info/licenses directory.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=Path(__file__).resolve().parents[2] / "Veyra-local")
    prepare(parser.parse_args().root)
