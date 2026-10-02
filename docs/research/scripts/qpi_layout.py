#!/usr/bin/env python3
"""
qpi_layout.py -- reference model (Python, stdlib only) of
  * the x86-64 layout rules needed for Qubic contract state structs, and
  * the binary layout + read-only decode algorithms of all QPI containers
    (BitArray, Array, SlowAnySizeArray, HashMap, HashSet, Collection, LinkedList, ProposalVoting helpers).

Source of truth: qubic core src/qpi/qpi_containers.h, src/qpi/impl/qpi_*_impl.h (identical in v1.303.2 and v1.306.0).
This file is a research artifact documenting/validating the algorithms; the product re-implements them in C++.
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qubic_identity import identity_from_pubkey, asset_name_to_str  # noqa: E402

NULL_INDEX = -1
MASK64 = (1 << 64) - 1


def align_up(x, a):
    return (x + a - 1) // a * a


# ======================================================================================================================
# Type model + layout engine
# ======================================================================================================================

class Type:
    name = "?"
    size = 0
    align = 1

    def read(self, mv, off):
        raise NotImplementedError

    def __repr__(self):
        return f"<{self.name} size={self.size} align={self.align}>"


class Prim(Type):
    def __init__(self, name, fmt):
        self.name = name
        self.fmt = "<" + fmt
        self.size = struct.calcsize(self.fmt)
        self.align = self.size

    def read(self, mv, off):
        return struct.unpack_from(self.fmt, mv, off)[0]


uint8 = Prim("uint8", "B")
sint8 = Prim("sint8", "b")
uint16 = Prim("uint16", "H")
sint16 = Prim("sint16", "h")
uint32 = Prim("uint32", "I")
sint32 = Prim("sint32", "i")
uint64 = Prim("uint64", "Q")
sint64 = Prim("sint64", "q")
bool_ = Prim("bool", "B")      # 1 byte; any non-zero value is "true"
char_ = Prim("char", "b")


class IdType(Type):
    """m256i / QPI::id : union of 32 bytes, alignment 8 (NO __m256i member -> NOT 32-byte aligned)."""
    name = "id"
    size = 32
    align = 8

    def read(self, mv, off):
        return bytes(mv[off:off + 32])


id_ = IdType()


class Field:
    __slots__ = ("name", "type", "offset")

    def __init__(self, name, type_, offset):
        self.name, self.type, self.offset = name, type_, offset


class Struct(Type):
    """Plain struct: members laid out in declaration order, each at the next multiple of its alignment;
    alignof = max member alignment; sizeof = end offset rounded up to alignof; empty struct has size 1."""

    def __init__(self, name, members):
        self.name = name
        self.fields = []
        off = 0
        al = 1
        for mname, mtype in members:
            off = align_up(off, mtype.align)
            self.fields.append(Field(mname, mtype, off))
            off += mtype.size
            al = max(al, mtype.align)
        self.align = al
        self.size = align_up(off, al) if self.fields else 1
        self.by_name = {f.name: f for f in self.fields}

    def off(self, name):
        return self.by_name[name].offset

    def ftype(self, name):
        return self.by_name[name].type

    def read(self, mv, off):
        return {f.name: f.type.read(mv, off + f.offset) for f in self.fields}


class Union(Type):
    def __init__(self, name, members):
        self.name = name
        self.members = members
        self.align = max(t.align for _, t in members)
        self.size = align_up(max(t.size for _, t in members), self.align)

    def read(self, mv, off):
        return bytes(mv[off:off + self.size])


class CArray(Type):
    """C array T x[n]."""

    def __init__(self, elem, n):
        self.elem, self.n = elem, n
        self.name = f"{elem.name}[{n}]"
        self.size = elem.size * n
        self.align = elem.align

    def read(self, mv, off):
        if isinstance(self.elem, Prim):
            return list(struct.unpack_from(f"<{self.n}{self.elem.fmt[1]}", mv, off))
        return [self.elem.read(mv, off + i * self.elem.size) for i in range(self.n)]


# ---- QPI helper types ------------------------------------------------------------------------------------------------
bit = Struct("bit", [("charValue", char_)])                              # qpi_types.h:34
uint128 = Struct("uint128_t", [("low", uint64), ("high", uint64)])       # platform/uint128.h:26
DateAndTime = Struct("DateAndTime", [("value", uint64)])                 # qpi_date_time.h:15 / :610
Asset = Struct("Asset", [("issuer", id_), ("assetName", uint64)])        # qpi_types.h:77
Entity = Struct("Entity", [("publicKey", id_), ("incomingAmount", sint64), ("outgoingAmount", sint64),
                           ("numberOfIncomingTransfers", uint32), ("numberOfOutgoingTransfers", uint32),
                           ("latestIncomingTransferTick", uint32), ("latestOutgoingTransferTick", uint32)])
NoData = Struct("NoData", [])


# ---- QPI containers expressed as "struct of the declared members" ----------------------------------------------------

def is_pow2(L):
    return L > 0 and (L & (L - 1)) == 0


def BitArray(L):
    assert is_pow2(L)
    t = Struct(f"BitArray<{L}>", [("_values", CArray(uint64, (L + 63) // 64))])
    t.kind, t.L = "BitArray", L
    assert t.size == bitarray_size(L)
    return t


def Array(T, L):
    assert is_pow2(L)
    t = Struct(f"Array<{T.name},{L}>", [("_values", CArray(T, L))])
    t.kind, t.L, t.T = "Array", L, T
    return t


def SlowAnySizeArray(T, L):
    assert L > 0
    t = Struct(f"SlowAnySizeArray<{T.name},{L}>", [("_values", CArray(T, L))])
    t.kind, t.L, t.T = "SlowAnySizeArray", L, T
    return t


def HashMap(K, V, L):
    assert is_pow2(L)
    E = Struct("Element", [("key", K), ("value", V)])
    t = Struct(f"HashMap<{K.name},{V.name},{L}>", [
        ("_elements", CArray(E, L)),
        ("_occupationFlags", CArray(uint64, (L * 2 + 63) // 64)),
        ("_population", uint64),
        ("_markRemovalCounter", uint64)])
    t.kind, t.L, t.K, t.V, t.E = "HashMap", L, K, V, E
    assert t.size == hashmap_size(K.size, K.align, V.size, V.align, L), (t.size, hashmap_size(K.size, K.align, V.size, V.align, L))
    return t


def HashSet(K, L):
    assert is_pow2(L)
    t = Struct(f"HashSet<{K.name},{L}>", [
        ("_keys", CArray(K, L)),
        ("_occupationFlags", CArray(uint64, (L * 2 + 63) // 64)),
        ("_population", uint64),
        ("_markRemovalCounter", uint64)])
    t.kind, t.L, t.K = "HashSet", L, K
    assert t.size == hashset_size(K.size, L)
    return t


PoV = Struct("PoV", [("value", id_), ("population", uint64), ("headIndex", sint64), ("tailIndex", sint64),
                     ("bstRootIndex", sint64)])
assert PoV.size == 64


def Collection(T, L):
    assert is_pow2(L)
    E = Struct("Element", [("value", T), ("priority", sint64), ("povIndex", sint64), ("bstParentIndex", sint64),
                           ("bstLeftIndex", sint64), ("bstRightIndex", sint64)])
    t = Struct(f"Collection<{T.name},{L}>", [
        ("_povs", CArray(PoV, L)),
        ("_povOccupationFlags", CArray(uint64, (L * 2 + 63) // 64)),
        ("_elements", CArray(E, L)),
        ("_population", uint64),
        ("_markRemovalCounter", uint64)])
    t.kind, t.L, t.T, t.E = "Collection", L, T, E
    assert t.size == collection_size(T.size, L)
    return t


def LinkedList(T, L):
    assert is_pow2(L)
    N = Struct("Node", [("value", T), ("nextIndex", sint64), ("prevIndex", sint64)])
    t = Struct(f"LinkedList<{T.name},{L}>", [
        ("_nodes", CArray(N, L)),
        ("_occupiedFlags", CArray(uint64, (L + 63) // 64)),
        ("_headIndex", sint64),
        ("_tailIndex", sint64),
        ("_freeHeadIndex", sint64),
        ("_nextUnusedIndex", uint64),
        ("_population", uint64)])
    t.kind, t.L, t.T, t.N = "LinkedList", L, T, N
    assert t.size == linkedlist_size(T.size, L)
    return t


# ---- closed-form size formulas (valid for all types with alignof <= 8, i.e. every type usable in QPI) --------------------

def bitarray_size(L):
    return 8 * ((L + 63) // 64)


def hashmap_element_size(sk, ak, sv, av):
    a = max(ak, av)
    return align_up(align_up(sk, av) + sv, a)


def hashmap_size(sk, ak, sv, av, L):
    return align_up(L * hashmap_element_size(sk, ak, sv, av), 8) + 8 * ((2 * L + 63) // 64) + 16


def hashset_size(sk, L):
    return align_up(L * sk, 8) + 8 * ((2 * L + 63) // 64) + 16


def collection_element_size(st):
    return align_up(st, 8) + 40


def collection_size(st, L):
    return 64 * L + 8 * ((2 * L + 63) // 64) + L * collection_element_size(st) + 16


def linkedlist_node_size(st):
    return align_up(st, 8) + 16


def linkedlist_size(st, L):
    return L * linkedlist_node_size(st) + 8 * ((L + 63) // 64) + 40


# ---- proposal voting (qpi_proposals.h, impl/qpi_proposals_impl.h) ----------------------------------------------------
NUMBER_OF_COMPUTORS = 676


def ProposalDataV1():
    Transfer = Struct("Transfer", [("destination", id_), ("amounts", Array(sint64, 4))])
    TransferInEpoch = Struct("TransferInEpoch", [("destination", id_), ("amount", sint64), ("targetEpoch", uint16)])
    VariableOptions = Struct("VariableOptions", [("variable", uint64), ("values", Array(sint64, 4))])
    VariableScalar = Struct("VariableScalar", [("variable", uint64), ("minValue", sint64), ("maxValue", sint64),
                                               ("proposedValue", sint64)])
    Data = Union("Data", [("transfer", Transfer), ("transferInEpoch", TransferInEpoch),
                          ("variableOptions", VariableOptions), ("variableScalar", VariableScalar)])
    t = Struct("ProposalDataV1", [("url", Array(uint8, 256)), ("epoch", uint16), ("type", uint16), ("tick", uint32),
                                  ("data", Data)])
    assert t.size == 256 + 8 + 64      # static_assert qpi_proposals.h:363
    return t


def ProposalDataYesNo():
    Transfer = Struct("Transfer", [("destination", id_), ("amount", sint64)])
    VariableOptions = Struct("VariableOptions", [("variable", uint64), ("value", sint64)])
    Data = Union("Data", [("transfer", Transfer), ("variableOptions", VariableOptions)])
    t = Struct("ProposalDataYesNo", [("url", Array(uint8, 256)), ("epoch", uint16), ("type", uint16), ("tick", uint32),
                                     ("data", Data)])
    assert t.size == 256 + 8 + 40      # static_assert qpi_proposals.h:437
    return t


def ProposalWithAllVoteData(kind, num_votes=NUMBER_OF_COMPUTORS):
    """kind: 'V1scalar' (ProposalDataV1<true>), 'V1' (ProposalDataV1<false>), 'YesNo' (ProposalDataYesNo).
    The proposal data type is a BASE CLASS; its members come first (no tail padding in either base)."""
    if kind == "YesNo":
        base = ProposalDataYesNo()
        votes = CArray(uint8, (2 * num_votes + 7) // 8)     # 2 bits per vote
    else:
        base = ProposalDataV1()
        votes = CArray(sint64 if kind == "V1scalar" else uint8, num_votes)
    members = [(f.name, f.type) for f in base.fields] + [("votes", votes)]
    t = Struct(f"ProposalWithAllVoteData<{kind},{num_votes}>", members)
    assert t.off("votes") == base.size
    return t


def ProposalAndVotingByComputors(slots):
    return Struct(f"ProposalAndVotingByComputors<{slots}>", [("currentProposalProposers", CArray(id_, slots))])


ProposalByAnyoneVotingByComputors = ProposalAndVotingByComputors  # derived class adds no data members


def ProposalAndVotingByShareholders(slots):
    return Struct(f"ProposalAndVotingByShareholders<{slots}>", [
        ("currentProposalProposers", CArray(id_, slots)),
        ("currentProposalShareholders", CArray(CArray(id_, NUMBER_OF_COMPUTORS), slots))])


def ProposalVoting(handling, kind, max_proposals):
    return Struct("ProposalVoting", [("proposersAndVoters", handling),
                                     ("proposals", CArray(ProposalWithAllVoteData(kind), max_proposals))])


# ======================================================================================================================
# Decode algorithms (read-only viewer semantics)
# ======================================================================================================================

def u64(mv, off):
    return struct.unpack_from("<Q", mv, off)[0]


def s64(mv, off):
    return struct.unpack_from("<q", mv, off)[0]


# ---- 2-bit occupation flags (HashMap / HashSet / Collection PoV table) ----------------------------------------------

def flag2(mv, flags_off, i):
    """State of slot i: 0 = never used/empty, 1 = occupied, 2 = marked for removal (tombstone), 3 = unused/invalid.
    Slot i uses bits (2*(i&31)) and (2*(i&31)+1) of little-endian uint64 word number i>>5."""
    return (u64(mv, flags_off + 8 * (i >> 5)) >> ((i & 31) << 1)) & 3


def flag2_counts(mv, flags_off, L):
    """Return (#empty, #occupied, #marked, #invalid) over slots [0, L)."""
    nwords = (2 * L + 63) // 64
    x = int.from_bytes(mv[flags_off:flags_off + 8 * nwords], "little")
    if 2 * L < 64 * nwords:                       # L < 32: mask off the unused upper bits of the single word
        x &= (1 << (2 * L)) - 1
    m = int("01" * L, 2)                          # 0b0101...01 (L pairs)
    lo = x & m
    hi = (x >> 1) & m
    occ = (lo & ~hi).bit_count()
    marked = (hi & ~lo).bit_count()
    invalid = (lo & hi).bit_count()
    return L - occ - marked - invalid, occ, marked, invalid


def flag2_iter(mv, flags_off, L, want=1):
    """Yield slot indices whose 2-bit state == want, in ascending index order (skips all-zero words quickly)."""
    nwords = (2 * L + 63) // 64
    for w, (word,) in enumerate(struct.iter_unpack("<Q", mv[flags_off:flags_off + 8 * nwords])):
        if not word:
            continue
        base = w << 5
        j = 0
        while word:
            if (word & 3) == want and base + j < L:
                yield base + j
            word >>= 2
            j += 1


# ---- HashMap ---------------------------------------------------------------------------------------------------------

def hashmap_header(mv, base, t):
    return {"population": u64(mv, base + t.off("_population")),
            "markRemovalCounter": u64(mv, base + t.off("_markRemovalCounter"))}


def hashmap_iter(mv, base, t):
    """Yield (slotIndex, keyOffset, valueOffset) for all live entries in slot order."""
    E = t.E
    for i in flag2_iter(mv, base + t.off("_occupationFlags"), t.L, 1):
        eo = base + i * E.size
        yield i, eo + E.off("key"), eo + E.off("value")


def hashset_iter(mv, base, t):
    for i in flag2_iter(mv, base + t.off("_occupationFlags"), t.L, 1):
        yield i, base + i * t.K.size


def hashmap_lookup(mv, base, t, key_bytes, hash_u64):
    """Read-only re-implementation of getElementIndex() (linear probing, stop at first empty slot)."""
    L = t.L
    E = t.E if t.kind == "HashMap" else None
    ksize = t.K.size
    fo = base + t.off("_occupationFlags")
    idx = hash_u64 & (L - 1)
    for _ in range(L):
        st = flag2(mv, fo, idx)
        if st == 0:
            return NULL_INDEX
        if st == 1:
            ko = base + idx * (E.size if E else ksize)
            if bytes(mv[ko:ko + ksize]) == key_bytes:
                return idx
        idx = (idx + 1) & (L - 1)
    return NULL_INDEX


# ---- Collection ------------------------------------------------------------------------------------------------------

class CollectionView:
    def __init__(self, mv, base, t):
        self.mv, self.base, self.t = mv, base, t
        self.L = t.L
        self.E = t.E
        self.povs_off = base + t.off("_povs")
        self.flags_off = base + t.off("_povOccupationFlags")
        self.elems_off = base + t.off("_elements")
        self.population = u64(mv, base + t.off("_population"))
        self.markRemovalCounter = u64(mv, base + t.off("_markRemovalCounter"))
        e = self.E
        self._o_prio, self._o_pov = e.off("priority"), e.off("povIndex")
        self._o_par, self._o_left, self._o_right = e.off("bstParentIndex"), e.off("bstLeftIndex"), e.off("bstRightIndex")

    # element accessors
    def eoff(self, i):
        return self.elems_off + i * self.E.size

    def priority(self, i):
        return s64(self.mv, self.eoff(i) + self._o_prio)

    def pov_index(self, i):
        return s64(self.mv, self.eoff(i) + self._o_pov)

    def parent(self, i):
        return s64(self.mv, self.eoff(i) + self._o_par)

    def left(self, i):
        return s64(self.mv, self.eoff(i) + self._o_left)

    def right(self, i):
        return s64(self.mv, self.eoff(i) + self._o_right)

    def value(self, i):
        return self.t.T.read(self.mv, self.eoff(i))

    # pov accessors
    def pov(self, p):
        return PoV.read(self.mv, self.povs_off + p * 64)

    def pov_state(self, p):
        return flag2(self.mv, self.flags_off, p)

    def iter_povs(self, want=1):
        """Yield (povIndex, PoV dict) of occupied PoVs in slot order."""
        for p in flag2_iter(self.mv, self.flags_off, self.L, want):
            yield p, self.pov(p)

    def pov_counts(self):
        return flag2_counts(self.mv, self.flags_off, self.L)

    def next_index(self, i):
        """_nextElementIndex(): in-order successor = next element in priority queue (lower-or-equal priority)."""
        if not (0 <= i < self.population):
            return NULL_INDEX
        r = self.right(i)
        if r != NULL_INDEX:
            i = r
            while True:
                l = self.left(i)
                if l == NULL_INDEX:
                    return i
                i = l
        p = self.parent(i)
        while p != NULL_INDEX:
            if self.left(p) == i:
                return p
            i, p = p, self.parent(p)
        return NULL_INDEX

    def prev_index(self, i):
        if not (0 <= i < self.population):
            return NULL_INDEX
        l = self.left(i)
        if l != NULL_INDEX:
            i = l
            while True:
                r = self.right(i)
                if r == NULL_INDEX:
                    return i
                i = r
        p = self.parent(i)
        while p != NULL_INDEX:
            if self.right(p) == i:
                return p
            i, p = p, self.parent(p)
        return NULL_INDEX

    def iter_pov_elements(self, pov, limit=None):
        """Yield element indices of one PoV from head (highest priority) to tail (lowest priority)."""
        i = pov["headIndex"]
        n = 0
        guard = pov["population"]
        while i != NULL_INDEX and n < guard and (limit is None or n < limit):
            yield i
            n += 1
            i = self.next_index(i)

    def iter_pov_elements_inorder(self, pov):
        """Alternative: explicit iterative in-order traversal of the BST (left, node, right) from bstRootIndex."""
        stack = []
        i = pov["bstRootIndex"]
        while stack or i != NULL_INDEX:
            while i != NULL_INDEX:
                stack.append(i)
                i = self.left(i)
            i = stack.pop()
            yield i
            i = self.right(i)


# ---- LinkedList ------------------------------------------------------------------------------------------------------

class LinkedListView:
    def __init__(self, mv, base, t):
        self.mv, self.base, self.t = mv, base, t
        self.L, self.N = t.L, t.N
        self.flags_off = base + t.off("_occupiedFlags")
        self.headIndex_raw = s64(mv, base + t.off("_headIndex"))
        self.tailIndex_raw = s64(mv, base + t.off("_tailIndex"))
        self.freeHeadIndex = s64(mv, base + t.off("_freeHeadIndex"))
        self.nextUnusedIndex = u64(mv, base + t.off("_nextUnusedIndex"))
        self.population = u64(mv, base + t.off("_population"))
        # zero-initialized, never-used list: _headIndex == _tailIndex == _freeHeadIndex == 0 (NOT -1)!
        self.never_used = self.population == 0 and self.nextUnusedIndex == 0
        self.headIndex = NULL_INDEX if self.population == 0 else self.headIndex_raw   # == headIndex() of the API
        self.tailIndex = NULL_INDEX if self.population == 0 else self.tailIndex_raw

    def occupied(self, i):
        if not (0 <= i < self.L):
            return False
        return (u64(self.mv, self.flags_off + 8 * (i >> 6)) >> (i & 63)) & 1 == 1

    def occupied_count(self):
        n = (self.L + 63) // 64
        x = int.from_bytes(self.mv[self.flags_off:self.flags_off + 8 * n], "little")
        return x.bit_count()

    def noff(self, i):
        return self.base + i * self.N.size

    def next(self, i):
        return s64(self.mv, self.noff(i) + self.N.off("nextIndex"))

    def prev(self, i):
        return s64(self.mv, self.noff(i) + self.N.off("prevIndex"))

    def value(self, i):
        return self.t.T.read(self.mv, self.noff(i))

    def __iter__(self):
        """Yield node indices from head to tail. MUST be guarded by population (zero-state caveat)."""
        i = self.headIndex
        n = 0
        while i != NULL_INDEX and n < self.population:
            if not self.occupied(i):
                raise ValueError(f"LinkedList corrupt: node {i} reached by traversal is not flagged occupied")
            yield i
            n += 1
            i = self.next(i)


# ---- BitArray --------------------------------------------------------------------------------------------------------

def bitarray_get(mv, base, i):
    """bit i = bit (i & 63) of little-endian uint64 word (i >> 6)  ==  bit (i & 7) of byte (i >> 3)."""
    return (mv[base + (i >> 3)] >> (i & 7)) & 1


def bitarray_bits(mv, base, L):
    return [bitarray_get(mv, base, i) for i in range(L)]


# ---- DateAndTime -----------------------------------------------------------------------------------------------------

def is_leap(y):
    return y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)


