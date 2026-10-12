# xisf2fits

`xisf2fits` converts single-image, unsigned 16-bit grayscale camera XISFs to
losslessly Rice-compressed FITS (`.fits.fz`). It supports raw mono and 2×2 Bayer
frames, including old N.I.N.A. files with unquoted text metadata. Source files
are never modified.

Version 0.4 reduces diagnostics on real NINA archives to actionable warnings
and tightens Bayer metadata checks; outputs are otherwise unchanged from 0.3.
Existing FITS files produced by 0.1 should be regenerated from their XISFs with
`--overwrite --verify`. See [fixes and open issues](docs/FIXES.md) for release
notes.

## Build

Requirements: a C++17 compiler, CMake 3.16+, pkg-config, and development packages
for CFITSIO, libxml2, OpenSSL Crypto, LZ4, Zstandard, and zlib 1.2.9+.

```sh
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --parallel
./build/xisf2fits --version
```

Single-configuration builds default to Release. Install with
`cmake --install build --prefix "$HOME/.local"`.

The converter parses its supported XISF subset using libxml2 and calls the
compression codecs directly. It no longer depends on libXISF: version 0.2.13
could ignore decompression errors or short results, and did not validate source
checksums. Every decoded block must now have exactly its advertised size.

## Usage

```text
xisf2fits [OPTIONS] INPUT.xisf OUTPUT.fits.fz
xisf2fits [OPTIONS] INPUT_DIRECTORY OUTPUT_DIRECTORY
```

A single-file output may be an existing directory. Directory conversion mirrors
the input tree, changing `.xisf` to `.fits.fz`, and accepts extensions in any
letter case. File symlinks are read without changing the lexical destination
path; directory symlinks are not followed.

```sh
./build/xisf2fits --verify input.xisf out.fits.fz
./build/xisf2fits --dry-run ~/astro/raw ./fits-output
./build/xisf2fits --verify ~/astro/raw ./fits-output
```

| Option | Behavior |
| --- | --- |
| `--verify` | Reread output pixels and null masks; check dimensions, unsigned scaling, Rice compression, metadata, and checksums in both HDUs. In a batch, verify existing outputs before skipping them. |
| `--dry-run` | Validate source headers, block bounds, metadata representability, and planned destinations without writing files or decoding pixels. Source block hashes are **not** checked. Attached string properties are deferred until conversion. |
| `--overwrite` | Replace an existing output after successful conversion and optional output verification. |
| `--row-order ORDER` | `top-down` (default) or `bottom-up`. Repeated options use the last value. |
| `--verbose` | For each file, list the keywords whose header comments were shortened. |
| `--version` | Print the program version. |
| `-h`, `--help` | Print help. |
| `--` | End option parsing; subsequent arguments are filenames. |

Without `--overwrite`, an existing single-file output is an error. In directory
mode, existing regular outputs are skipped without inspection unless `--verify`
is used. A verified rerun reads the source again and rejects outdated, corrupt,
or mismatched outputs; it does not repair them automatically. Verification
compares the converter's canonical metadata records, so an externally edited
header with different formatting can require regeneration even if values match.

Unsupported sample formats, RGB images, and multi-image containers are skipped
in batches. Malformed data is a failure. Directory-access failures are reported
while other directories continue. Inputs mapping to the same destination are
reported as collisions before either is converted. Destinations differing only
in ASCII letter case (`M31/a.xisf` and `m31/a.xisf`) also collide, even on
case-sensitive filesystems, so a batch cannot silently lose files when copied to
or written on a case-insensitive one. Exit status is `1` if any
file/directory failed, otherwise `0`; interruption returns `128 + signal`.

## Pixels and source integrity

Supported storage: attachment, inline base64/base16, and embedded data;
uncompressed, zlib, LZ4, LZ4HC, or Zstandard; compressed subblocks and byte
shuffling. Only little-endian UInt16, single-channel Gray images are accepted.
The XML header, including inline and embedded blocks, is limited to 64 MiB, and
decoded string properties to 64 MiB in total. DTDs and external data locations
are rejected.
Dimensions, arithmetic, source block extents, compression descriptors, and exact
decompressed sizes are checked before output creation.

