# Fixes and open issues

The conversion contract is in README.md. This file records what changed in
each release and the design decisions for issues that remain open.

## Unreleased

- **`-j`/`--jobs N` converts files in parallel in directory mode.** The default
  stays at 1; `0` uses every hardware thread. On the reference archive's
  single hard disk, 8 jobs converted cold NINA frames with `--verify` 4.4
  times faster than one (5.1 instead of 1.15 frames per second), and more jobs
  added only seeking. Each file's diagnostics are printed together. Libraries
  that initialize lazily (libxml2, CFITSIO, OpenSSL digests) are initialized
  before workers start. CFITSIO's checksum timestamps, which use the
  non-reentrant `gmtime()`, are written one file at a time. New output
  directories are created and removed under a lock, so one worker cannot
  remove a directory just before another uses it. ThreadSanitizer reports no
  races in conversion, verification, overwrite, or dry-run batches of real
  frames.
- **Multi-image skips say what the file is.** The 141 PixInsight masters in
  the reference archive were skipped as "multi-image XISF unsupported", which
  hid that their images are Float32. The reason now lists each image's id and
  sample format and names integration masters, for example
  `PixInsight integration master (integration Float32, rejection_low Float32,
  rejection_high Float32)`. Masters are still skipped.
- **The default `Resolution` is omitted silently.** N.I.N.A. 3.0.0.1075 writes
  `<Resolution horizontal="72" vertical="72" unit="inch"/>`, the XISF default
  display resolution, which says nothing about the optics. It accounted for 42
  of the 43 diagnostic lines in a 0.4 dry run. Other values are still reported.

## 0.4.0

Checked against a 12,843-frame N.I.N.A. 3.0–3.2 archive: a dry run went from
178,042 diagnostic lines to 43, with no failures. Converted outputs are
identical to 0.3 apart from checksum timestamps.

- **Truncated header comments are summarized.** Every NINA frame has about a
  dozen comments cut to fit 80-character cards, mostly on long
  `HIERARCH XISF.IMAGE.*` property names whose capture keywords keep the full
  comment. Instead of one warning per card, each run prints one `NOTE` counting
  the affected files. The new `--verbose` option lists each file's keywords.
- **Known-benign property differences are not reported.** N.I.N.A. writes
  `Instrument:Focuser:Position` as 0 whatever the real position, and XISF
  TimePoints end in a `Z` that FITS `DATE-OBS` omits. Both are still retained.
- **Typographic characters are normalized silently.** Curly quotes and dashes
  have exact ASCII equivalents; only lossy replacements are reported, in one
  warning per file.
- **Bayer offsets are checked as written, not by parity.** Offsets that are all
  zero with no pattern are omitted with a warning. A nonzero offset without
  `BAYERPAT` fails, including even values such as `2` that 0.3 accepted with an
  XML CFA. An empty `BAYERPAT` fails; it previously replaced the XML CFA with
  `BAYERPAT=''`.
- **Tests:** the version test no longer expects `0.2.0`, and
  `validate_archive.py` handles SHA3 checksums and images without a checksum.

## 0.3.0

- SHA3-256 and SHA3-512 source checksums; SHA-1/SHA-2 names are accepted with
  or without a hyphen.
- Decoded block-backed string properties are capped at 64 MiB in total,
  checked before decompression.
- Inline blocks larger than libxml2's 10 MB text-node default are accepted
  within the 64 MiB header limit.
- Destinations differing only in ASCII letter case collide in every batch.
- Trailing blanks in `BAYERPAT` are ignored, and equivalent offsets with
  different spellings no longer conflict.
- A blocked temporary-link cleanup after hard-link publication no longer
  fails a completed conversion.
- `BUILD_TESTING=ON` without the Python test packages registers only the C++
  tests instead of failing configuration.

## Open issues

These are listed under Known issues in README.md. None of them affects valid
NINA archives on local filesystems.

- **Directory fsync rejected with EINVAL.** Some network mounts reject `fsync`
  on directories, failing a file whose output is already complete. Only
  directory `fsync` with `EINVAL`, `EOPNOTSUPP`, or `ENOSYS` should become a
  warning, aggregated into one per file. File sync errors, directory `EIO`, and
  open/close failures must still fail. `syncPath` would append to a warning list
  supplied by `TemporaryOutput` and `publish`. Extend the fault interposer with
  a configurable errno to test this.
- **One unrepresentable property fails the whole file.** A NaN, infinite,
  malformed, or out-of-range numeric property or an invalid Boolean should be
  omitted with a warning (`omitted IMAGE property Test: <reason>`), keeping
  the rest. Empty IDs and keyword collisions must still fail, so check empty IDs
  before any omission. Do not wrap metadata preparation in a broad exception
  handler: checksum, CFA, duplicate-keyword, and astrometry failures must
  remain failures.
- **Decoded image size is unbounded.** A small compressed block declaring a
  huge geometry allocates accordingly before decompression fails. Planned:
  `--max-image-megabytes N` (MiB, default 1024, `0` disables), checked against
  `width * height * 2` in `readSource` right after the dimension overflow
  checks, before any block is decoded. Exceeding it is a failure, not a skip.
  It bounds decoded pixels only; peak memory is about twice that plus the
  stored block.
