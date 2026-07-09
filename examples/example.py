#!/usr/bin/env python3
"""Use libsquish.so from Python via ctypes — no wrapper module needed."""
import ctypes
import os

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "..", "libsquish.so"))

_lib.squish_version.restype = ctypes.c_char_p
_lib.squish_strerror.restype = ctypes.c_char_p
_lib.squish_strerror.argtypes = [ctypes.c_int]
_lib.squish_compress_alloc.argtypes = [
    ctypes.c_char_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
_lib.squish_decompress_alloc.argtypes = [
    ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
_lib.squish_free.argtypes = [ctypes.c_void_p]


def _check(rc: int) -> None:
    if rc != 0:
        raise RuntimeError(_lib.squish_strerror(rc).decode())


def compress(data: bytes) -> bytes:
    out, out_len = ctypes.c_void_p(), ctypes.c_size_t()
    _check(_lib.squish_compress_alloc(data, len(data),
                                      ctypes.byref(out), ctypes.byref(out_len)))
    try:
        return ctypes.string_at(out, out_len.value)
    finally:
        _lib.squish_free(out)


def decompress(data: bytes) -> bytes:
    out, out_len = ctypes.c_void_p(), ctypes.c_size_t()
    _check(_lib.squish_decompress_alloc(data, len(data),
                                        ctypes.byref(out), ctypes.byref(out_len)))
    try:
        return ctypes.string_at(out, out_len.value)
    finally:
        _lib.squish_free(out)


if __name__ == "__main__":
    print("libsquish", _lib.squish_version().decode())
    original = ("Ten models vote on every bit; a mixer learns whom to trust. "
                * 200).encode()
    packed = compress(original)
    restored = decompress(packed)
    assert restored == original
    print(f"{len(original)} -> {len(packed)} bytes "
          f"({8 * len(packed) / len(original):.3f} bpb), round-trip verified")
