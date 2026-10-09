#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Independent, bounded encrypted+casefold filename oracle (not filesystem images).

OpenSSL EVP supplies CBC-CTS/CS3 and SipHash-2-4; Python supplies HMAC-SHA512
HKDF, curated Unicode-12.1-stable expected folds and explicit wire encoding.
No core, C test-keyring, e2fsprogs or Linux implementation is imported.
"""
import argparse
import base64
import ctypes as C
import ctypes.util
import hashlib
import hmac
import json
from pathlib import Path
import struct

SOURCES = {
    "cts": "https://www.rfc-editor.org/rfc/rfc3962.html#appendix-B",
    "siphash_vectors": "https://github.com/veorq/SipHash/blob/master/vectors.h",
    "siphash_license": "https://github.com/veorq/SipHash/blob/master/LICENSE_CC0",
    "cts_provider": "https://docs.openssl.org/3.0/man3/EVP_EncryptInit/",
    "siphash_provider": "https://docs.openssl.org/3.0/man7/EVP_MAC-Siphash/",
    "casefold": "https://www.unicode.org/Public/12.1.0/ucd/CaseFolding.txt",
    "hash_wrapper": "https://github.com/torvalds/linux/blob/v6.12/fs/ext4/hash.c#L276-L303",
    "prepared_filter": "https://github.com/torvalds/linux/blob/v6.12/fs/ext4/namei.c#L1319-L1402",
    "fold_buffer": "https://github.com/torvalds/linux/blob/v6.12/fs/unicode/utf8-core.c#L93-L112",
}
MASTER = bytes((i * 7 + 3) & 255 for i in range(64))
NONCE = bytes(range(16))
META_FIELDS = ("name_len query_len cipher_len hashbytes_len queryhashbytes_len "
               "stored_major stored_minor query_major query_minor prepared_filter "
               "expected_relaxed_match expected_strict_match name_utf8_valid "
               "query_utf8_valid padding_flag").split()


class Param(C.Structure):
    """OpenSSL public OSSL_PARAM ABI from <openssl/core.h>."""
    _fields_ = [("key", C.c_char_p), ("data_type", C.c_uint), ("data", C.c_void_p),
                ("data_size", C.c_size_t), ("return_size", C.c_size_t)]


class Provider:
    def __init__(self, path=None):
        path = path or ctypes.util.find_library("crypto")
        if not path:
            raise RuntimeError("OpenSSL 3 libcrypto is required for requested generation")
        self.lib = C.CDLL(path)
        self.library = str(path)
        pointer, integer, size = C.c_void_p, C.c_int, C.c_size_t
        self.bind("OpenSSL_version", C.c_char_p, [integer])
        self.bind("EVP_CIPHER_fetch", pointer, [pointer, C.c_char_p, C.c_char_p])
        self.bind("EVP_CIPHER_free", None, [pointer])
        self.bind("EVP_CIPHER_CTX_new", pointer, [])
        self.bind("EVP_CIPHER_CTX_free", None, [pointer])
        self.bind("EVP_EncryptInit_ex2", integer,
                  [pointer, pointer, pointer, pointer, C.POINTER(Param)])
        self.bind("EVP_EncryptUpdate", integer,
                  [pointer, pointer, C.POINTER(integer), pointer, integer])
        self.bind("EVP_EncryptFinal_ex", integer, [pointer, pointer, C.POINTER(integer)])
        self.bind("EVP_Q_mac", pointer,
                  [pointer, C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(Param),
                   pointer, size, pointer, size, pointer, size, C.POINTER(size)])
        self.version = self.lib.OpenSSL_version(0).decode("ascii")

    def bind(self, name, restype, argtypes):
        function = getattr(self.lib, name)
        function.restype, function.argtypes = restype, argtypes

    @staticmethod
    def require(result, operation):
        if not result:
            raise RuntimeError(f"OpenSSL provider operation failed: {operation}")

    def cts(self, key, plain):
        if len(key) not in (16, 32) or not 16 <= len(plain) <= 255:
            raise ValueError("Bounded CTS key/input length")
        cipher = self.lib.EVP_CIPHER_fetch(None, f"AES-{len(key) * 8}-CBC-CTS".encode(), None)
        self.require(cipher, "fetch CBC-CTS")
        context = self.lib.EVP_CIPHER_CTX_new()
        try:
            self.require(context, "allocate cipher context")
            mode = C.create_string_buffer(b"CS3")
            params = (Param * 2)(Param(b"cts_mode", 4, C.addressof(mode), 3, 0), Param())
            key_buffer, iv = C.create_string_buffer(key), C.create_string_buffer(bytes(16))
            source, output = C.create_string_buffer(plain), C.create_string_buffer(len(plain) + 16)
            used, final = C.c_int(), C.c_int()
            self.require(self.lib.EVP_EncryptInit_ex2(context, cipher, key_buffer, iv, params),
                         "initialize explicit CS3")
            self.require(self.lib.EVP_EncryptUpdate(context, output, C.byref(used), source,
                                                    len(plain)), "encrypt complete CTS input")
            if not 0 <= used.value <= len(plain):
                raise RuntimeError("Invalid CTS update length")
            self.require(self.lib.EVP_EncryptFinal_ex(context, C.byref(output, used.value),
                                                      C.byref(final)), "finalize CTS")
            if used.value + final.value != len(plain):
                raise RuntimeError("CTS changed the plaintext length")
            return output.raw[:len(plain)]
        finally:
            if context:
                self.lib.EVP_CIPHER_CTX_free(context)
            self.lib.EVP_CIPHER_free(cipher)

    def siphash(self, key, message):
        if len(key) != 16 or len(message) > 4095:
            raise ValueError("Bounded SipHash key/input length")
        size, compression, finalization = C.c_size_t(8), C.c_uint(2), C.c_uint(4)
        params = (Param * 4)(
            Param(b"size", 2, C.addressof(size), C.sizeof(size), 0),
            Param(b"c-rounds", 2, C.addressof(compression), C.sizeof(compression), 0),
            Param(b"d-rounds", 2, C.addressof(finalization), C.sizeof(finalization), 0), Param())
        output, count = C.create_string_buffer(8), C.c_size_t()
        self.require(self.lib.EVP_Q_mac(None, b"SIPHASH", None, None, params,
                                        C.create_string_buffer(key), len(key),
                                        C.create_string_buffer(message), len(message),
                                        output, 8, C.byref(count)), "SipHash-2-4/64")
        if count.value != 8:
            raise RuntimeError("SipHash returned the wrong width")
        return output.raw


def hkdf(context, length, nonce=b""):
    """RFC5869 extract/expand with fscrypt's SHA512 domain separation."""
    if not 0 < length <= 64:
        raise ValueError("Only one bounded HKDF output block is needed")
    extracted = hmac.new(bytes(64), MASTER, hashlib.sha512).digest()
    return hmac.new(extracted, b"fscrypt\0" + bytes((context,)) + nonce + b"\x01",
                    hashlib.sha512).digest()[:length]


