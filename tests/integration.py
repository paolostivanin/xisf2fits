"""Independent XISF fixtures and strict FITS validation (no production serializers)."""
import base64
import ctypes
import ctypes.util
import hashlib
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zlib

import lz4.block
import numpy as np
import zstandard
from astropy.io import fits

BINARY = str(Path(sys.argv.pop(1)).resolve())
FAULTS = str(Path(sys.argv.pop(1)).resolve()) if len(sys.argv) > 1 and sys.argv[1].endswith(".so") else None
NS = "http://www.pixinsight.com/xisf"
PIXELS = np.array([[0, 1, 255, 256], [32767, 32768, 65534, 65535], [17, 312, 10000, 60000]], dtype=np.uint16)


def fixture(path, pixels=PIXELS, keywords=(), properties=(), codec=None, shuffle=False,
            subblocks=False, checksum="sha-256", bad_checksum=False, location="attachment",
            image_attrs=None, extra=(), raw_override=None, payload_override=None, siblings=()):
    """Write an XISF envelope directly, independently of converter/libXISF code."""
    pixels = np.asarray(pixels, dtype=np.uint16)
    raw = pixels.astype("<u2").tobytes() if raw_override is None else raw_override
    if shuffle:
        raw = raw[0::2] + raw[1::2]
    def compress(data):
        if codec in ("lz4", "lz4hc"):
            return lz4.block.compress(data, store_size=False, mode="high_compression" if codec == "lz4hc" else "default")
        if codec == "zlib":
            return zlib.compress(data)
        if codec == "zstd":
            return zstandard.ZstdCompressor().compress(data)
        return data
    chunks = [raw[:len(raw)//2], raw[len(raw)//2:]] if subblocks else [raw]
    compressed = [compress(chunk) for chunk in chunks]
    stored = b"".join(compressed) if payload_override is None else payload_override
    attrs = {"geometry": f"{pixels.shape[1]}:{pixels.shape[0]}:1", "sampleFormat": "UInt16", "colorSpace": "Gray"}
    if codec:
        attrs["compression"] = codec + ("+sh" if shuffle else "") + f":{pixels.size * 2}" + (":2" if shuffle else "")
    if subblocks:
        attrs["subblocks"] = ":".join(f"{len(c)},{len(r)}" for c, r in zip(compressed, chunks))
    if checksum:
        digest = hashlib.new(checksum.lower().replace("sha3-", "sha3_").replace("-", ""), stored).hexdigest()
        attrs["checksum"] = checksum + ":" + ("0" * len(digest) if bad_checksum else digest)
    if location == "attachment":
        attrs["location"] = f"attachment:16384:{len(stored)}"
    else:
        attrs["location"] = location
    attrs.update(image_attrs or {})
    root = ET.Element("xisf", version="1.0", xmlns=NS)
    image = ET.SubElement(root, "Image", attrs)
    for attributes in siblings:
        ET.SubElement(root, "Image", attributes)
    if location == "inline:base64":
        image.text = base64.b64encode(stored).decode()
    elif location == "inline:base16":
        image.text = stored.hex()
    elif location == "embedded":
        ET.SubElement(image, "Data", encoding="base64").text = base64.b64encode(stored).decode()
    for name, value, comment in keywords:
        ET.SubElement(image, "FITSKeyword", name=name, value=value, comment=comment)
    metadata = ET.SubElement(root, "Metadata")
    for scope, identifier, typ, value, comment in properties:
        parent = image if scope == "IMAGE" else metadata
        p = ET.SubElement(parent, "Property", id=identifier, type=typ, comment=comment)
        if typ == "String":
            p.text = value
        else:
            p.set("value", value)
    for tag, attributes in extra:
        ET.SubElement(image, tag, attributes)
    xml = ET.tostring(root, encoding="utf-8")
    header = b"XISF0100" + struct.pack("<II", len(xml), 0) + xml
    assert location != "attachment" or len(header) <= 16384
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + (b"\0" * (16384 - len(header)) + stored if location == "attachment" else b""))
    return path


class ConversionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xisf2fits-integration-")
        self.root = Path(self.temp.name)
        self.src = self.root / "in.xisf"
        self.out = self.root / "out.fits.fz"

    def tearDown(self):
        self.temp.cleanup()

    def run_converter(self, *args, expected=0, cwd=None, env=None):
        r = subprocess.run([BINARY, *map(str, args)], text=True, capture_output=True, cwd=cwd, env=env, timeout=30)
        self.assertEqual(r.returncode, expected, r.stdout + r.stderr)
        return r

    def convert(self, *options):
        return self.run_converter(*options, self.src, self.out)

    def check_fits(self, expected=PIXELS, path=None):
        path = path or self.out
        # Check physical table HDU checksums as well as the virtual image header.
        with fits.open(path, disable_image_compression=True, checksum=True) as hdus:
            hdus.verify("exception")
            self.assertEqual(len(hdus), 2)
            for hdu in hdus:
                self.assertEqual(hdu.verify_checksum(), 1)
                self.assertEqual(hdu.verify_datasum(), 1)
            self.assertEqual(hdus[1].header["ZCMPTYPE"], "RICE_1")
        with fits.open(path, uint=True) as hdus:
            hdus.verify("exception")
            self.assertEqual(hdus[1].data.dtype, np.dtype("uint16"))
            np.testing.assert_array_equal(hdus[1].data, expected)
            return hdus[1].header.copy()

    def test_compression_and_storage_matrix(self):
        for codec in (None, "zlib", "lz4", "lz4hc", "zstd"):
            for shuffle in ((False, True) if codec else (False,)):
                for subblocks in ((False, True) if codec else (False,)):
                    for location in ("attachment", "inline:base64", "inline:base16", "embedded"):
                        with self.subTest(codec=codec, shuffle=shuffle, subblocks=subblocks, location=location):
                            fixture(self.src, codec=codec, shuffle=shuffle, subblocks=subblocks, location=location)
                            self.convert("--verify", "--overwrite")
                            self.check_fits()

    def test_legacy_nina_strings(self):
        values = {"OBJECT": "IC 1805", "DATE-OBS": "2023-09-09T00:17:01.721", "OBJCTRA": "02 32 42",
                  "OBJCTDEC": "+61 27 00", "TELESCOP": "TecnoSky AG70", "FILTER": "SII", "IMAGETYP": "LIGHT",
                  "FOCNAME": "ZWO Focuser (1)", "SWCREATE": "N.I.N.A. 3.0.0.1033 (x64)", "CUSTOM": "A/B camera"}
        fixture(self.src, keywords=[(k, v, "capture") for k, v in values.items()])
        self.convert("--verify")
        header = self.check_fits()
        for key, value in values.items():
            self.assertIsInstance(header[key], str)
            self.assertEqual(header[key], value)

    def test_typed_values_and_quotes(self):
        fixture(self.src, keywords=[("OBJECT", "1234", ""), ("FILTER", "T", ""), ("REAL", "1.234D-5", ""),
                  ("FLAG", "T", ""), ("COUNT", "9223372036854775807", ""), ("CUSTOM", "'a/b O''Brien'", ""),
                  ("COMPLEX", "(1.0, -2)", ""), ("EMPTY", "", "undefined"), ("EMPTYSTR", "''", "")])
        self.convert("--verify")
        h = self.check_fits()
        self.assertEqual(h["OBJECT"], "1234")
        self.assertEqual(h["FILTER"], "T")
        self.assertEqual(h["CUSTOM"], "a/b O'Brien")
        self.assertIs(h["FLAG"], True)
        self.assertEqual(h["COUNT"], 9223372036854775807)
        self.assertAlmostEqual(h["REAL"], 1.234e-5)
        self.assertEqual(h["COMPLEX"], complex(1, -2))
        self.assertIsNone(h["EMPTY"])
        self.assertEqual(h["EMPTYSTR"], "")

    def test_comment_history_both_attributes_and_order(self):
        fixture(self.src, keywords=[("COMMENT", "", "comment attribute"), ("HISTORY", "", "history one"),
                 ("HISTORY", "history two", "history three"), ("COMMENT", "same", "same")])
        self.convert("--verify")
        h = self.check_fits()
        self.assertEqual(list(h["COMMENT"]), ["comment attribute", "same"])
        self.assertEqual(list(h["HISTORY"]), ["history one", "history two", "history three"])

    def test_long_strings_and_hierarch(self):
        strings = {"OBJECT": "x" * 68, "LONGKEYWORD": "a/b ' &" * 40,
                   "TRAILAMP": "a" * 100 + "&", "QUOTE": "'" * 100,
                   "XISF.IMAGE.TEST": "a" * 140, "SHORTAMP": "&"}
        fixture(self.src, keywords=[(k, "'" + v.replace("'", "''") + "'", "") for k, v in strings.items()])
        self.convert("--verify")
        h = self.check_fits()
        for key, value in strings.items():
            self.assertEqual(h[key], value, key)
        self.assertEqual(h["LONGSTRN"], "OGIP 1.0")

    def test_unicode_and_comment_truncation_reported(self):
        fixture(self.src, keywords=[("FOCNAME", "'Observer’s focuser'", "é"), ("OBJECT", "'" + "x"*65 + "'", "comment" * 30),
                                   ("TELESCOP", "'Bob’s “scope” – 70mm'", ""), ("HISTORY", "tab\there", "")])
        r = self.convert("--verify")
        # Typographic quotes and dashes have exact equivalents; only lossy
        # replacements are reported, once per file.
        self.assertIn("ASCII-normalized keywords (lossy): FOCNAME, HISTORY\n", r.stderr)
        self.assertEqual(r.stderr.count("ASCII-normalized"), 1)
        self.assertIn("NOTE: header comments shortened to fit 80-character cards in 1 file", r.stderr)
        h = self.check_fits()
        self.assertEqual(h["FOCNAME"], "Observer's focuser")
        self.assertEqual(h["TELESCOP"], 'Bob\'s "scope" - 70mm')
        self.assertEqual(h["OBJECT"], "x"*65)

    def test_truncated_comments_summarized(self):
        keywords = [("OBJECT", "'" + "x" * 60 + "'", "object comment " * 3),
                    ("XISF.IMAGE.LONGNAME", "'value'", "hierarch comment " * 4),
                    ("LONGSTR", "'" + "s" * 150 + "'", "continued comment " * 4),
                    # The value fills the card, so the comment is dropped entirely.
                    ("FULL", "'" + "f" * 68 + "'", "dropped"),
                    ("SHORT", "1", "fits")]
        fixture(self.src, keywords=keywords)
        # Truncation is expected on real frames: one note per run, not a warning.
        r = self.convert("--verify")
        self.assertEqual(r.stderr, "NOTE: header comments shortened to fit 80-character cards in 1 file "
                                   "(values intact; --verbose lists keywords)\n")
        h = self.check_fits()
        self.assertEqual(h["LONGSTR"], "s" * 150)
        self.assertEqual(h.comments["SHORT"], "fits")
        r = self.convert("--verify", "--overwrite", "--verbose")
        self.assertEqual(r.stderr.splitlines(), [
            f"NOTE {str(self.src)!r}: truncated header comments: OBJECT, XISF.IMAGE.LONGNAME, LONGSTR, FULL".replace("'", '"'),
            "NOTE: header comments shortened to fit 80-character cards in 1 file (values intact)"])
        fixture(self.src, keywords=[("SHORT", "1", "fits")])
        self.assertEqual(self.convert("--verify", "--overwrite", "--verbose").stderr, "")
        # In a batch, the note counts successful files; --verbose lists each file's own keywords.
        batch = self.root / "batch"
        fixture(batch / "a.xisf", keywords=[("OBJECT", "'" + "x" * 60 + "'", "a" * 30)])
        fixture(batch / "b.xisf", keywords=[("FILTER", "'" + "x" * 60 + "'", "b" * 30), ("SHORT", "1", "fits")])
        fixture(batch / "c.xisf", keywords=[("SHORT", "1", "fits")])
        fixture(batch / "d.xisf", keywords=[("OBJECT", "'" + "x" * 60 + "'", "d" * 30)], bad_checksum=True)
        r = self.run_converter("--dry-run", batch, self.root / "batch-out")
        self.assertIn("in 3 files", r.stderr)  # Dry runs do not check source hashes.
        r = self.run_converter("--verify", batch, self.root / "batch-out", expected=1)
        self.assertEqual(r.stderr.count("NOTE"), 1)
        self.assertIn("NOTE: header comments shortened to fit 80-character cards in 2 files", r.stderr)
        r = self.run_converter("--verify", "--verbose", batch, self.root / "batch-out", expected=1)
        notes = sorted(l for l in r.stderr.splitlines() if "truncated header comments" in l)
        # d.xisf fails its source checksum before its metadata is prepared.
        self.assertEqual(len(notes), 2, r.stderr)
        self.assertTrue(notes[0].endswith('a.xisf": truncated header comments: OBJECT'), notes)
        self.assertTrue(notes[1].endswith('b.xisf": truncated header comments: FILTER'), notes)
        self.assertIn("in 2 files (values intact)", r.stderr)

    def test_properties_precision_and_conflicts(self):
        props = [("IMAGE", "Instrument:Telescope:Aperture", "Float32", "0.07", "[m] aperture"),
                 ("IMAGE", "Observation:Location:Latitude", "Float64", "45.60888888888889", "[deg] latitude"),
                 ("IMAGE", "Instrument:Focuser:Position", "Float32", "0", ""),
                 ("FILE", "XISF:CreationTime", "TimePoint", "2023-09-09T00:22:10.4442762Z", ""),
                 ("FILE", "XISF:CreatorApplication", "String", "N.I.N.A.", ""),
                 ("IMAGE", "Enabled", "Boolean", "true", ""),
                 ("IMAGE", "SomeVector", "F64Vector", "1,2", "")]
        fixture(self.src, keywords=[("FOCUSPOS", "2946", ""), ("SITELAT", "45.6088888888889", "")], properties=props,
                extra=[("Resolution", {"horizontal": "300", "vertical": "300", "unit": "inch"})])
        r = self.convert("--verify")
        h = self.check_fits()
        self.assertEqual(h["XISF.IMAGE.INSTRUMENT.TELESCOPE.APERTURE"], .07)
        self.assertIn("45.60888888888889", str(h.cards["XISF.IMAGE.OBSERVATION.LOCATION.LATITUDE"]))
        self.assertEqual(h["FOCUSPOS"], 2946)
        self.assertEqual(h["XISF.IMAGE.INSTRUMENT.FOCUSER.POSITION"], 0)
        self.assertIsInstance(h["XISF.IMAGE.INSTRUMENT.FOCUSER.POSITION"], float)
        self.assertEqual(h["XISF.FILE.XISF.CREATIONTIME"], "2023-09-09T00:22:10.4442762Z")
        self.assertIs(h["XISF.IMAGE.ENABLED"], True)
        self.assertIn("property conflicts with FOCUSPOS", r.stderr)
        self.assertIn("omitted unsupported property SomeVector", r.stderr)
        self.assertIn("omitted XISF image element: Resolution", r.stderr)

    def test_known_benign_property_differences(self):
        # N.I.N.A. writes Instrument:Focuser:Position as 0; a TimePoint's Z and
        # trailing fractional zeros do not change DATE-OBS. The default 72 dpi
        # display Resolution is omitted without a warning.
        keywords = [("SWCREATE", "'N.I.N.A. 3.1.2.9001 (x64)'", ""), ("FOCUSPOS", "3200", ""),
                    ("DATE-OBS", "'2023-11-17T18:15:09.635'", "")]
        props = [("IMAGE", "Instrument:Focuser:Position", "Float32", "0", ""),
                 ("IMAGE", "Observation:Time:Start", "TimePoint", "2023-11-17T18:15:09.6350Z", "")]
        fixture(self.src, keywords=keywords, properties=props,
                extra=[("Resolution", {"horizontal": "72", "vertical": "72", "unit": "inch"})])
        r = self.convert("--verify")
        self.assertNotIn("conflicts", r.stderr)
        self.assertNotIn("Resolution", r.stderr)
        h = self.check_fits()
        self.assertEqual((h["FOCUSPOS"], h["XISF.IMAGE.INSTRUMENT.FOCUSER.POSITION"]), (3200, 0.0))
        self.assertEqual(h["XISF.IMAGE.OBSERVATION.TIME.START"], "2023-11-17T18:15:09.6350Z")
        # Real differences are still reported: a nonzero NINA focuser property,
        # another time, and the zero focuser property from other software.
        for kw, prop, name in [(keywords, [("IMAGE", "Instrument:Focuser:Position", "Float32", "3100", "")], "FOCUSPOS"),
                               (keywords, [("IMAGE", "Observation:Time:Start", "TimePoint", "2023-11-17T18:15:09.636Z", "")], "DATE-OBS"),
                               (keywords[1:], props[:1], "FOCUSPOS")]:
            with self.subTest(name=name, prop=prop):
                fixture(self.src, keywords=kw, properties=prop)
                r = self.convert("--verify", "--overwrite")
                self.assertIn(f"property conflicts with {name}", r.stderr)

    def test_keyword_and_property_collisions_fail(self):
        for kwargs in [dict(keywords=[("OBJECT", "A", ""), ("OBJECT", "B", "")]),
                       dict(properties=[("IMAGE", "A:B", "String", "1", ""), ("IMAGE", "A.B", "String", "2", "")])]:
            with self.subTest(kwargs=kwargs):
                fixture(self.src, **kwargs)
                r = self.run_converter(self.src, self.out, expected=1)
                self.assertTrue("duplicate" in r.stderr or "collision" in r.stderr)
                self.assertFalse(self.out.exists())

    def test_source_checksum_always_checked(self):
        for option in ([], ["--verify"]):
            fixture(self.src, bad_checksum=True)
            r = self.run_converter(*option, self.src, self.out, expected=1)
            self.assertIn("source checksum mismatch", r.stderr)
            self.assertFalse(self.out.exists())
        r = self.convert("--dry-run")
        self.assertIn("source hashes not checked", r.stdout)

    def test_checksum_aliases(self):
        for algorithm in ("sha1", "sha-1", "sha256", "sha-256", "sha512", "sha-512", "sha3-256", "SHA3-512"):
            with self.subTest(algorithm=algorithm):
                fixture(self.src, checksum=algorithm)
                self.convert("--verify", "--overwrite")
                self.check_fits()

    def test_large_inline_block(self):
        # Over libxml2's default 10,000,000-byte text-node limit once encoded.
        pixels = (np.arange(2400 * 2200, dtype=np.uint32) * 7919 % 65536).astype(np.uint16).reshape(2200, 2400)
        for location in ("inline:base64", "embedded"):
            with self.subTest(location=location):
                fixture(self.src, pixels=pixels, location=location)
                self.convert("--overwrite")
                self.check_fits(pixels)

    def test_string_property_budget(self):
        # A small compressed block may advertise an arbitrary decoded size.
        big = {"id": "Big", "type": "String", "location": f"attachment:16384:{PIXELS.size * 2}",
               "compression": f"zlib:{1 << 30}"}
        fixture(self.src, extra=[("Property", big)])
        for option in ([], ["--dry-run"]):
            with self.subTest(option=option):
                r = self.run_converter(*option, self.src, self.out, expected=1)
                self.assertIn("string properties exceed 64 MiB", r.stderr)
                self.assertFalse(self.out.exists())

    def test_missing_checksum_explicit(self):
        fixture(self.src, checksum=None)
        r = self.convert("--verify")
        self.assertIn("source image checksum absent", r.stderr)
        self.assertNotIn("[source checksum verified]", r.stdout)

    def test_invalid_source_descriptors(self):
        cases = [{"geometry": "0:3:1"}, {"geometry": "18446744073709551615:3:1"},
                 {"geometry": "4x:3:1"}, {"location": "attachment:999999999:24"},
                 {"location": "attachment:1:24"}, {"byteOrder": "big"},
                 {"checksum": "sha-256:xyz"}, {"checksum": "md5:" + "0"*32},
                 {"compression": "lz4+sh:24:0"}, {"compression": "lz4:24", "subblocks": "2,30"}]
        for attrs in cases:
            with self.subTest(attrs=attrs):
                fixture(self.src, image_attrs=attrs)
                self.run_converter("--verify", self.src, self.out, expected=1)
                self.assertFalse(self.out.exists())

    def test_truncated_uncompressed_source(self):
        fixture(self.src, checksum=None)
        self.src.write_bytes(self.src.read_bytes()[:-4])
        r = self.run_converter("--verify", self.src, self.out, expected=1)
        self.assertIn("attachment outside source file", r.stderr)

    def test_short_and_invalid_decompression_even_with_valid_hash(self):
        for codec in ("zlib", "lz4", "lz4hc", "zstd"):
            for kwargs in (dict(raw_override=b"\x01\x00"*4), dict(payload_override=b"corrupt")):
                with self.subTest(codec=codec, kwargs=kwargs):
                    fixture(self.src, codec=codec, **kwargs)
                    r = self.run_converter("--verify", self.src, self.out, expected=1)
                    self.assertIn("decompression", r.stderr)
                    self.assertFalse(self.out.exists())

    def test_xml_and_signature_validation(self):
        for content in (b"short", b"XISF0100" + struct.pack("<II", 0xffffffff, 0),
                        b"XISF0100" + struct.pack("<II", 7, 0) + b"<broken",
                        b'<!DOCTYPE xisf [<!ENTITY x "bad">]><xisf xmlns="'+NS.encode()+b'" version="1.0"/>'):
            if content.startswith(b"<!DOCTYPE"):
                content = b"XISF0100" + struct.pack("<II", len(content), 0) + content
            self.src.write_bytes(content)
            self.run_converter(self.src, self.out, expected=1)

    def test_structural_keywords_cannot_change_pixel_interpretation(self):
        unsafe = ["BLANK", "TFIELDS", "THEAP", "TSCAL1", "TZERO1", "TNULL1", "TUNIT1", "ZSIMPLE", "ZEXTEND",
                  "ZBLOCKED", "ZTENSION", "ZPCOUNT", "ZGCOUNT", "ZQUANTIZ", "ZDITHER0", "ZBLANK", "ZSCALE", "ZZERO", "ZHEAPPTR"]
        fixture(self.src, keywords=[(name, "0", "stale") for name in unsafe] +
                [("NAXISFOO", "123", "custom"), ("TSCALX", "99", "custom"), ("EXTNAME", "'SCIENCE'", "")])
        self.convert("--verify")
        h = self.check_fits()
        self.assertEqual(h["XISF.FITS.NAXISFOO"], 123)
        self.assertEqual(h["TSCALX"], 99)
        self.assertEqual(h["EXTNAME"], "SCIENCE")
        self.assertNotIn("BLANK", h)

    def test_row_order_and_repeated_options(self):
        fixture(self.src)
        self.convert("--verify", "--row-order", "bottom-up")
        self.assertEqual(self.check_fits(PIXELS[::-1])["ROWORDER"], "BOTTOM-UP")
        self.convert("--verify", "--overwrite", "--row-order", "bottom-up", "--row-order", "top-down")
        self.assertEqual(self.check_fits()["ROWORDER"], "TOP-DOWN")

    def test_cfa_even_odd_heights_and_offsets(self):
        for height in (2, 3):
            for pattern in ("RGGB", "BGGR", "GRBG", "GBRG"):
                for bottom in (False, True):
                    with self.subTest(height=height, pattern=pattern, bottom=bottom):
                        pixels = PIXELS[:height]
                        fixture(self.src, pixels=pixels, keywords=[("BAYERPAT", f"'{pattern}'", ""), ("XBAYROFF", "1", ""), ("YBAYROFF", "1", "")])
                        self.convert("--verify", "--overwrite", "--row-order", "bottom-up" if bottom else "top-down")
                        h = self.check_fits(pixels[::-1] if bottom else pixels)
                        rows = np.array(list(pattern)).reshape(2, 2)
                        expected = "".join(rows[(y + 1 + (height-1 if bottom else 0)) % 2, (x+1) % 2] for y in range(2) for x in range(2))
                        self.assertEqual(h["BAYERPAT"], expected)
                        self.assertEqual((h["XBAYROFF"], h["YBAYROFF"]), (0, 0))

    def test_xml_cfa_and_conflict(self):
        extra = [("ColorFilterArray", {"width": "2", "height": "2", "pattern": "RGGB"})]
        fixture(self.src, pixels=PIXELS[:2], extra=extra)
        self.convert("--verify", "--row-order", "bottom-up")
        self.assertEqual(self.check_fits(PIXELS[:2][::-1])["BAYERPAT"], "GBRG")
        fixture(self.src, extra=extra, keywords=[("BAYERPAT", "'BGGR'", "")])
        self.run_converter("--overwrite", self.src, self.out, expected=1)
        # The XML CFA already describes the first pixel; a nonzero offset without
        # BAYERPAT is ambiguous, while a zero offset is not.
        fixture(self.src, pixels=PIXELS[:2], extra=extra, keywords=[("XBAYROFF", "1", "")])
        r = self.run_converter("--overwrite", self.src, self.out, expected=1)
        self.assertIn("ambiguous", r.stderr)
        fixture(self.src, pixels=PIXELS[:2], extra=extra, keywords=[("XBAYROFF", "0", "")])
        self.convert("--verify", "--overwrite")
        self.assertEqual(self.check_fits(PIXELS[:2])["BAYERPAT"], "RGGB")

    def test_bayer_padding_and_offset_forms(self):
        # Trailing blanks are insignificant in FITS strings.
        fixture(self.src, pixels=PIXELS[:2], keywords=[("BAYERPAT", "'RGGB    '", "")])
        self.convert("--verify", "--overwrite")
        self.assertEqual(self.check_fits(PIXELS[:2])["BAYERPAT"], "RGGB")
        # Equal offsets written differently must not conflict.
        fixture(self.src, pixels=PIXELS[:2], keywords=[("BAYERPAT", "'RGGB'", ""), ("XBAYROFF", "1", ""),
                                                       ("XBAYEROFF", "+1", "")])
        self.convert("--verify", "--overwrite")
        h = self.check_fits(PIXELS[:2])
        self.assertEqual((h["BAYERPAT"], h["XBAYROFF"]), ("GRBG", 0))

    def test_zero_bayer_offsets_without_pattern(self):
        for offsets in ([("XBAYROFF", "0", ""), ("YBAYROFF", "+000", "")], [("XBAYEROFF", "-0", "")],
                        [("XBAYROFF", "0", ""), ("YBAYROFF", "0", ""), ("XBAYEROFF", "0", ""), ("YBAYEROFF", "-0", "")]):
            with self.subTest(offsets=offsets):
                fixture(self.src, keywords=offsets)
                r = self.convert("--verify", "--overwrite")
                names = ", ".join(name for name, _, _ in offsets)
                self.assertIn(f"omitted zero Bayer offsets without a pattern: {names}\n", r.stderr)
                h = self.check_fits()
                for name, _, _ in offsets:
                    self.assertNotIn(name, h)
                self.assertNotIn("BAYERPAT", h)
                self.run_converter("--dry-run", self.src, self.root / "dry.fits.fz")
        fixture(self.src, keywords=[("SHORT", "1", "")])
        self.assertNotIn("Bayer", self.convert("--overwrite").stderr)

    def test_invalid_bayer_offsets_and_patterns_fail(self):
        extra = [("ColorFilterArray", {"width": "2", "height": "2", "pattern": "RGGB"})]
        cases = [  # Nonzero offsets need BAYERPAT, even when their parity is zero.
            dict(keywords=[("XBAYROFF", "1", "")]), dict(keywords=[("YBAYROFF", "2", "")]),
            dict(keywords=[("XBAYROFF", "-2", "")]), dict(keywords=[("XBAYROFF", "9" * 30, "")]),
            dict(keywords=[("XBAYROFF", "0", ""), ("XBAYEROFF", "2", "")]),
            dict(keywords=[("XBAYROFF", "0.0", "")]), dict(keywords=[("YBAYROFF", "'0'", "")]),
            dict(keywords=[("YBAYROFF", "2", "")], extra=extra), dict(keywords=[("XBAYROFF", "-2", "")], extra=extra),
            dict(keywords=[("XBAYROFF", "0", ""), ("XBAYEROFF", "2", "")], extra=extra),
            dict(keywords=[("BAYERPAT", "'RGGB'", ""), ("XBAYROFF", "T", "")]),
            # A present but empty pattern must not replace an XML CFA.
            dict(keywords=[("BAYERPAT", "''", "")]), dict(keywords=[("BAYERPAT", "'    '", "")]),
            dict(keywords=[("BAYERPAT", "", "")]),
            dict(keywords=[("BAYERPAT", "''", "")], extra=extra), dict(keywords=[("BAYERPAT", "'  '", "")], extra=extra),
            dict(keywords=[("BAYERPAT", "", "")], extra=extra)]
        for case in cases:
            with self.subTest(case=case):
                self.out.unlink(missing_ok=True)
                fixture(self.src, pixels=PIXELS[:2], **case)
                self.run_converter("--overwrite", self.src, self.out, expected=1)
                self.run_converter("--dry-run", self.src, self.root / "dry.fits.fz", expected=1)
                self.assertFalse(self.out.exists())

    def test_xml_cfa_zero_offsets_and_bayerpat_parity(self):
        extra = [("ColorFilterArray", {"width": "2", "height": "2", "pattern": "RGGB"})]
        fixture(self.src, pixels=PIXELS[:2], extra=extra,
                keywords=[("XBAYROFF", "+0", ""), ("YBAYROFF", "-0", ""), ("XBAYEROFF", "000", "")])
        self.convert("--verify", "--overwrite")
        h = self.check_fits(PIXELS[:2])
        self.assertEqual((h["BAYERPAT"], h["XBAYROFF"], h["YBAYROFF"]), ("RGGB", 0, 0))
        self.assertNotIn("XBAYEROFF", h)
        # With BAYERPAT, even nonzero offsets shift nothing and large tokens are not narrowed.
        fixture(self.src, pixels=PIXELS[:2], keywords=[("BAYERPAT", "'RGGB'", ""), ("XBAYROFF", "-2", ""),
                                                       ("YBAYROFF", "9" * 30, ""), ("YBAYEROFF", "1", "")])
        self.convert("--verify", "--overwrite")
        self.assertEqual(self.check_fits(PIXELS[:2])["BAYERPAT"], "GBRG")

    def test_source_continue_cards(self):
        fixture(self.src, keywords=[("OBJECT", "'first&'", ""), ("CONTINUE", "' second&'", ""),
                                   ("CONTINUE", "' O''Brien'", "source comment")])
        self.convert("--verify")
        h = self.check_fits()
        self.assertEqual(h["OBJECT"], "first second O'Brien")
        self.assertEqual(h.comments["OBJECT"], "source comment")

    def test_long_names_and_commentary(self):
        fixture(self.src, keywords=[("X" * 64, "''''", ""), ("HISTORY", "a" * 160, ""),
                                   ("COMMENT", "", ""), ("", "blank-name commentary", "")])
        self.convert("--verify")
        h = self.check_fits()
        self.assertEqual(h["X" * 64], "'")
        self.assertEqual("".join(h["HISTORY"]), "a" * 160)

    def test_numeric_property_ranges(self):
        for typ, value in [("UInt8", "-1"), ("UInt8", "256"), ("Int8", "128"), ("Int64", "9223372036854775808"),
                           ("Float32", "1e100"), ("Float64", "NaN")]:
            fixture(self.src, properties=[("IMAGE", "Test", typ, value, "")])
            self.run_converter(self.src, self.out, expected=1)

    def test_large_numeric_keyword_fails_cleanly(self):
        fixture(self.src, keywords=[("CUSTOM", "9" * 9000, "")])
        r = self.run_converter(self.src, self.out, expected=1)
        self.assertIn("FITS numeric value too long: CUSTOM", r.stderr)
        self.assertFalse(self.out.exists())

    def test_wcs_and_astrometry_guards(self):
        for name in ("CTYPE1", "CRPIX2A", "CD1_2", "PC3_2", "A_ORDER", "WCSAXES"):
            fixture(self.src, keywords=[(name, "'RA---TAN'" if name == "CTYPE1" else "1", "")])
            self.run_converter("--row-order", "bottom-up", self.src, self.out, expected=1)
        fixture(self.src, properties=[("IMAGE", "PCL:AstrometricSolution:ProjectionSystem", "String", "TAN", "")])
        self.run_converter(self.src, self.out, expected=1)

    def test_literal_filenames(self):
        for name in ("bracket[1]", "paren(template)", "!bang", "file.gz", "brace{1}", "space ' quote"):
            with self.subTest(name=name):
                directory = self.root / name
                src = fixture(directory / (name + ".xisf"))
                out = directory / (name + ".fits.fz")
                self.run_converter("--verify", src, out)
                self.check_fits(path=out)
                self.assertEqual({p.name for p in directory.iterdir()}, {src.name, out.name})

    def test_cli_separator_and_version(self):
        src = fixture(self.root / "-input.xisf")
        self.run_converter("--verify", "--", src.name, "-output.fits.fz", cwd=self.root)
        self.check_fits(path=self.root / "-output.fits.fz")
        self.assertRegex(self.run_converter("--version").stdout, r"^xisf2fits \d+\.\d+\.\d+\n$")

    def test_jobs_option(self):
        fixture(self.src)
        for value in ("x", "-1", ""):
            r = self.run_converter("--jobs", value, self.src, self.out, expected=1)
            self.assertIn("invalid job count", r.stderr)
        self.assertIn("--jobs requires a number", self.run_converter(self.src, self.out, "-j", expected=1).stderr)
        # A single file is one job; 0 selects every hardware thread.
        self.run_converter("-j", "0", "--verify", self.src, self.out)
        self.check_fits()

    def test_failed_overwrite_preserves_output(self):
        fixture(self.src)
        self.convert("--verify")
        old = self.out.read_bytes()
        fixture(self.src, bad_checksum=True)
        self.run_converter("--overwrite", self.src, self.out, expected=1)
        self.assertEqual(self.out.read_bytes(), old)
        self.assertFalse(list(self.root.glob(".xisf2fits-*")))

    def test_batch_existing_outputs_verified(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        fixture(srcdir / "frame.xisf", keywords=[("OBJECT", "IC 1805", "")])
        self.run_converter("--verify", srcdir, outdir)
        r = self.run_converter("--verify", srcdir, outdir)
        self.assertIn("existing output verified", r.stdout)
        path = outdir / "frame.fits.fz"
        # A change in the source metadata must invalidate an otherwise intact output.
        fixture(srcdir / "frame.xisf", keywords=[("OBJECT", "IC 1848", "")])
        r = self.run_converter("--verify", srcdir, outdir, expected=1)
        self.assertIn("regenerate using --overwrite --verify", r.stderr)
        self.assertTrue(path.exists())

    def test_both_hdu_checksums_checked(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        fixture(srcdir / "frame.xisf")
        for hdu in (0, 1):
            self.run_converter("--verify", "--overwrite", srcdir, outdir)
            path = outdir / "frame.fits.fz"
            with fits.open(path, disable_image_compression=True) as hdus:
                offset = hdus[hdu].fileinfo()["hdrLoc"]
            raw = bytearray(path.read_bytes())
            index = raw.index(b"CHECKSUM", offset)
            raw[index+11] = ord("0") if raw[index+11] != ord("0") else ord("1")
            path.write_bytes(raw)
            r = self.run_converter("--verify", srcdir, outdir, expected=1)
            self.assertIn("checksum", r.stderr)

    def test_batch_collision_and_unsupported_image(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        fixture(srcdir / "a.xisf")
        fixture(srcdir / "a.XISF")
        fixture(srcdir / "rgb.xisf", image_attrs={"colorSpace": "RGB"})
        fixture(srcdir / "good.xisf")
        sibling = {"geometry": "4:3:1", "colorSpace": "Gray", "location": "attachment:16384:24"}
        fixture(srcdir / "master.xisf", image_attrs={"id": "integration", "sampleFormat": "Float32"},
                siblings=[{**sibling, "id": "rejection_low", "sampleFormat": "Float32"}])
        fixture(srcdir / "pair.xisf", image_attrs={"id": "a"}, siblings=[{**sibling, "id": "b", "sampleFormat": "UInt16"}])
        r = self.run_converter("--verify", srcdir, outdir, expected=1)
        self.assertIn("destination collision", r.stderr)
        self.assertIn("PixInsight integration master (integration Float32, rejection_low Float32)", r.stdout)
        self.assertIn("multi-image XISF (a UInt16, b UInt16)", r.stdout)
        self.assertIn("3 skipped, 2 failed", r.stdout)
        self.assertFalse((outdir / "a.fits.fz").exists())
        self.check_fits(path=outdir / "good.fits.fz")

    def test_batch_case_collision(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        fixture(srcdir / "x" / "a.xisf")
        if (srcdir / "X" / "a.xisf").exists():
            self.skipTest("source filesystem is case-insensitive")
        fixture(srcdir / "X" / "a.xisf")
        fixture(srcdir / "good.xisf")
        # Refused even on case-sensitive destinations, where both could coexist.
        r = self.run_converter("--verify", srcdir, outdir, expected=1)
        self.assertEqual(r.stderr.count("destination collision"), 2)
        self.assertIn("1 converted, 0 skipped, 2 failed", r.stdout)
        self.assertFalse((outdir / "x" / "a.fits.fz").exists())
        self.assertFalse((outdir / "X" / "a.fits.fz").exists())
        self.check_fits(path=outdir / "good.fits.fz")

    def test_batch_jobs(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        # Concurrent workers create the same new output directories.
        names = [Path(f"t{t}", f"s{s}", f"f{n}.xisf") for t in range(3) for s in range(2) for n in range(3)]
        for name in names:
            fixture(srcdir / name, keywords=[("OBJECT", f"'{name.stem}'", "")])
        fixture(srcdir / "t0" / "corrupt.xisf", bad_checksum=True)
        fixture(srcdir / "t1" / "rgb.xisf", image_attrs={"colorSpace": "RGB"})
        r = self.run_converter("--jobs", "4", "--verify", srcdir, outdir, expected=1)
        self.assertIn("18 converted, 1 skipped, 1 failed", r.stdout)
        # Each file's result is printed whole, on its own line.
        results = r.stdout.splitlines()[:-1]
        self.assertEqual(len(results), 19)
        self.assertTrue(all(line.startswith(("CONVERTED \"", "SKIPPED \"")) for line in results), results)
        self.assertEqual(r.stderr.count("FAILED "), 1)
        for name in names:
            self.assertEqual(self.check_fits(path=outdir / name.with_suffix(".fits.fz"))["OBJECT"], name.stem)
        self.assertFalse(list(outdir.rglob(".xisf2fits-*")))
        r = self.run_converter("-j", "0", "--verify", srcdir, outdir, expected=1)
        self.assertIn("0 converted, 19 skipped, 1 failed", r.stdout)

    def test_batch_unreadable_directory(self):
        if os.geteuid() == 0:
            self.skipTest("permission test requires unprivileged user")
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        fixture(srcdir / "blocked" / "a.xisf")
        fixture(srcdir / "good.xisf")
        (srcdir / "blocked").chmod(0)
        try:
            r = self.run_converter("--verify", srcdir, outdir, expected=1)
            self.assertIn("Permission denied", r.stderr)
            self.check_fits(path=outdir / "good.fits.fz")
        finally:
            (srcdir / "blocked").chmod(0o700)

    def test_dry_run_creates_nothing(self):
        fixture(self.src)
        dst = self.root / "nested" / "dir" / "out.fits.fz"
        self.run_converter("--dry-run", "--verify", self.src, dst)
        self.assertFalse(dst.parent.exists())

    def fault_environment(self, fault):
        if not FAULTS:
            self.skipTest("fault interposition requires Linux")
        env = os.environ.copy()
        libraries = [env.get("XISF2FITS_TEST_ASAN_RUNTIME"), FAULTS, env.get("LD_PRELOAD")]
        env["LD_PRELOAD"] = ":".join(p for p in libraries if p)
        env["XISF2FITS_TEST_FAULT"] = fault
        env["XISF2FITS_TEST_DEST"] = str(self.out)
        env["XISF2FITS_TEST_SYNC_LOG"] = str(self.root / "sync.log")
        return env

    def test_publication_does_not_require_hardlinks(self):
        fixture(self.src)
        self.run_converter("--verify", self.src, self.out, env=self.fault_environment("no-hardlinks"))
        self.check_fits()
        self.assertEqual((self.root / "sync.log").read_text(), "FDD")

    def test_published_output_survives_stale_temporary(self):
        fixture(self.src)
        self.run_converter("--verify", self.src, self.out, env=self.fault_environment("stale-temporary"))
        self.check_fits()
        # The blocked removal leaves the private temporary directory behind.
        self.assertTrue(list(self.root.glob(".xisf2fits-*/image.tmp")))

    def test_publication_race_cannot_clobber_destination(self):
        fixture(self.src)
        self.run_converter("--verify", self.src, self.out, expected=1, env=self.fault_environment("publish-race"))
        self.assertEqual(self.out.read_text(), "race winner")
        self.assertFalse(list(self.root.glob(".xisf2fits-*")))

    def test_failed_file_sync_preserves_existing_destination(self):
        fixture(self.src)
        self.out.write_text("previous output")
        r = self.run_converter("--verify", "--overwrite", self.src, self.out, expected=1, env=self.fault_environment("file-sync-error"))
        self.assertIn("synchronize", r.stderr)
        self.assertEqual(self.out.read_text(), "previous output")
        self.assertFalse(list(self.root.glob(".xisf2fits-*")))

    def test_directory_sync_failure_reports_complete_published_file(self):
        fixture(self.src)
        r = self.run_converter("--verify", self.src, self.out, expected=1, env=self.fault_environment("directory-sync-error"))
        self.assertIn("synchronize", r.stderr)
        self.check_fits()

    def test_signal_cleans_up_before_publication(self):
        fixture(self.src)
        self.out.write_text("previous output")
        r = self.run_converter("--overwrite", self.src, self.out, expected=128+signal.SIGTERM, env=self.fault_environment("interrupt"))
        self.assertIn("interrupted", r.stderr)
        self.assertEqual(self.out.read_text(), "previous output")
        self.assertFalse(list(self.root.glob(".xisf2fits-*")))

    def test_signal_stops_all_jobs_and_cleans_up(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        for n in range(8):
            fixture(srcdir / f"d{n}" / "frame.xisf")
        # Every worker raises SIGTERM when syncing its temporary file.
        r = self.run_converter("--jobs", "4", srcdir, outdir, expected=128+signal.SIGTERM, env=self.fault_environment("interrupt"))
        self.assertIn("interrupted", r.stderr)
        self.assertNotIn("Summary", r.stdout)
        # Temporary and newly created directories are removed; outdir itself may
        # remain if another worker still used it when its creator cleaned up.
        self.assertEqual(list(outdir.rglob("*")) if outdir.exists() else [], [])

    def test_invalid_header_rejected_even_with_valid_checksums(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        fixture(srcdir / "frame.xisf", keywords=[("OBJECT", "IC 1805", "")])
        self.run_converter("--verify", srcdir, outdir)
        path = outdir / "frame.fits.fz"
        cf = ctypes.CDLL(ctypes.util.find_library("cfitsio"))
        ptr, status, typ = ctypes.c_void_p(), ctypes.c_int(), ctypes.c_int()
        cf.ffdkopn(ctypes.byref(ptr), str(path).encode(), 1, ctypes.byref(status))
        cf.ffmahd(ptr, 2, ctypes.byref(typ), ctypes.byref(status))
        cf.ffucrd(ptr, b"OBJECT", b"OBJECT  = IC 1805", ctypes.byref(status))
        cf.ffpcks(ptr, ctypes.byref(status))
        cf.ffclos(ptr, ctypes.byref(status))
        self.assertEqual(status.value, 0)
        r = self.run_converter("--verify", srcdir, outdir, expected=1)
        self.assertIn("invalid unquoted FITS value: OBJECT", r.stderr)

    def test_primary_header_syntax_and_duplicates(self):
        srcdir, outdir = self.root / "sources", self.root / "outputs"
        fixture(srcdir / "frame.xisf")
        cf = ctypes.CDLL(ctypes.util.find_library("cfitsio"))
        for records, expected in [([b"BAD_KEY = 1"], "invalid FITS keyword name"),
                                  ([b"TEST    = 1", b"TEST    = 2"], "duplicate FITS keyword")]:
            self.run_converter("--verify", "--overwrite", srcdir, outdir)
            ptr, status = ctypes.c_void_p(), ctypes.c_int()
            cf.ffdkopn(ctypes.byref(ptr), str(outdir / "frame.fits.fz").encode(), 1, ctypes.byref(status))
            for record in records:
                cf.ffprec(ptr, record, ctypes.byref(status))
            cf.ffpcks(ptr, ctypes.byref(status))
            cf.ffclos(ptr, ctypes.byref(status))
            self.assertEqual(status.value, 0)
            if records[0].startswith(b"BAD_KEY"):
                # CFITSIO refuses to write an illegal keyword name itself.
                # Syntax is checked before checksums, so corrupt it on disk.
                path = outdir / "frame.fits.fz"
                path.write_bytes(path.read_bytes().replace(b"BAD_KEY =", b"BAD!KEY =", 1))
            r = self.run_converter("--verify", srcdir, outdir, expected=1)
            self.assertIn(expected, r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
