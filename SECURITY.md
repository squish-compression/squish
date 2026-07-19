# Security Policy

## Supported Versions

SQUISH is pre-1.0 and does not yet maintain parallel release branches.
Security fixes are made against `main`; please use the latest commit.

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities.

Instead, report it privately by emailing **paige@paigejulianne.com** with:

- A description of the vulnerability and its potential impact
- Steps to reproduce (a minimal input file or code snippet is ideal)
- The commit hash or version you tested against

You should receive an acknowledgment within a few days. Once a fix is
available, a new release will be cut and the reporter credited (unless they
prefer to remain anonymous).

## Scope

Relevant vulnerability classes for this project include:

- Memory-safety issues in `squish.c`/`squish.h` (buffer overflows,
  out-of-bounds reads/writes, use-after-free) reachable via
  `squish_decompress_alloc`/`squish_compress_alloc` on attacker-controlled
  input
- Integer overflows in size/length handling that could lead to
  under-allocation
- Decompression bombs or other resource-exhaustion issues distinct from the
  documented `squish_compress_bound` guarantees
- Checksum/integrity bypass allowing corrupted data to decompress as valid

Since SQUISH decompresses untrusted input in many use cases, crashes or
memory-safety bugs triggered by malformed compressed streams are treated as
security issues, not just correctness bugs.

## Resource characteristics of decompression (by design)

Callers feeding untrusted streams to the `*_alloc`/file helpers should know:

- The output allocation is controlled by the archive header, up to the
  format limit of 4 GiB − 16 bytes per member. Check `squish_content_size()`
  first and refuse sizes your application cannot afford.
- Model state costs ~150 MB per active block — times the thread count when
  `nthreads` splits a member across cores.
- Context mixing has no compression-ratio ceiling, so a tiny valid stream
  legitimately expanding to gigabytes (e.g. compressed zeros) cannot be
  distinguished from a bomb by ratio alone; the size pre-check above is the
  supported guard. Truncated or forged streams that starve the decoder are
  detected and rejected early rather than decoded to the claimed length.