def date_and_time_decode(v):
    """uint64 -> dict of fields (qpi_date_time.h:56-61, 107-153, 600-609)."""
    d = {"year": (v >> 46) & 0xFFFF, "month": (v >> 42) & 0xF, "day": (v >> 37) & 0x1F, "hour": (v >> 32) & 0x1F,
         "minute": (v >> 26) & 0x3F, "second": (v >> 20) & 0x3F, "millisec": (v >> 10) & 0x3FF, "microsec": v & 0x3FF,
         "reserved": v >> 62}
    dim = [0, 31, 29 if is_leap(d["year"]) else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    d["valid"] = (v != 0 and 1 <= d["month"] <= 12 and 1 <= d["day"] <= dim[d["month"]] and d["hour"] < 24
                  and d["minute"] < 60 and d["second"] < 60 and d["millisec"] < 1000 and d["microsec"] < 1000)
    return d


def date_and_time_str(v):
    d = date_and_time_decode(v)
    s = "%04d-%02d-%02d %02d:%02d:%02d.%03d'%03d" % (d["year"], d["month"], d["day"], d["hour"], d["minute"],
                                                     d["second"], d["millisec"], d["microsec"])
    if v == 0:
        return s + " (unset)"
    return s if d["valid"] else s + " (INVALID)"


def date_and_time_encode(year, month, day, hour=0, minute=0, second=0, millisec=0, microsec=0):
    return (year << 46) | (month << 42) | (day << 37) | (hour << 32) | (minute << 26) | (second << 20) | (millisec << 10) | microsec


# ---- pretty printing -------------------------------------------------------------------------------------------------

def fmt_id(b):
    return identity_from_pubkey(b)


def pretty(t, v, hints=None):
    """Convert a decoded value to a JSON-friendly object, rendering ids as identities."""
    hints = hints or {}
    if isinstance(t, IdType):
        return fmt_id(v)
    if t is DateAndTime:
        return date_and_time_str(v["value"])
    if t is bit:
        return bool(v["charValue"])
    if t is uint128:
        return (v["high"] << 64) | v["low"]
    if isinstance(t, Struct):
        if getattr(t, "kind", None) in ("Array", "SlowAnySizeArray"):
            return [pretty(t.T, x, hints) for x in v["_values"]]
        out = {}
        for f in t.fields:
            x = v[f.name]
            if f.name in hints.get("asset_name_fields", ("assetName",)) and f.type is uint64:
                out[f.name] = f"{asset_name_to_str(x)!r} ({x})"
            else:
                out[f.name] = pretty(f.type, x, hints)
        return out
    if isinstance(t, CArray):
        return [pretty(t.elem, x, hints) for x in v]
    if isinstance(t, Union):
        return v.hex()
    return v


if __name__ == "__main__":
    # tiny self check of closed forms against the g++ probe output (see ../p01-work/probe_qpi.cpp)
    K40 = Struct("K40", [("a", id_), ("b", sint64)])
    K48 = Struct("K48", [("a", id_), ("b", uint64), ("c", sint64)])
    V1 = Struct("V1", [("x", uint8)])
    V12 = Struct("V12", [("a", uint32), ("b", uint32), ("c", uint32)])
    V3 = Struct("V3", [("a", uint8), ("b", uint8), ("c", uint8)])
    expected = [
        (HashMap(id_, uint64, 1024), 41232), (HashMap(uint64, uint8, 16), 280), (HashMap(uint8, uint64, 16), 280),
        (HashMap(id_, V1, 4), 184), (HashMap(uint32, V12, 64), 1056), (HashMap(uint16, V3, 2), 40),
        (HashMap(uint8, uint8, 1), 32),
        (HashSet(id_, 1024), 33040), (HashSet(uint32, 8), 56), (HashSet(uint8, 1), 32), (HashSet(V3, 64), 224),
        (Collection(K40, 1024), 147728), (Collection(K48, 64), 9760), (Collection(uint8, 16), 1816), (Collection(V3, 1), 136),
        (LinkedList(K40, 64), 3632), (LinkedList(uint8, 128), 3128), (LinkedList(V3, 1), 72),
        (BitArray(2), 8), (BitArray(64), 8), (BitArray(128), 16), (BitArray(4096), 512), (BitArray(1), 8),
        (Array(V3, 4), 12), (SlowAnySizeArray(V12, 5), 60), (Entity, 64), (Asset, 40), (uint128, 16), (DateAndTime, 8),
        (bit, 1), (NoData, 1),
        (ProposalWithAllVoteData("V1scalar"), 5736), (ProposalWithAllVoteData("V1"), 1008), (ProposalWithAllVoteData("YesNo"), 480),
        (ProposalVoting(ProposalAndVotingByComputors(200), "V1", 200), 208000),
        (ProposalVoting(ProposalAndVotingByComputors(100), "V1scalar", 100), 576800),
        (ProposalVoting(ProposalAndVotingByShareholders(8), "YesNo", 8), 177152),
    ]
    bad = 0
    for t, exp in expected:
        ok = t.size == exp
        bad += not ok
        print(("PASS " if ok else "FAIL ") + f"{t.name}: model={t.size} g++={exp}")
    sys.exit(1 if bad else 0)
