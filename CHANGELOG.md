# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Per [CONTRIBUTING.md](CONTRIBUTING.md), any change to the model constants in
`squish.c` is a compressed-format break and requires a new magic number.

## [Unreleased]

### Added

- Seekable directory archives (`SQAR02`) and a library archive API. A
  directory is now packed so that each file is compressed as its **own**
  stream, behind a header and a compressed index of paths and offsets — so a
  reader can view the header, list members, and inflate a single file or
  subtree by seeking straight to it, without decompressing the rest of the
  archive. New in `squish.h`: `squish_archive_create`, `squish_archive_open` /
  `_open_memory` / `_close`, `_probe`, `_info_get`, `_count`, `_stat`, `_find`,
  `_extract` / `_extract_path` / `_extract_to_file`, and `_extract_subtree`,
  plus the `squish_archive`, `squish_archive_info`, and `squish_archive_entry`
  types. All archive logic now lives in libsquish (previously CLI-only), so
  DLL/SO consumers get it too. Directories, empty directories, and unix
  permission bits are preserved; members are stored sorted (reproducible), and
  extraction rejects absolute paths and `..` traversal. Single-file
  compression is unchanged and byte-for-byte compatible with earlier releases.
  See docs/FORMAT.md §12 and docs/API.md.
- CLI: directory operations for `c`, `d`, `l`, `x`, and `s`. `squish c <dir>`
  builds a seekable `SQAR` archive; `squish l <archive>` lists it; `squish x
  <archive> <member> [dest]` extracts one file or subtree without touching the
  rest; `squish d` extracts the whole tree (auto-detecting an archive vs. a
  plain stream); `squish s` packs a directory into a self-extracting executable
  (the stub sniffs the payload to tell an archive from a single-file stream).
- CLI: `s` command — build a self-extracting archive. `squish s input output`
  compresses `input` and appends it (plus the original name and a 32-byte
  trailer) to a copy of the `squish` CLI used as a stub; the resulting
  executable restores the file when run, with no `squish` or `libsquish`
  installed, and checksum-verifies it. Running an archive takes `-f`
  (overwrite), `-q`, `-t`, and an optional output path; build takes the same
  `-t`/`-b`/`-q` as `c`. Extraction sanitizes the stored name to a basename,
  so an archive never writes outside the target directory. The archive is
  platform-specific (the stub is the host's CLI); see docs/FORMAT.md §11.
- Multi-threaded compression and decompression: new `SQ01` multi-block
  container (independent `SQ02` chunk streams; see docs/FORMAT.md §1b) and
  library functions `squish_compress_mt` / `squish_decompress_mt`, their
  `_alloc_mt` / `_file_mt` variants, and `squish_threads()`. Output depends
  only on the chunk size (default 16 MiB), never on the thread count, and
  the `squish_compress_bound` guarantee is preserved. Existing
  decompression entry points read `SQ01` streams transparently. (The
  `SQ01` magic previously named an unreleased pre-1.0 draft no reader
  accepted; it has been reassigned to the multi-block container.)
- CLI: `-t N` / `--threads N` — 0 = all cores; compression defaults to 1,
  keeping the ratio-optimal single-block format, decompression to all
  cores — and `-b N` / `--block N` block size in MiB for `-t`
- CLI: live status line on stderr (percent, bytes, throughput) while
  compressing/decompressing when stderr is a terminal; `-q`/`--quiet`
  suppresses the status line and the final summary (errors still print)
- Library: `squish_progress_fn` callback type and progress-reporting file
  helpers `squish_compress_file2` / `squish_decompress_file2` (additive;
  existing functions unchanged)
- `build-windows.bat`: builds `squish.dll` + `squish.exe` with MSVC from a
  plain command prompt (locates Visual Studio via vswhere; no make needed)

### Fixed

- Undefined behavior: `memcpy` was called with a NULL pointer and length 0
  when compressing empty input (`src == NULL, src_len == 0`, stored mode)
  or decompressing an empty stored stream to a NULL destination; caught by
  UBSan, now guarded
- `squish_decompressed_size` now rejects headers claiming sizes at or above
  `SQUISH_MAX_INPUT` (impossible for a valid stream) with
  `SQUISH_E_FORMAT` instead of returning them, hardening callers that size
  allocations from untrusted headers

### Changed

- CLI: the status line and summary now measure throughput on a wall clock
  (was CPU time, which over-counts when threads are in play)
- Build: the MSVC `squish.exe` (`make windows-dll`, `build-windows.bat`) now
  links libsquish statically instead of against `squish.dll`, matching the
  mingw `make dll` build. This makes `squish.exe` stand alone, which the new
  `s` command requires (an archive copies the CLI as its stub). `squish.dll`
  is still built for library consumers.
- Building now requires a threads library: `-pthread` outside Windows
  (added to the Makefile and pkg-config file), Win32 threads on Windows

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
