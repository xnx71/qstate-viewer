#!/usr/bin/env python3
"""
Pure-Python (stdlib only) KangarooTwelve (KT128, empty customization string) and Qubic identity codec.

Written from the public K12 specification (RFC 9861 / "KangarooTwelve: fast hashing based on Keccak-p"),
NOT copied from the Qubic core. The Qubic core's KangarooTwelve() (src/kangaroo_twelve.h:1394) is standard
KT128 with an empty customization string; getIdentity() is src/four_q.h:1777.

API
    k12(data: bytes, outlen: int) -> bytes
    identity_from_pubkey(pk32: bytes, lower=False) -> str      (60 letters, incl. 4 checksum letters)
    pubkey_from_identity(ident: str, verify_checksum=True) -> bytes
    contract_id(index: int) -> bytes                           (id(contractIndex, 0, 0, 0))
    asset_name_to_str(u64: int) -> str
    hash_u64_of_key(key_bytes: bytes, is_id: bool) -> int      (QPI::HashFunction<KeyT>::hash)

Run as a script to execute the self test against reference vectors produced by the core's own code
(see ../p01-work/ref_identity.cpp) and against identities that appear in the core source.
"""
import struct
import sys

MASK64 = (1 << 64) - 1

# ----------------------------------------------------------------------------------------------------------------------
# Keccak-p[1600, 12 rounds]
# ----------------------------------------------------------------------------------------------------------------------

def _rc_table():
    # Independent, explicit LFSR implementation (bit-by-bit) to avoid any transcription error.
    def rc_bit(t):
        if t % 255 == 0:
            return 1
        R = 1
        for _ in range(t % 255):
            R <<= 1
            if R & 0x100:
                R ^= 0x171
        return R & 1
    out = []
    for ir in range(24):
        rc = 0
        for j in range(7):
            if rc_bit(j + 7 * ir):
                rc |= 1 << ((1 << j) - 1)
        out.append(rc)
    return out


_RC = _rc_table()
assert _RC[0] == 0x0000000000000001 and _RC[23] == 0x8000000080008008 and _RC[12] == 0x000000008000808B

# rho offsets, indexed [x][y]
_RHO = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61], [28, 55, 25, 21, 56], [27, 20, 39, 8, 14]]


def _rol(v, n):
    n %= 64
    return ((v << n) | (v >> (64 - n))) & MASK64 if n else v


def keccak_p1600_12(lanes):
    """In-place Keccak-p[1600, n_r = 12] on a list of 25 64-bit lanes; lane index = x + 5*y."""
    A = lanes
    for rnd in range(12, 24):
        # theta
        C = [A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20] for x in range(5)]
        D = [C[(x - 1) % 5] ^ _rol(C[(x + 1) % 5], 1) for x in range(5)]
        for i in range(25):
            A[i] ^= D[i % 5]
        # rho + pi
        B = [0] * 25
        for x in range(5):
            for y in range(5):
                B[y + 5 * ((2 * x + 3 * y) % 5)] = _rol(A[x + 5 * y], _RHO[x][y])
        # chi
        for y in range(0, 25, 5):
            b0, b1, b2, b3, b4 = B[y:y + 5]
            A[y] = b0 ^ ((~b1) & b2)
            A[y + 1] = b1 ^ ((~b2) & b3)
            A[y + 2] = b2 ^ ((~b3) & b4)
            A[y + 3] = b3 ^ ((~b4) & b0)
            A[y + 4] = b4 ^ ((~b0) & b1)
        # iota
        A[0] ^= _RC[rnd]
        for i in range(25):
            A[i] &= MASK64
    return A


_RATE = 168  # bytes; capacity 256 bits


def turboshake128(msg: bytes, dsep: int, outlen: int) -> bytes:
    """TurboSHAKE128(M, D, L): sponge over Keccak-p[1600,12], rate 168 bytes, domain separation byte D."""
    state = [0] * 25
    padded = bytearray(msg)
    padded.append(dsep)
    padlen = (-len(padded)) % _RATE
    padded.extend(b"\x00" * padlen)
    padded[-1] ^= 0x80
    for off in range(0, len(padded), _RATE):
        block = struct.unpack_from("<21Q", padded, off)
        for i in range(21):
            state[i] ^= block[i]
        keccak_p1600_12(state)
    out = bytearray()
    while True:
        out.extend(struct.pack("<21Q", *state[:21]))
        if len(out) >= outlen:
            return bytes(out[:outlen])
        keccak_p1600_12(state)


def _length_encode(x: int) -> bytes:
    if x == 0:
        return b"\x00"
    n = (x.bit_length() + 7) // 8
    return x.to_bytes(n, "big") + bytes([n])


_CHUNK = 8192


