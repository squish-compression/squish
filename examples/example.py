#!/usr/bin/env python3
# Copyright (C) 2026  Paige Julianne Sullivan
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Use libsquish.so from Python via ctypes — no wrapper module needed."""
import ctypes
import os

_lib = ctypes.CDLL(os.path.join(os.path.dirname(__file__), "..", "libsquish.so"))

_lib.squish_version.restype = ctypes.c_char_p
_lib.squish_strerror.restype = ctypes.c_char_p
_lib.squish_strerror.argtypes = [ctypes.c_int]
_PROGRESS = ctypes.CFUNCTYPE(None, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p)
_lib.squish_compress_alloc.argtypes = [
    ctypes.c_char_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_int, ctypes.c_size_t, _PROGRESS, ctypes.c_void_p]
_lib.squish_decompress_alloc.argtypes = [
    ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t),
    ctypes.c_int, _PROGRESS, ctypes.c_void_p]
_lib.squish_free.argtypes = [ctypes.c_void_p]

_NO_CB = ctypes.cast(None, _PROGRESS)


def _check(rc: int) -> None:
    if rc != 0:
        raise RuntimeError(_lib.squish_strerror(rc).decode())


def compress(data: bytes, threads: int = 1, chunk: int = 0) -> bytes:
    """Compress `data` into a one-member SQUISH archive."""
    out, out_len = ctypes.c_void_p(), ctypes.c_size_t()
    _check(_lib.squish_compress_alloc(data, len(data),
                                      ctypes.byref(out), ctypes.byref(out_len),
                                      threads, chunk, _NO_CB, None))
    try:
        return ctypes.string_at(out, out_len.value)
    finally:
        _lib.squish_free(out)


def decompress(data: bytes, threads: int = 0) -> bytes:
    """Restore the single member of a SQUISH archive."""
    out, out_len = ctypes.c_void_p(), ctypes.c_size_t()
    _check(_lib.squish_decompress_alloc(data, len(data),
                                        ctypes.byref(out), ctypes.byref(out_len),
                                        threads, _NO_CB, None))
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
