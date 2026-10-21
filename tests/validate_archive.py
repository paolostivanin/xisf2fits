"""Optional, read-only archive validation. Outputs live in a temporary directory.

Usage: python tests/validate_archive.py BUILD/xisf2fits /path/to/archive
Select one frame per NINA version, one frame with PixInsight commentary, and
one multi-image master per layout of image ids.
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
    if image.get("checksum"):
        algorithm, digest = image.get("checksum").split(":")
        # XISF names (SHA-1, SHA256, SHA3-256) to hashlib names (sha1, sha256, sha3_256).
        algorithm = algorithm.lower()
        algorithm = algorithm.replace("-", "_") if algorithm.startswith("sha3") else algorithm.replace("-", "")
        assert hashlib.new(algorithm, stored).hexdigest() == digest.lower()
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
    assert channels == 1 and image.get("sampleFormat") in ("UInt16", "Float32")
    return np.frombuffer(raw, dtype="<f4" if image.get("sampleFormat") == "Float32" else "<u2").reshape(height, width)


def same_pixels(actual, expected):
    if expected.dtype.kind == "f":
        # Bitwise: signed zeros, denormals and NaN payloads must survive.
        np.testing.assert_array_equal(np.asarray(actual, dtype="=f4").view("=u4"), expected.astype("=f4").view("=u4"))
    else:
        np.testing.assert_array_equal(actual, expected)


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
            samples.setdefault("master " + "+".join(image.get("id", "") for image in images), path)
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
            images = doc.findall("x:Image", NS)
            image = images[0]
            output = Path(directory) / f"{i}.fits.fz"
            result = subprocess.run([str(binary), "--verify", str(path), str(output)], capture_output=True, text=True, timeout=300)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            expected = [pixels(path, item) for item in images]
            with fits.open(output, disable_image_compression=True, checksum=True) as hdus:
                hdus.verify("exception")
                assert len(hdus) == len(images) + 1, (path, len(hdus))
                for hdu in hdus:
                    assert hdu.verify_checksum() == hdu.verify_datasum() == 1
                for hdu, item in zip(hdus[1:], images):
                    float32 = item.get("sampleFormat") == "Float32"
                    assert hdu.header["ZCMPTYPE"] == ("GZIP_2" if float32 else "RICE_1"), (path, item.get("id"))
                    assert not float32 or hdu.header["ZQUANTIZ"] == "NONE", (path, item.get("id"))
            with fits.open(output, uint=True) as hdus:
                hdus.verify("exception")
                for hdu, item, data in zip(hdus[1:], images, expected):
                    same_pixels(hdu.data, data)
                    if len(images) > 1:
                        assert hdu.header["EXTNAME"] == item.get("id"), (path, hdu.header["EXTNAME"])
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
                for scope, props in (("IMAGE", image.findall("x:Property", NS)), ("FILE", doc.findall("x:Metadata/x:Property", NS))):
                    for prop in props:
                        # Native PixInsight solutions are omitted, with a warning.
                        if prop.get("id").startswith(("PCL:AstrometricSolution", "AstrometricSolution")):
                            assert "omitted PixInsight astrometric solution" in result.stderr, path
                            continue
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
                count = sum(data.size for data in expected)
                total_pixels += count
            print(f"PASS {version}: {count:,} pixels; {path.relative_to(archive)}", flush=True)
    assert samples, "no NINA frames or masters found"
    print(f"PASS total: {total_pixels:,} pixel comparisons, {total_keywords} capture keywords, {total_properties} properties", flush=True)


if __name__ == "__main__":
    main()