def k12(data: bytes, outlen: int, custom: bytes = b"") -> bytes:
    """KangarooTwelve / KT128(M, C, L). The Qubic core always uses C = empty."""
    S = bytes(data) + custom + _length_encode(len(custom))
    if len(S) <= _CHUNK:
        return turboshake128(S, 0x07, outlen)
    final = bytearray(S[:_CHUNK])
    final.extend(b"\x03" + b"\x00" * 7)
    n = 0
    for off in range(_CHUNK, len(S), _CHUNK):
        final.extend(turboshake128(S[off:off + _CHUNK], 0x0B, 32))
        n += 1
    final.extend(_length_encode(n))
    final.extend(b"\xFF\xFF")
    return turboshake128(bytes(final), 0x06, outlen)


# ----------------------------------------------------------------------------------------------------------------------
# Qubic identity <-> public key   (core: src/four_q.h getIdentity():1777, getPublicKeyFromIdentity():1723)
# ----------------------------------------------------------------------------------------------------------------------

def identity_from_pubkey(pk: bytes, lower: bool = False) -> str:
    """32-byte public key / id -> 60-letter identity (56 payload letters + 4 checksum letters)."""
    if len(pk) != 32:
        raise ValueError("public key must be 32 bytes")
    base = ord('a') if lower else ord('A')
    chars = []
    for i in range(4):
        frag = int.from_bytes(pk[8 * i:8 * i + 8], "little")      # uint64 fragment i, little-endian
        for _ in range(14):                                        # 14 base-26 digits, least significant first
            chars.append(chr(base + frag % 26))
            frag //= 26
    checksum = int.from_bytes(k12(pk, 3), "little") & 0x3FFFF      # 3 bytes of K12 -> 18 bits
    for _ in range(4):
        chars.append(chr(base + checksum % 26))
        checksum //= 26
    return "".join(chars)


def pubkey_from_identity(ident: str, verify_checksum: bool = True) -> bytes:
    """60-letter (or 56-letter, without checksum) upper-case identity -> 32-byte public key."""
    if len(ident) not in (56, 60):
        raise ValueError("identity must have 56 or 60 letters")
    out = bytearray()
    for i in range(4):
        frag = 0
        for j in range(13, -1, -1):
            c = ord(ident[i * 14 + j]) - ord('A')
            if not 0 <= c < 26:
                raise ValueError("identity must consist of A-Z")
            frag = frag * 26 + c
        # NOTE: 26**14 > 2**64, the core lets this wrap modulo 2**64 (unsigned long long arithmetic).
        out += (frag & MASK64).to_bytes(8, "little")
    pk = bytes(out)
    if verify_checksum and len(ident) == 60 and identity_from_pubkey(pk) != ident:
        raise ValueError("identity checksum mismatch")
    return pk


def contract_id(index: int) -> bytes:
    """id(contractIndex, 0, 0, 0): u64._0 = contractIndex, the other three uint64 are zero."""
    return struct.pack("<4Q", index, 0, 0, 0)


NULL_ID = bytes(32)


def is_contract_id_like(pk: bytes, max_contracts: int = 1024) -> bool:
    """Heuristic used by a viewer: upper 24 bytes zero and u64._0 < MAX_NUMBER_OF_CONTRACTS."""
    return pk[8:] == bytes(24) and int.from_bytes(pk[:8], "little") < max_contracts


def asset_name_to_str(v: int) -> str:
    """uint64 asset name -> text: little-endian bytes, up to 7 chars, stop at first NUL (8th byte must be 0)."""
    raw = (v & MASK64).to_bytes(8, "little")
    s = raw[:7].split(b"\x00", 1)[0]
    try:
        return s.decode("ascii")
    except UnicodeDecodeError:
        return s.decode("latin-1")


def asset_name_from_str(s: str) -> int:
    b = s.encode("ascii")
    if len(b) > 7:
        raise ValueError("asset name has at most 7 characters")
    return int.from_bytes(b.ljust(8, b"\x00"), "little")


def hash_u64_of_key(key_bytes: bytes, is_id: bool) -> int:
    """QPI::HashFunction<KeyT>::hash (qpi_hash_map_impl.h:17-30).
    id/m256i keys: first 8 bytes as little-endian uint64 (key.u64._0).
    every other key type: first 8 bytes of K12 over the raw sizeof(KeyT) bytes of the key (including padding bytes)."""
    if is_id:
        return int.from_bytes(key_bytes[:8], "little")
    return int.from_bytes(k12(key_bytes, 8), "little")


# ----------------------------------------------------------------------------------------------------------------------
# Self test
# ----------------------------------------------------------------------------------------------------------------------