Every declared source block checksum is validated during a conversion, **even
without `--verify`**. SHA-1, SHA-256, SHA-512, SHA3-256, and SHA3-512 are supported;
the SHA-1/SHA-2 names are accepted with or without a hyphen. The digest covers stored bytes before decompression and byte
unshuffling. An invalid, unsupported, or mismatched declared checksum is an
error. An image without a checksum produces a warning: comparing decoded pixels
with FITS cannot establish whether the source was already damaged. Source hashes
do not authenticate the XML metadata.

Each output contains an empty primary HDU and a compressed image extension.
Metadata is on **HDU 2**, alongside `BITPIX=16`, `BZERO=32768`, and `BSCALE=1`.
Do not inspect only the empty primary header. New FITS `CHECKSUM`/`DATASUM` cards
are written for both HDUs.

Rows remain in XISF order by default (`ROWORDER='TOP-DOWN'`). `bottom-up` reverses
them and writes `ROWORDER='BOTTOM-UP'`. Row reversal is rejected when WCS or
distortion keywords are present. Ordinary NINA pointing hints such as RA, DEC,
and EQUINOX are retained. Supported Bayer patterns are normalized to the first
output pixel, with zero `XBAYROFF`/`YBAYROFF`; reversal accounts for image-height
parity. Trailing blanks in `BAYERPAT` are ignored; an empty, blank, or undefined
`BAYERPAT` fails. An XML `ColorFilterArray` already describes the first stored
pixel: FITS offsets apply only to `BAYERPAT`, so with an XML CFA and no
`BAYERPAT`, zero offsets are accepted and any nonzero offset (even `2`) is
ambiguous and fails. Without any pattern, offsets (`XBAYROFF`, `YBAYROFF`,
`XBAYEROFF`, `YBAYEROFF`) that are all zero are omitted with one warning, and a
nonzero offset fails. Conflicting XML/FITS CFA descriptions, non-integer
offsets, or unsupported patterns fail.

## Metadata contract

- Image-level FITS keywords remain available to conventional readers: target,
  pointing, observation time, camera, exposure, filter, optics, and WCS metadata.
  Legacy unquoted NINA strings are quoted and escaped. Known text keywords remain
  strings even when their contents are `T`, `F`, or numbers. Unknown unquoted
  custom values are inferred only when the entire value is a valid FITS scalar;
  their original type cannot always be reconstructed from old untyped headers.
- Undefined values are permitted; an explicitly quoted empty string remains a
  string. Long values use the `CONTINUE` convention, and extended names use
  `HIERARCH`. Existing source continuation sequences are joined before writing.
- COMMENT/HISTORY text can come from either XML attribute. Both distinct texts
  are retained, in value-then-comment order, and long commentary is split across
  cards. Identical duplicate singleton keywords are collapsed with a warning;
  conflicting duplicates fail.
- Scalar image and container properties use
  `HIERARCH XISF.IMAGE.<ID>` and `HIERARCH XISF.FILE.<ID>`. IDs are uppercased with
  colons changed to dots. For example, `Instrument:Telescope:Aperture` becomes
  `XISF.IMAGE.INSTRUMENT.TELESCOPE.APERTURE`, retaining its source units (meters).
  Numeric tokens retain their precision; integral floating values gain `.0` to
  preserve their FITS real type. Full timestamp precision is retained. Existing
  capture keywords are not overwritten by properties. Conflicts are reported
  and both values remain available; a property may be stale even when its FITS
  keyword is correct. A TimePoint matches `DATE-OBS` regardless of its trailing
  `Z` and insignificant fractional zeros. N.I.N.A. 3.x always writes
  `Instrument:Focuser:Position` as 0, so when `SWCREATE` names N.I.N.A. a zero
  value is retained without a conflict warning; `FOCUSPOS` holds the real
  position. Property-name collisions fail.
  Nonfinite or out-of-range numeric properties are rejected.
