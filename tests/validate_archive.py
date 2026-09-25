"""Optional, read-only archive validation. Outputs live in a temporary directory.

Usage: python tests/validate_archive.py BUILD/xisf2fits /path/to/DeepSky
Select one frame per NINA version and one frame with PixInsight commentary.
This is deliberately separate from CTest, which needs no personal archive.
"""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib

from astropy.io import fits
import lz4.block
import numpy as np
import zstandard

NS = {"x": "http://www.pixinsight.com/xisf"}


def header(path):
    with path.open("rb") as stream:
        signature = stream.read(16)
        if len(signature) != 16 or signature[:8] != b"XISF0100":
            raise ValueError(f"invalid XISF signature: {path}")
        size = struct.unpack_from("<I", signature, 8)[0]
        if size > 64 * 1024 * 1024:
            raise ValueError(f"oversized header: {path}")
        return ET.fromstring(stream.read(size))


def pixels(path, image):
    kind, offset, size = image.get("location").split(":")
    assert kind == "attachment"
    with path.open("rb") as stream:
        stream.seek(int(offset))
        stored = stream.read(int(size))
    assert len(stored) == int(size)
    algorithm, digest = image.get("checksum").split(":")
    assert hashlib.new(algorithm.replace("-", ""), stored).hexdigest() == digest.lower()
    compression = image.get("compression", "").split(":")
    codec = compression[0].split("+")[0]
    raw = stored
    if codec:
        sizes = [tuple(map(int, part.split(","))) for part in image.get("subblocks", "").split(":") if part]
        if not sizes:
            sizes = [(len(stored), int(compression[1]))]
        chunks = []
        offset = 0
        for compressed, uncompressed in sizes:
            block = stored[offset:offset + compressed]
            if codec in ("lz4", "lz4hc"):
                block = lz4.block.decompress(block, uncompressed_size=uncompressed)
            elif codec == "zlib":
                block = zlib.decompress(block)
            elif codec == "zstd":
                block = zstandard.ZstdDecompressor().decompress(block, max_output_size=uncompressed)
            else:
                raise ValueError(codec)
            assert len(block) == uncompressed
            chunks.append(block)
            offset += compressed
        assert offset == len(stored)
        raw = b"".join(chunks)
    if "+sh" in compression[0]:
        width = int(compression[2])
        raw = np.frombuffer(raw, dtype=np.uint8).reshape(width, -1).T.copy().tobytes()
    width, height, channels = map(int, image.get("geometry").split(":"))
    assert channels == 1 and image.get("sampleFormat") == "UInt16"
    return np.frombuffer(raw, dtype="<u2").reshape(height, width)


def normalized(value):
    return value.translate(str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-",
                                        "\t": " ", "\r": " ", "\n": " "})).encode("ascii", "replace").decode()


def keyword_value(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] == "'":
        return normalized(value[1:-1].replace("''", "'")).rstrip()
    if not value:
        return None
    if value in ("T", "F"):
        return value == "T"
    if re.fullmatch(r"[+-]?\d+", value):
        return int(value)
    if re.fullmatch(r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[EeDd][+-]?\d+)?", value):
        return float(value.replace("D", "E").replace("d", "e"))
    return normalized(value).rstrip()


def main():
    binary, archive = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
    samples = {}
    scanned = 0
    for path in sorted(archive.rglob("*")):
        if path.suffix.lower() != ".xisf":
            continue
        doc = header(path)
        images = doc.findall("x:Image", NS)
        scanned += 1
        if len(images) != 1:
            continue
        keywords = images[0].findall("x:FITSKeyword", NS)
        version = next((k.get("value", "").strip("'") for k in keywords if k.get("name") == "SWCREATE"), "")
        if "N.I.N.A." not in version:
            continue
        samples.setdefault(version, path)
        if any(k.get("name") in ("COMMENT", "HISTORY") and not k.get("value") and k.get("comment") for k in keywords):
            samples.setdefault("PixInsight commentary", path)
    print(f"Scanned {scanned} XISF headers; validating {len(samples)} representative cases", flush=True)
    total_pixels = total_keywords = total_properties = 0
    with tempfile.TemporaryDirectory(prefix="xisf2fits-archive-validation-") as directory:
        for i, (version, path) in enumerate(sorted(samples.items())):
            doc = header(path)
            image = doc.find("x:Image", NS)
            output = Path(directory) / f"{i}.fits.fz"
            result = subprocess.run([str(binary), "--verify", str(path), str(output)], capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            expected = pixels(path, image)
            with fits.open(output, disable_image_compression=True, checksum=True) as hdus:
                hdus.verify("exception")
                for hdu in hdus:
                    assert hdu.verify_checksum() == hdu.verify_datasum() == 1
            with fits.open(output, uint=True) as hdus:
                hdus.verify("exception")
                np.testing.assert_array_equal(hdus[1].data, expected)
                h = hdus[1].header
                for key in image.findall("x:FITSKeyword", NS):
                    name = key.get("name").upper()
                    if name in ("COMMENT", "HISTORY"):
                        for value in dict.fromkeys([key.get("value", ""), key.get("comment", "")]):
                            if value:
                                assert normalized(value).rstrip() in "".join(h[name]), (path, name, value)
                    elif name in h and name not in ("ROWORDER", "BITPIX", "BZERO", "BSCALE", "CHECKSUM", "DATASUM", "SIMPLE", "EXTEND") and not name.startswith("NAXIS"):
                        assert h[name] == keyword_value(key.get("value", "")), (path, name, h[name], key.get("value"))
                        total_keywords += 1
                    elif name not in ("SIMPLE", "EXTEND", "END", "CHECKSUM", "DATASUM"):
                        raise AssertionError(f"missing source keyword {name}: {path}")
                for scope, pattern in (("IMAGE", "x:Image/x:Property"), ("FILE", "x:Metadata/x:Property")):
                    for prop in doc.findall(pattern, NS):
                        key = "XISF." + scope + "." + prop.get("id").upper().replace(":", ".")
                        value = prop.text or "" if prop.get("type") == "String" else prop.get("value", "")
                        if prop.get("type") in ("String", "TimePoint"):
                            assert h[key] == normalized(value).rstrip(), (path, key)
                        else:
                            assert h[key] == keyword_value(value), (path, key)
                            assert value.upper() in str(h.cards[key]), (path, key, "numeric precision")
                            if prop.get("type").startswith("Float"):
                                assert isinstance(h[key], float), (path, key, "floating property type")
                        total_properties += 1
                total_pixels += expected.size
            print(f"PASS {version}: {expected.size:,} pixels; {path.relative_to(archive)}", flush=True)
    assert samples, "no NINA frames found"
    print(f"PASS total: {total_pixels:,} pixel comparisons, {total_keywords} capture keywords, {total_properties} properties", flush=True)


if __name__ == "__main__":
    main()