def known_answers(provider):
    # RFC3962 Appendix B, zero IV, including aligned input that distinguishes CS3.
    key = b"chicken teriyaki"
    text = b"I would like the General Gau's Chicken, please, and wonton soup."
    for size, expected in ((17, "c6353568f2bf8cb4d8a580362da7ff7f97"),
                           (32, "39312523a78662d5be7fcbcc98ebf5a8"
                                "97687268d6ecccc0c07b25e25ecfe584")):
        if provider.cts(key, text[:size]).hex() != expected:
            raise RuntimeError(f"RFC3962 CS3 known answer differs at length {size}")
    # Authors' CC0 vectors_sip64: messages are bytes0..length-1, key bytes0..15.
    answers = {0: "310e0edd47db6f72", 1: "fd67dc93c539f874", 7: "37d1018bf50002ab",
               8: "6224939a79f5f593", 15: "e545be4961ca29a1", 16: "db9bc2577fcc2a3f"}
    for size, expected in answers.items():
        if provider.siphash(bytes(range(16)), bytes(range(size))).hex() != expected:
            raise RuntimeError(f"Published SipHash answer differs at length {size}")
    raw = []
    key = bytes((i * 5 + 2) & 255 for i in range(32))
    for size in (16, 17, 31, 32, 48, 148, 149, 150, 152, 254, 255):
        plain = bytes((i * 13 + 1) & 255 for i in range(size))
        cipher = provider.cts(key, plain)
        # Existing independent AES256 answer, generated before this oracle.
        if size == 255 and hashlib.sha256(cipher).hexdigest() != \
                "24229f47b319d65dd4163b302813954ffe08d09c9184119c2c0df07ca5a1f418":
            raise RuntimeError("Existing AES256-CS3 length255 answer differs")
        raw.append(dict(length=size, key=key.hex(), plaintext=plain.hex(), ciphertext=cipher.hex()))
    return dict(cts=raw, siphash=[dict(length=n, key=bytes(range(16)).hex(),
                                     message=bytes(range(n)).hex(), output=answer)
                               for n, answer in answers.items()])


