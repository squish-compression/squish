# Contributing to SQUISH

## Build and test

```sh
make            # libsquish.so + squish CLI
make test       # build and run the test suite
```

No dependencies beyond libc/libm. Run `make test` before sending a change —
it covers the API contract, round-trips, error paths, and robustness
(`tests/test_squish.c`).

For anything touching compression ratio or speed, also run the benchmark
suite (`bench/run_squish.py`, see [bench/RESULTS.md](bench/RESULTS.md)) and
include before/after numbers in the PR description.

## Format stability

The model constants in `squish.c` define the bitstream. **Any change to
them is a format break**: streams compressed with the old constants won't
decompress correctly with the new ones. If your change alters the model,
bump the format's magic number (see [docs/FORMAT.md](docs/FORMAT.md)) so old
and new streams can be told apart, and call this out explicitly in your PR.

## Code style

- Match the existing style in the file you're editing (brace placement,
  naming, comment style) rather than introducing a new one.
- Keep `-Wall -Wextra` clean (the Makefile builds with both).
- No new dependencies beyond libc/libm.
- Public API changes go in `squish.h` with doc comments, and should be
  reflected in [docs/API.md](docs/API.md).

## Copyright headers

New source files should carry the same GPLv3 header used in the existing
`.c`/`.h`/`.py` files (see any existing file for the exact text).

## Submitting changes

1. Fork the repo and create a branch off `main`.
2. Make your change, with tests for new behavior.
3. Ensure `make test` passes.
4. Open a pull request describing the change and, if relevant, its impact
   on compression ratio/speed.

By contributing, you agree your contributions are licensed under the
project's [GPLv3 license](LICENSE).
