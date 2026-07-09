# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Per [CONTRIBUTING.md](CONTRIBUTING.md), any change to the model constants in
`squish.c` is a compressed-format break and requires a new magic number.

## [Unreleased]

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