- Structural, scaling, null, compression-table, and checksum keywords are
  regenerated or excluded. Descriptive `EXTNAME` is retained. Custom NAXIS/ZNAXIS
  lookalike names are moved under `HIERARCH XISF.FITS.*`, with a warning, because
  strict readers can otherwise misinterpret them as axis definitions.

FITS headers are ASCII. Curly quotes and dashes are normalized to their ASCII
equivalents without a warning; other unsupported characters become `?`, and
control whitespace becomes spaces. Keywords affected by these lossy
replacements are listed in one warning per file. Keyword comments can be
truncated or dropped to fit the available card space. Every NINA frame has some,
mostly on long `HIERARCH XISF.*` property names whose matching capture keywords
keep the full comment, so this is not a warning: one `NOTE` per run counts the
affected files, and `--verbose` lists each file's keywords. Values are never
silently truncated. FITS readers may ignore insignificant trailing string
spaces.

Vectors, matrices, complex **properties**, properties that cannot fit even using
HIERARCH, thumbnails, ICC profiles, display settings, and other unsupported XML
elements are omitted and reported. Complex FITS keyword values are supported.
XISF image/container layout and compression attributes are not reproduced as
FITS metadata. Native PixInsight astrometric solutions are rejected because
copying their properties would not make a usable FITS WCS solution.

This is a conversion with explicit limitations, **not an exact XISF metadata
archive**. No original-XML extension or sidecar is created. Conventional readers
may ignore HIERARCH properties; the ordinary capture keywords remain usable.

## Publication and recovery

Output is written in a private `.xisf2fits-*` temporary directory beside its
intended destination. The completed file is flushed with `fsync` before atomic
publication. Linux no-replace rename avoids a hard-link requirement on
filesystems supporting that operation; hard links are a fallback. If neither
safe operation is supported, conversion fails rather than risking overwriting
an existing destination. `--overwrite` uses atomic replacement.

Affected directories are synchronized after publication. A directory-sync
failure is reported even if a complete output has already been published.
Durability ultimately depends on the filesystem, storage hardware, and network
server honoring these operations; SMB capabilities vary by server.

Exceptions and SIGINT/SIGTERM clean up temporary output and newly created empty
directories. SIGKILL, a crash, or power loss can leave private temporary
directories behind. They are not treated as completed outputs or automatically
removed, since another converter could still be using them. Existing output
files are not proof of success: use `--verify` when resuming an archive migration.

## Tests

Integration tests need an independent Python FITS reader and fixture encoders.
These are test-only dependencies; without them, the default `BUILD_TESTING=ON`
configuration warns and registers only the C++ unit tests:

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
cmake -S . -B build -DBUILD_TESTING=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Fixtures are generated directly from XML and explicit pixel arrays, independent
of the converter. Astropy strictly validates headers and compares all pixels,
metadata types/values, and both HDU checksums. Tests cover codecs, subblocks,
shuffle/storage variants, historical NINA metadata, corruption, CFA/WCS,
filename syntax, reruns, collisions, and cancellation/publication failures.
Linux fault tests use a test-only preloaded library; it is never installed with
the converter. Permission tests require an unprivileged user.

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Optional real-archive validation scans headers and chooses representatives of
each NINA version, plus a frame with PixInsight commentary. It independently
checks source hashes/decompression, every pixel, and capture/property metadata.
It writes only temporary outputs and does not modify the archive:

```sh
.venv/bin/python tests/validate_archive.py build/xisf2fits ~/astro/raw
```

## Supported platforms

Linux is the tested platform, including the fault-injection publication tests.
macOS may build but is untested; its `fsync` does not force a full flush to
stable storage (`F_FULLFSYNC` is not used), so durability guarantees are weaker.
Windows is not supported.

## Known issues

- Some filesystems and network mounts reject `fsync` on a directory with
  `EINVAL`. The file is then reported as failed even though the complete output
  was already published; `--verify` on a rerun confirms it.
- A single unrepresentable value fails the whole file rather than omitting that
  value: for example a NaN numeric property.
- The decoded image size is bounded only by its declared geometry and the
  exact-size checks, not by an application memory budget. A small compressed
  block declaring a very large image can allocate accordingly before its
  decompression fails.

## License

MIT. See [LICENSE](LICENSE).