# Identities that appear verbatim in the core source (file:line refer to core v1.306.0)
SOURCE_IDENTITIES = [
    ("src/public_settings.h:84 ARBITRATOR", "AFZPUAIYVPNUYGJRQVLUKOPPVLHAZQTGLYAAUUNBXFTVTAMSBKQBLEIEPCVJ"),
    ("src/public_settings.h:85 DISPATCHER", "XPXYKFLGSWRHRGAUKWFWVXCDVEYAPCPCNUTMUDWFGDYQCWZNJMWFZEEGCFFO"),
    ("test/contract_qtreat.cpp:532 QTREAT admin (gtest EXPECT_EQ on operator<<(m256i))",
     "QTREATZZIVFYQAIBKCZPSHGLIRMALZKHEWAPFLFXJAMDAXMGTBKQVXHHDHUD"),
    ("test/contract_qx.cpp:182", "EEWCBEZNLEITWFWVEOFBLKHVXTAARMIGJNXICDIRIFDBUDGFXEYABULCFXAN"),
    ("src/contracts/qRWA.h:1237 QMINE issuer", "QMINEQQXYBEGBHNSUPOUYDIQKZPCBPQIIHUUZMCPLBPCCAIARVZBTYKGFCWM"),
    ("src/contracts/QPayhub.h:1255 QPAY issuer", "QPAYNOWSWZMGHFEAEVJXGZAVSHABAZDDBDIHTEBOPCOGHRGBCYCUZOHCVLXG"),
    ("src/qubic.cpp:2048 doge dispatcher", "XPILPIJYHRBTACMMIRSJLIZWCXDBHWVEOTZBQFBXWEUXDZGGDEKDQPIEQKQK"),
    ("src/contracts/QTREAT.h:1272 signer 1", "IPPERNTLFKHNKHJMYXIJTPJYXOACKDRYVRGCYRGQYDFCWPXQQDPMOXTDENCI"),
]

# Official KangarooTwelve test vectors (RFC 9861, KT128, C = empty, 32-byte output, M = ptn(n))
K12_OFFICIAL = {
    0: "1ac2d450fc3b4205d19da7bfca1b37513c0803577ac7167f06fe2ce1f0ef39e5",
    1: "2bda92450e8b147f8a7cb629e784a058efca7cf7d8218e02d345dfaa65244a1f",
    17: "6bf75fa2239198db4772e36478f8e19b0f371205f6a9a93a273f51df37122888",
    289: "0c315ebcdedbf61426de7dcf8fb725d1e74675d7f5327a5067f367b108ecb67c",
    4913: "cb552e2ec77d9910701d578b457ddf772c12e322e4ee7fe417f92c758f0d59d0",
    83521: "8701045e22205345ff4dda05555cbb5c3af1a771c2b89baef37db43d9998b9fe",
}


def _ptn(n):
    return bytes(i % 251 for i in range(n))


def self_test(ref_path=None, verbose=True):
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            ok = False
        if verbose or not cond:
            print(("PASS " if cond else "FAIL ") + msg)

    for n, hx in K12_OFFICIAL.items():
        check(k12(_ptn(n), 32).hex() == hx, f"K12 official vector ptn({n})")

    for where, ident in SOURCE_IDENTITIES:
        pk = pubkey_from_identity(ident, verify_checksum=False)
        check(identity_from_pubkey(pk) == ident, f"identity round trip incl. checksum: {ident}  [{where}]  pk={pk.hex()}")

    check(identity_from_pubkey(NULL_ID) == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAFXIB", "NULL_ID identity")
    check(identity_from_pubkey(contract_id(1)) == "BAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARMID", "QX contract id identity")
    check(asset_name_to_str(asset_name_from_str("QUTIL")) == "QUTIL", "asset name round trip")

    if ref_path:
        n_id = n_k12 = 0
        mode = None
        for line in open(ref_path):
            line = line.strip()
            if not line:
                continue
            if line.startswith("# pubkey_hex"):
                mode = "id"; continue
            if line.startswith("# k12 len"):
                mode = "k12"; continue
            parts = line.split()
            if mode == "id":
                pk = bytes.fromhex(parts[0])
                good = identity_from_pubkey(pk) == parts[1] and k12(pk, 3).hex() == parts[2]
                good = good and pubkey_from_identity(parts[1]) == pk
                n_id += 1
                check(good, f"core getIdentity() vector {parts[0]} -> {parts[1]}") if not good else None
            elif mode == "k12":
                n = int(parts[0])
                good = k12(_ptn(n), 32).hex() == parts[1]
                n_k12 += 1
                check(good, f"core KangarooTwelve() vector len={n}") if not good else None
        check(n_id > 0 and n_k12 > 0, f"compared against core-generated reference file: {n_id} identities, {n_k12} K12 digests")
    return ok


if __name__ == "__main__":
    ref = sys.argv[1] if len(sys.argv) > 1 else None
    sys.exit(0 if self_test(ref) else 1)
