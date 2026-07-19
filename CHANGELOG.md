# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Per [CONTRIBUTING.md](CONTRIBUTING.md), any change to the model constants in
`squish.c` is a compressed-format break and requires a new magic number.

## [Unreleased]

### Changed

- **One on-disk format.** Everything SQUISH writes is now a single seekable
  **SQUISH archive** (magic `SQSH`, version 1; see docs/FORMAT.md). A file or a
  memory buffer is a one-member archive; a directory is a many-member archive.
  This replaces the four pre-release formats — the `SQ02` single stream, the
  `SQ01` multi-block stream, and the `SQAR01`/`SQAR02` directory archives — with
  one container. Its atom is a coded **block** (`mode + payload + checksum`, no
  magic or length — the archive index frames it); a member's data is a list of
  blocks (chunked for parallelism), so big members still (de)compress on all
  cores and a reader still seeks straight to any member or block. None of these
  formats had shipped, so nothing on disk needs migrating.
- **Streamlined library API.** Parallelism is folded into the core functions via
  `nthreads`/`chunk_size` parameters rather than separate `_mt` entry points.
  `squish_compress_alloc` / `squish_decompress_alloc` now take
  `(nthreads, chunk_size, progress, user)` and produce/consume a one-member
  archive; `squish_compress_file` / `squish_decompress_file` likewise. New
  `squish_content_size` (reads the archive's uncompressed size from the header)
  replaces `squish_decompressed_size`. The `squish_archive_*` API is unchanged
  except `squish_archive_info` now also reports `chunk_size` and a `SINGLE`
  flag. Removed: `squish_compress`/`_decompress` (caller-buffer), every `_mt`
  variant, `squish_compress_file2`/`_decompress_file2`, and
  `squish_decompressed_size`.
- CLI: `c` packs a file or directory into an archive; `d` restores a single
  file or extracts a whole tree (chosen by the archive's `SINGLE` flag); `l`
  and `x` inspect and extract members. Live status line and `-q`/`-t`/`-b`
  options as before.

### Removed

- The self-extracting archive format and the `squish s` command. Distribute the
  `.sqsh` archive and the `squish` CLI separately.

## [1.0.0]

### Added

- Initial release: libsquish context-mixing compressor (`squish.c`,
  `squish.h`) with CLI front end (`squish_cli.c`)
- Ten statistical models (byte contexts order 0-6, word model, record
  model, long-range match model) fused by an online-trained logistic mixer
  and arithmetic coder
- Round-trip integrity checksum; `squish_compress_bound(n) = n + 17`
  stored-mode fallback for incompressible data
- Build via Makefile: shared/static library, CLI, Windows DLL
  cross-compile, install target
- Test suite (`tests/test_squish.c`)
- Benchmarks against zip, bzip2, rar, xz on Silesia + Canterbury + enwik8
  (`bench/`, see [bench/RESULTS.md](bench/RESULTS.md))
- API reference ([docs/API.md](docs/API.md)) and format specification
  ([docs/FORMAT.md](docs/FORMAT.md))
- Algorithm design document ([SQUISH.md](SQUISH.md))
- Project meta: `LICENSE` (GPLv3) with per-file copyright headers,
  `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md` (Contributor Covenant v2.1),
  `SECURITY.md`, `CHANGELOG.md`, `.github/` CODEOWNERS, PR and issue
  templates
- CI (`.github/workflows/ci.yml`): build + test on gcc/clang, plus a
  mingw-w64 cross-compile check of `make dll`
- `make dll` cross-compiles both `squish.dll` and `squish.exe`
  (statically linked against a mingw-built `libsquish-win.a`)
- `make windows-dll`: builds `squish.dll` + `squish.exe` with MSVC
  (`cl.exe`), for native Windows builds without mingw

[Unreleased]: https://github.com/paigejulianne/squish/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/paigejulianne/squish/releases/tag/v1.0.0
