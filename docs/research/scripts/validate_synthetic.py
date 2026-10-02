#!/usr/bin/env python3
"""
validate_synthetic.py -- decode synthetic/synthetic_state.bin (raw memory image produced by the REAL QPI containers,
see synthetic/gen_synthetic.cpp) with the documented read-only algorithms and compare with the logical content the
containers reported through their public API (synthetic/synthetic_expected.json).

This covers what the real epoch-229 files cannot: LinkedList (not used by any contract yet), heavy tombstone / slot
reuse / cleanup() paths, L < 32 tables, K12-hashed struct keys with padding, BST rebuild, zero-state LinkedList.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from qubic_identity import hash_u64_of_key, identity_from_pubkey, asset_name_to_str, contract_id  # noqa: E402
from qpi_layout import (Struct, Array, BitArray, HashMap, HashSet, Collection, LinkedList, id_, bit, uint128, DateAndTime,  # noqa
                        Asset, uint8, uint16, uint32, uint64, sint64, flag2, flag2_counts, hashmap_iter, hashset_iter,
                        hashmap_lookup, CollectionView, LinkedListView, bitarray_get, date_and_time_decode, u64, s64,
                        date_and_time_str, date_and_time_encode, NULL_INDEX)

K40 = Struct("K40", [("a", id_), ("b", sint64)])
V12 = Struct("V12", [("a", uint32), ("b", uint32), ("c", uint32)])
KS = Struct("KS", [("x", uint16), ("y", uint8)])

Synth = Struct("Synth", [
    ("marker0", uint8),
    ("hmId", HashMap(id_, uint64, 64)),
    ("hmU32", HashMap(uint32, V12, 16)),
    ("hmKS", HashMap(KS, uint8, 32)),
    ("hsId", HashSet(id_, 128)),
    ("hsU64", HashSet(uint64, 32)),
    ("hmSmall", HashMap(id_, uint8, 8)),
    ("coll", Collection(K40, 64)),
    ("collSmall", Collection(uint64, 4)),
    ("ll", LinkedList(K40, 16)),
    ("llNeverUsed", LinkedList(uint64, 8)),
    ("llEmptied", LinkedList(uint64, 8)),
    ("llReset", LinkedList(uint64, 8)),
    ("llFull", LinkedList(uint64, 128)),
    ("bits", BitArray(128)),
    ("bits8", BitArray(8)),
    ("arr", Array(uint16, 8)),
    ("dt", DateAndTime),
    ("dtZero", DateAndTime),
    ("asset", Asset),
    ("flagTrue", bit),
    ("flagFalse", bit),
    ("big", uint128),
    ("marker1", uint8),
])

results = []


def check(cond, msg):
    results.append(bool(cond))
    print(("[PASS] " if cond else "[FAIL] ") + msg)


def main():
    mv = memoryview(open(os.path.join(HERE, "synthetic", "synthetic_state.bin"), "rb").read())
    exp = json.load(open(os.path.join(HERE, "synthetic", "synthetic_expected.json")))

    check(Synth.size == exp["sizeof"] == len(mv), f"sizeof(Synth): model {Synth.size} == g++ {exp['sizeof']} == file {len(mv)}")
    bad = [f.name for f in Synth.fields if exp["offsets"][f.name] != f.offset]
    check(not bad, f"all {len(Synth.fields)} member offsets match g++ offsetof() {bad if bad else ''}")
    check(mv[0] == 0xA5 and mv[Synth.off("marker1")] == 0x5A, "marker bytes at first/last member offsets")

    # ---------------- hash maps -------------------------------------------------------------------------------------------
    def hm_check(name, key_fmt, val_fmt, is_id):
        t, base = Synth.ftype(name), Synth.off(name)
        got = []
        for i, ko, vo in hashmap_iter(mv, base, t):
            got.append([i] + key_fmt(ko) + val_fmt(vo))
        want = [e[:len(got[0])] if got else e for e in exp[name]["entries"]]
        check(got == want, f"{name}: {len(got)} live entries (slot, key, value) identical to API iteration order")
        pop = u64(mv, base + t.off("_population"))
        e, o, m, inv = flag2_counts(mv, base + t.off("_occupationFlags"), t.L)
        check(pop == exp[name]["population"] == o, f"{name}: _population {pop} == API population == #0b01 flags; "
                                                   f"tombstones(0b10)={m}, _markRemovalCounter={u64(mv, base + t.off('_markRemovalCounter'))}")
        # lookup through the hash function
        ok = True
        homes = []
        for i, ko, vo in hashmap_iter(mv, base, t):
            kraw = bytes(mv[ko:ko + t.K.size])
            h = hash_u64_of_key(kraw, is_id)
            homes.append(h & (t.L - 1))
            ok &= hashmap_lookup(mv, base, t, kraw, h) == i
        check(ok, f"{name}: every live key is found again by hash + linear probing ({'u64._0' if is_id else 'K12'} hash)")
        return homes

    hm_check("hmId", lambda ko: [bytes(mv[ko:ko + 32]).hex()], lambda vo: [u64(mv, vo)], True)
    homes = hm_check("hmU32", lambda ko: [struct.unpack_from("<I", mv, ko)[0]],
                     lambda vo: [list(struct.unpack_from("<3I", mv, vo))], False)
    check(homes == [e[3] for e in exp["hmU32"]["entries"]], "hmU32: Python K12 home slots == core HashFunction<uint32>::hash & (L-1)")
    homes = hm_check("hmKS", lambda ko: list(struct.unpack_from("<HB", mv, ko)), lambda vo: [mv[vo]], False)
    check(homes == [e[4] for e in exp["hmKS"]["entries"]],
          "hmKS: Python K12 over all sizeof(KS)=4 bytes (incl. padding byte) == core HashFunction<KS>::hash & (L-1)")
    hm_check("hmSmall", lambda ko: [bytes(mv[ko:ko + 32]).hex()], lambda vo: [mv[vo]], True)

    # tombstone details for hmId
    t, base = Synth.ftype("hmId"), Synth.off("hmId")
    fo = base + t.off("_occupationFlags")
    states = [flag2(mv, fo, i) for i in range(t.L)]
    tomb = [i for i, s in enumerate(states) if s == 2]
    check(all(bytes(mv[base + i * t.E.size: base + (i + 1) * t.E.size]) == bytes(t.E.size) for i in tomb),
          f"hmId: tombstone slots {tomb} hold all-zero Element bytes (CLEAR_UNUSED_ELEMENT)")
    check(u64(mv, base + t.off("_markRemovalCounter")) == 3 and len(tomb) == 2,
          "hmId: 3 removals, 1 tombstone reused by a later set() -> 2 flags 0b10 but _markRemovalCounter == 3 (never decremented on reuse)")

    # ---------------- hash sets -------------------------------------------------------------------------------------------
    t, base = Synth.ftype("hsId"), Synth.off("hsId")
    got = [[i, bytes(mv[ko:ko + 32]).hex()] for i, ko in hashset_iter(mv, base, t)]
    check(got == exp["hsId"]["keys"], f"hsId: {len(got)} live keys identical to API iteration")
    e, o, m, inv = flag2_counts(mv, base + t.off("_occupationFlags"), t.L)
    check(u64(mv, base + t.off("_population")) == o == exp["hsId"]["population"] and m == 4
          and u64(mv, base + t.off("_markRemovalCounter")) == 4,
          "hsId: after cleanup() + 4 removals: _population == #0b01, #0b10 == _markRemovalCounter == 4")

    t, base = Synth.ftype("hsU64"), Synth.off("hsU64")
    got = []
    for i, ko in hashset_iter(mv, base, t):
        k = u64(mv, ko)
        got.append([i, k, hash_u64_of_key(bytes(mv[ko:ko + 8]), False) & (t.L - 1)])
    check(got == exp["hsU64"]["keys"], f"hsU64: {len(got)} live keys + K12 home slots identical to core")
    ok = all(hashmap_lookup(mv, base, t, bytes(mv[ko:ko + 8]), hash_u64_of_key(bytes(mv[ko:ko + 8]), False)) == i
             for i, ko in hashset_iter(mv, base, t))
    check(ok, "hsU64: lookups succeed in a nearly full table (wrap-around probing)")

    # ---------------- collections -----------------------------------------------------------------------------------------
    for name, vfmt in (("coll", lambda cv, i: bytes(mv[cv.eoff(i):cv.eoff(i) + 40]).hex()),
                       ("collSmall", lambda cv, i: bytes(mv[cv.eoff(i):cv.eoff(i) + 8]).hex())):
        t, base = Synth.ftype(name), Synth.off(name)
        cv = CollectionView(mv, base, t)
        check(cv.population == exp[name]["population"], f"{name}: _population {cv.population} == API population()")
        want = {p["pov"]: p for p in exp[name]["povs"]}
        got = {}
        for p, pov in cv.iter_povs():
            elems = [[i, cv.priority(i), vfmt(cv, i)] for i in cv.iter_pov_elements(pov)]
            inorder = list(cv.iter_pov_elements_inorder(pov))
            got[pov["value"].hex()] = {"pov": pov["value"].hex(), "population": pov["population"], "head": pov["headIndex"],
                                       "tail": pov["tailIndex"], "elements": elems}
            assert inorder == [e[0] for e in elems]
        check(got == want, f"{name}: {len(got)} PoVs; per-PoV population/head/tail and element order (index, priority, value) "
                           f"identical to API headIndex()/nextElementIndex() traversal")
        e, o, m, inv = cv.pov_counts()
        check(o == len(want), f"{name}: #0b01 PoV flags ({o}) == number of PoVs with population > 0; tombstoned PoVs={m}, "
                              f"_markRemovalCounter={cv.markRemovalCounter}")
        # dense prefix
        tail = bytes(mv[cv.eoff(cv.population): cv.elems_off + t.L * t.E.size])
        check(tail == bytes(len(tail)), f"{name}: _elements[_population..L) all zero (dense prefix of {cv.population})")
        # backward traversal
        okb = True
        for p, pov in cv.iter_povs():
            fwd = list(cv.iter_pov_elements(pov))
            back = []
            i = pov["tailIndex"]
            while i != NULL_INDEX:
                back.append(i)
                i = cv.prev_index(i)
            okb &= back == fwd[::-1]
        check(okb, f"{name}: tail->prevElementIndex traversal is the exact reverse of head->nextElementIndex")

    # ---------------- linked lists ----------------------------------------------------------------------------------------
    for name, vsize in (("ll", 40), ("llNeverUsed", 8), ("llEmptied", 8), ("llReset", 8), ("llFull", 8)):
        t, base = Synth.ftype(name), Synth.off(name)
        lv = LinkedListView(mv, base, t)
        got = [[i, bytes(mv[lv.noff(i):lv.noff(i) + vsize]).hex()] for i in lv]
        e = exp[name]
        occ = [i for i in range(t.L) if lv.occupied(i)]
        check(got == e["elements"] and lv.population == e["population"] and lv.headIndex == e["head"] and lv.tailIndex == e["tail"]
              and occ == e["occupied"] and lv.occupied_count() == lv.population,
              f"{name}: population={lv.population}, head/tail, element order and occupied flags identical to API; raw fields: "
              f"_headIndex={lv.headIndex_raw} _tailIndex={lv.tailIndex_raw} _freeHeadIndex={lv.freeHeadIndex} "
              f"_nextUnusedIndex={lv.nextUnusedIndex}")
        # backward
        back = []
        i = lv.tailIndex
        while i != NULL_INDEX and len(back) <= lv.population:
            back.append(i)
            i = lv.prev(i)
        check(back == [g[0] for g in got][::-1], f"{name}: tail->prevIndex traversal is the reverse of head->nextIndex")
    lv = LinkedListView(mv, Synth.off("llNeverUsed"), Synth.ftype("llNeverUsed"))
    check(lv.never_used and lv.headIndex_raw == 0 and lv.tailIndex_raw == 0 and lv.freeHeadIndex == 0,
          "llNeverUsed: zero state has RAW _headIndex == _tailIndex == _freeHeadIndex == 0 (not -1) -> must gate on _population")
    lv = LinkedListView(mv, Synth.off("llEmptied"), Synth.ftype("llEmptied"))
    check(lv.population == 0 and lv.nextUnusedIndex == 3 and lv.headIndex_raw == NULL_INDEX and lv.freeHeadIndex != NULL_INDEX,
          f"llEmptied: population 0, _nextUnusedIndex=3, raw head == -1, free list head = {lv.freeHeadIndex} (freed nodes chained via nextIndex)")
    # free-list walk
    seen = []
    i = lv.freeHeadIndex
    while i != NULL_INDEX and len(seen) < 16:
        seen.append(i)
        i = lv.next(i)
    check(sorted(seen) == [0, 1, 2], f"llEmptied: free list chain {seen} covers the 3 freed nodes; their prevIndex is -1 and value zeroed: "
                                     f"{[(lv.prev(j), u64(mv, lv.noff(j))) for j in seen]}")

    # ---------------- BitArray / Array / DateAndTime / Asset / bit / uint128 / ids ----------------------------------------
    base = Synth.off("bits")
    got = "".join(str(bitarray_get(mv, base, i)) for i in range(128))
    check(got == exp["bits"], "BitArray<128>: bit i == (byte[i>>3] >> (i&7)) & 1 matches BitArray::get(i) for all 128 bits")
    base = Synth.off("bits8")
    check("".join(str(bitarray_get(mv, base, i)) for i in range(8)) == exp["bits8"] and Synth.ftype("bits8").size == 8,
          "bit_8 (BitArray<8>): occupies a full uint64 (8 bytes); bits 0 and 6 set -> first byte 0x41")
    check(Synth.ftype("arr").read(mv, Synth.off("arr"))["_values"] == [i * 1111 for i in range(8)], "Array<uint16,8>: plain C array")
    v = u64(mv, Synth.off("dt"))
    d = date_and_time_decode(v)
    check([d[k] for k in ("year", "month", "day", "hour", "minute", "second", "millisec", "microsec")] == exp["dt"] and d["valid"]
          and v == date_and_time_encode(*exp["dt"]),
          f"DateAndTime: raw 0x{v:016x} -> {date_and_time_str(v)}")
    check(not date_and_time_decode(u64(mv, Synth.off("dtZero")))["valid"] and not exp["dtZeroValid"], "DateAndTime: value 0 is invalid/unset")
    a = Asset.read(mv, Synth.off("asset"))
    check(asset_name_to_str(a["assetName"]) == "QUTIL" and a["issuer"] == struct.pack("<4Q", 1, 2, 3, 4),
          f"Asset: issuer bytes = id(1,2,3,4) little-endian u64 x4; assetName {a['assetName']} -> 'QUTIL'")
    check(mv[Synth.off("flagTrue")] == 1 and mv[Synth.off("flagFalse")] == 0, "bit: 1 byte, true stored as 0x01")
    b = uint128.read(mv, Synth.off("big"))
    check(b["low"] == 0x99AABBCCDDEEFF00 and b["high"] == 0x1122334455667788, "uint128: low uint64 first, then high (uint128_t(high, low) ctor)")
    pk = bytes.fromhex(exp["idFromLetters"])
    check(identity_from_pubkey(pk)[:56] == "QTREATZZIVFYQAIBKCZPSHGLIRMALZKHEWAPFLFXJAMDAXMGTBKQVXHH",
          f"ID(_Q,_T,_R,...) letters == first 56 identity letters; full identity {identity_from_pubkey(pk)}")
    check(bytes.fromhex(exp["contractId5"]) == contract_id(5), "id(5,0,0,0) == 05 00.. (contract id: index in u64._0)")

    print(f"\nSUMMARY: {sum(results)} passed, {len(results) - sum(results)} failed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
