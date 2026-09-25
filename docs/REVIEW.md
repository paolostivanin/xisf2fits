# NINA XISF → FITS integrity review

Reviewed 2026-09-25. The original 0.1 converter preserved the decoded pixels in
the examined frame, but did not reliably preserve metadata or establish source
integrity. Its `[verified]` message could accompany invalid FITS headers,
unchecked corrupted input, and valid pixels marked as undefined.

## Archive evidence

A header-only inventory of `/mnt/photos/DeepSky` found 12,984 XISFs:
12,843 single-image NINA-origin frames and 141 multi-image containers. Of the
NINA-origin frames, 5,418 (42.2%) had unquoted `SWCREATE` values; these represent
the legacy unquoted-string problem. All 12,843 carried source image checksums.
There were 42 frames with COMMENT/HISTORY text in the `comment` attribute and
none with XML CFA elements. Some NINA-origin files had subsequently been edited
by PixInsight; their retained SWCREATE version does not identify the last writer.

The reported IC1805/SII frame reproduced `OBJECT = IC 1805`, an unquoted
DATE-OBS, and broken sexagesimal coordinates. CFITSIO parsed `IC 1805` as `IC`
and `+61 27 00` as `+61`, and classified `TecnoSky AG70` as logical. Its original
26,108,352 pixel values did match an independent LZ4 decode. This demonstrates
why a pixel-only comparison is insufficient.

## Findings and disposition

| Priority | Finding | Implemented remedy |
| --- | --- | --- |
| Critical | Legacy NINA text was emitted as invalid unquoted FITS values. | Explicit string handling, complete-token scalar inference, escaping, and independent strict FITS validation. |
| High | COMMENT/HISTORY stored in the XML comment attribute was discarded. | Preserve either field, both distinct texts, ordering, and long commentary. |
| High | Source checksums were ignored. | Validate declared block hashes before decoding; explicitly report missing image checksums. |
| High | Truncated uncompressed attachments became zero-filled pixels and still passed verification. | Validate block bounds and require exact reads. |
| High | libXISF 0.2.13 ignored zlib errors and did not require exact LZ4/ZSTD decoded lengths. | Use direct codec calls with checked return statuses and exact output sizes for the supported image subset. |
| High | XML properties, precision, creation time, and aperture were lost. | Preserve representable scalars under HIERARCH, retaining source numeric tokens/timestamps and reporting omissions/conflicts. |
| High | Copied structural keywords could alter image interpretation. `BLANK=0` made source value 32768 appear undefined while verification passed. | Filter structural/null/scaling/table/compression keywords; verify unsigned scaling and pixel null masks. |
| High | CFITSIO interpreted brackets/parentheses in paths and left stray files. | Literal disk-file APIs, absolute paths, and a fixed filename inside a private temporary directory. |
| Medium | No file/directory synchronization before/after publication. | File fsync before atomic publication; directory synchronization afterward, including newly created paths. |
| Medium | Publication required hard links. | Prefer Linux atomic no-replace rename; retain hard links only as a safe fallback. |
| Medium | Empty/long values and extended keyword names were rejected. | Undefined values, CONTINUE, and HIERARCH support, with explicit handling of unrepresentable metadata. |
| Medium | CFA was ignored and row reversal could change Bayer interpretation. | Preserve supported 2×2 CFA, account for offsets/height parity, reject conflicts, and retain the WCS flip guard. |
| Medium | Verification checked only the image checksum, ignored metadata, and disabled null detection. | Validate both HDUs, full header syntax, duplicate singleton keys, canonical metadata, every pixel, and null masks. |
| Medium | Existing batch outputs were trusted solely because they existed. | `--verify` validates existing outputs against their sources before skipping; failures request explicit regeneration. |
| Low | One unreadable directory aborted scanning; output collisions were not detected. | Continue with reported directory errors; precompute exact destination collisions. |
| Low | Signal cleanup, diagnostics, and error classification were unreliable. | Orderly SIGINT/SIGTERM cleanup, typed unsupported/cancelled outcomes, CFITSIO error-stack details, and consistent result/diagnostic streams. |
| Low | Missing `--`/`--version`, sticky repeated row-order option, misleading ROWORDER comment, and fortification overrides. | Fix CLI semantics, expose CMake version, describe output orientation, and honor externally supplied fortification. |