def cases():
    """Curated expected folds; never use the host Python Unicode database."""
    rows = []

    def add(label, name, query, folded, query_folded=None, match=True, valid=True,
            hash_bytes=None, query_hash_bytes=None):
        encode = lambda value: value.encode("utf-8") if isinstance(value, str) else value
        name, query, folded = encode(name), encode(query), encode(folded)
        query_folded = folded if query_folded is None else encode(query_folded)
        hash_bytes = folded if hash_bytes is None else encode(hash_bytes)
        query_hash_bytes = query_folded if query_hash_bytes is None else encode(query_hash_bytes)
        rows.append((label, name, query, folded, query_folded, match, valid,
                     hash_bytes, query_hash_bytes))

    for length in (1, 15, 16, 17, 28, 31, 32, 144, 148, 149, 150, 252, 253, 254, 255):
        add(f"ascii-{length}", b"A" * length, b"a" * length, b"a" * length)
    add("sharp-s", "Straße", "STRASSE", b"strasse")
    add("canonical-accent", "É", "e\u0301", "e\u0301")
    add("sigma", "Σ", "ς", "σ")
    add("dotted-i", "İ", "i\u0307", "i\u0307")
    add("ligature", "ﬃ", "FFI", b"ffi")
    add("empty-fold", "\u00ad", "\u034f", b"")
    expanded = "ι\u0308\u0301"
    for suffix in (2, 3):
        add(f"fold-{252 + suffix}", "ΐ" * 42 + "A" * suffix,
            "ΐ" * 42 + "a" * suffix, expanded * 42 + "a" * suffix)
    add("expanded-fold", "ΐ" * 43 + "A", "ΐ" * 43 + "a", expanded * 43 + "a")
    add("different", "Alpha", "Beta", b"alpha", b"beta", match=False)
    add("invalid-exact", b"\xffName", b"\xffName", b"\xffName", valid=False)
    add("invalid-different", b"\xffName", b"\xffname", b"\xffName", b"\xffname",
        match=False, valid=False)
    # CGJ prevents reordering in the first fold, then disappears. Linux's hash
    # wrapper folds the prepared result again; comparison retains the first fold.
    first_fold = "a\u0301\u0323"
    second_fold = "a\u0323\u0301"
    add("combining-barrier", "A\u0301\u034f\u0323", "a\u0301\u034f\u0323",
        first_fold, hash_bytes=second_fold, query_hash_bytes=second_fold)
    add("combining-barrier-distinct", "A\u0301\u034f\u0323", second_fold,
        first_fold, second_fold, match=False,
        hash_bytes=second_fold, query_hash_bytes=second_fold)
    return rows