Two qualifications to the initial report matter: `!bang.fits.fz` and embedded
`.gz` in a basename did not reproduce unsafe behavior because the old temporary
filename altered CFITSIO's input. Brackets and parentheses did reproduce the
problem. Also, SMB hard-link support depends on the server/share; it is not
universally absent. `EXTNAME` is legitimate descriptive image metadata and is
retained, rather than treating every reported reserved keyword as corrupting.

Source properties are not always more reliable than capture keywords. An
examined frame has `Instrument:Focuser:Position=0` while `FOCUSPOS=2946`.
Both values now survive; the conflict is reported without overwriting the
capture keyword. Slightly different coordinate precision likewise survives in
the HIERARCH property instead of replacing the original capture record.

## Validation

- C++ tests cover full-token typing, structural/WCS classification, serialization,
  no-overwrite publication, explicit replacement, cancellation cleanup, pixel
  mismatch detection, and null interpretation despite valid FITS checksums.
- Python integration tests use independently generated XISFs and Astropy, with
  parameter matrices for every supported codec, byte shuffling, compressed
  subblocks, and attachment/inline/embedded storage. They cover legacy/current
  metadata forms, corruption, precision, CFA parity, long strings, filename
  syntax, CLI/batch behavior, primary/image checksums, and invalid headers.
- Linux test-only fault interposition exercises an actual publication race,
  missing hard-link support, file/directory fsync failures, SIGTERM cleanup,
  and file-before-directory synchronization ordering.
- Normal and address/undefined-behavior sanitizer builds are exercised with the
  same tests. LeakSanitizer requires execution without a tracing sandbox.
- Optional archive validation selected one frame from each observed version
  (3.0.0.1033, 1036, 1075, 2002, 2003, 3.1.2.9001, and 3.2.0.9001), plus a
  PixInsight-modified frame. All eight passed source hashing, independent
  decompression, strict FITS validation, both HDU checksums, **208,866,816 pixel
  comparisons**, **419 capture-keyword comparisons**, and **192 property
  comparisons**. Only temporary outputs were written.

The header inventory covered the archive; the full pixel/hash validation was
representative, not an exhaustive validation of every archived image.

## Remaining format and environment limits

The chosen output remains FITS headers only. Unicode normalization, truncated
comments, unsupported properties, and ancillary-image/display omissions are
reported. This does not provide byte-for-byte preservation of the original XML.
Native PixInsight astrometric solutions remain unsupported and are rejected.
Unknown unquoted custom keywords cannot always reveal their original type.

Power-loss guarantees depend on the filesystem and storage honoring fsync and
atomic rename. SIGKILL can leave private temporary directories. Existing outputs
without `--verify` are explicitly skipped without validation. Tests simulate
unavailable hard links; they are not a hardware/filesystem certification for all
exFAT or SMB implementations.

License selection and initializing/restoring repository history remain
repository-owner tasks; this change does neither.

## References

- [FITS standard](https://fits.gsfc.nasa.gov/fits_standard.html)
- [CFITSIO literal disk-file APIs](https://heasarc.gsfc.nasa.gov/docs/software/fitsio/c/c_user/node35.html)
- [CFITSIO image/null-mask reads](https://heasarc.gsfc.nasa.gov/docs/software/fitsio/c/c_user/node40.html)
- [CONTINUE and HIERARCH conventions](https://heasarc.gsfc.nasa.gov/docs/fcg/common_dict.html)
- [libXISF 0.2.13 source](https://gitea.nouspiro.space/nou/libXISF/src/tag/v0.2.13/libxisf.cpp)