def hashes(provider, key, text):
    combined = int.from_bytes(provider.siphash(key, text), "little")
    major, minor = (combined >> 32) & 0xfffffffe, combined & 0xffffffff
    return (major - 2 if major == 0xfffffffe else major), minor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--libcrypto", help="Explicit existing OpenSSL3 library path")
    args = parser.parse_args()
    provider = Provider(args.libcrypto)
    vectors = known_answers(provider)  # Fail before producing requested fixtures if unavailable.
    directory_key, hash_key = hkdf(2, 32, NONCE), hkdf(5, 16, NONCE)
    context = bytes((2, 1, 4, 0, 0, 0, 0, 0)) + hkdf(1, 16) + NONCE
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "context.bin").write_bytes(context)
    (output / "keys.bin").write_bytes(directory_key + hash_key)
    names = []
    for label, name, query, folded, query_folded, match, valid, hash_bytes, query_hash_bytes in cases():
        if not 1 <= len(name) <= 255 or not 1 <= len(query) <= 255 or \
                max(len(folded), len(query_folded), len(hash_bytes), len(query_hash_bytes)) > 4095:
            raise RuntimeError("Curated case exceeds Linux input/fold bounds")
        length = min(255, (max(16, len(name)) + 3) // 4 * 4)
        cipher = provider.cts(directory_key, name.ljust(length, b"\0"))
        major, minor = hashes(provider, hash_key, hash_bytes)
        query_major, query_minor = hashes(provider, hash_key, query_hash_bytes)
        # Stored disk words are LE; no-key envelope targets little-endian Linux.
        envelope = struct.pack("<II", major, minor) + cipher[:149]
        if len(cipher) > 149:
            envelope += hashlib.sha256(cipher[149:]).digest()
        nokey = base64.urlsafe_b64encode(envelope).rstrip(b"=")
        if len(nokey) > 252:
            raise RuntimeError("No-key output exceeds Linux bound")
        prepared = valid and 0 < len(query_folded) < 255
        values = (len(name), len(query), len(cipher), len(hash_bytes), len(query_hash_bytes),
                  major, minor, query_major, query_minor, int(prepared), int(match),
                  int(match and valid), int(valid), int(valid), 0)
        for extension, data in (("name", name), ("query", query), ("cipher", cipher),
                                ("hashbytes", hash_bytes), ("queryhashbytes", query_hash_bytes),
                                ("nokey", nokey)):
            (output / f"{label}.{extension}").write_bytes(data)
        (output / f"{label}.meta").write_text(" ".join(map(str, values)) + "\n")
        names.append(label)
    (output / "manifest").write_text("\n".join(names) + "\n")
    (output / "vectors.json").write_text(json.dumps(vectors, indent=2) + "\n")
    details = dict(schema=1, meta_fields=META_FIELDS, case_count=len(names),
                   master=MASTER.hex(), nonce=NONCE.hex(), context=context.hex(),
                   cts_key=directory_key.hex(), siphash_key=hash_key.hex(),
                   cts_info=(b"fscrypt\0\x02" + NONCE).hex(),
                   siphash_info=(b"fscrypt\0\x05" + NONCE).hex(),
                   provider=provider.version, library=provider.library, sources=SOURCES,
                   scope="Synthetic filename/hash vectors, not native filesystem images",
                   unicode="Curated Unicode12.1-stable NFDICF; malformed names are raw-hashed",
                   strict_invalid="No-match expectation is the core's strict-invalid lookup contract",
                   no_key_endian="Little-endian Linux x86_64/arm64 envelope")
    (output / "keys.json").write_text(json.dumps(details, indent=2) + "\n")
    total = sum(path.stat().st_size for path in output.iterdir())
    if total >= 1024 * 1024:
        raise RuntimeError("Fixture output exceeds the 1MiB bound")
    print(f"PASS {len(names)} encrypted-casefold oracle cases, {total} bytes; {provider.version}")


if __name__ == "__main__":
    main()
