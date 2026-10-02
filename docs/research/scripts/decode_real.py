#!/usr/bin/env python3
"""
decode_real.py -- decode REAL epoch-229 contract state files with the algorithms documented in
01-qpi-types-and-containers.md and print evidence + consistency checks.

Read-only: files are mmap'ed with ACCESS_READ; nothing is loaded wholesale.
"""
import mmap
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qubic_identity import (identity_from_pubkey, asset_name_to_str, hash_u64_of_key, contract_id)  # noqa: E402
from qpi_layout import (flag2_counts, hashmap_header, hashmap_iter, hashset_iter, hashmap_lookup, CollectionView,  # noqa: E402
                        pretty, u64, NULL_INDEX, IdType, PoV)
import contract_states_229 as cs  # noqa: E402

CHECKS = []


def check(cond, msg):
    CHECKS.append((bool(cond), msg))
    print(("    [PASS] " if cond else "    [FAIL] ") + msg)


def open_state(index):
    path = cs.state_path(index)
    f = open(path, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    return path, memoryview(mm)


def is_zero(mv, a, b, chunk=1 << 24):
    while a < b:
        n = min(chunk, b - a)
        if mv[a:a + n].tobytes().count(0) != n:
            return False
        a += n
    return True


def idstr(b):
    return identity_from_pubkey(b)


# ----------------------------------------------------------------------------------------------------------------------

def show_hashmap(mv, state_t, field, n_show=5, key_is_id=None, verify_lookup=200, hints=None):
    f = state_t.by_name[field]
    t, base = f.type, f.offset
    hdr = hashmap_header(mv, base, t)
    empty, occ, marked, invalid = flag2_counts(mv, base + t.off("_occupationFlags"), t.L)
    print(f"  {state_t.name}.{field}: {t.name}  @offset {base}  sizeof={t.size}  sizeof(Element)={t.E.size} "
          f"(key@{t.E.off('key')}, value@{t.E.off('value')})")
    print(f"    _population={hdr['population']}  _markRemovalCounter={hdr['markRemovalCounter']}  "
          f"flags: empty(00)={empty} occupied(01)={occ} marked(10)={marked} invalid(11)={invalid}")
    check(hdr["population"] == occ, f"{field}: _population ({hdr['population']}) == number of 0b01 flags ({occ})")
    check(invalid == 0, f"{field}: no 0b11 flags")
    check(marked <= hdr["markRemovalCounter"], f"{field}: #0b10 flags ({marked}) <= _markRemovalCounter ({hdr['markRemovalCounter']})")
    if key_is_id is None:
        key_is_id = isinstance(t.K, IdType)
    n = 0
    lookups_ok = True
    displaced = 0
    for i, ko, vo in hashmap_iter(mv, base, t):
        kraw = bytes(mv[ko:ko + t.K.size])
        if n < n_show:
            k = pretty(t.K, t.K.read(mv, ko), hints)
            v = pretty(t.V, t.V.read(mv, vo), hints)
            vs = str(v)
            if len(vs) > 230:
                vs = vs[:230] + " ..."
            print(f"    slot {i}: key={k}  value={vs}")
        if n < verify_lookup:
            h = hash_u64_of_key(kraw, key_is_id)
            home = h & (t.L - 1)
            if home != i:
                displaced += 1
            if hashmap_lookup(mv, base, t, kraw, h) != i:
                lookups_ok = False
        n += 1
    m = min(n, verify_lookup)
    hname = "key.u64._0" if key_is_id else "K12(key bytes)[0..8)"
    check(lookups_ok, f"{field}: re-lookup of first {m} live keys via hash={hname} & (L-1) + linear probing finds the same slot "
                      f"({displaced} of them displaced from home slot by collisions)")
    # removed (0b10) slots must be all-zero (CLEAR_UNUSED_ELEMENT) -- check up to 1000 of them
    if marked:
        from qpi_layout import flag2_iter
        zero_ok, cnt = True, 0
        for i in flag2_iter(mv, base + t.off("_occupationFlags"), t.L, 2):
            eo = base + i * t.E.size
            zero_ok &= is_zero(mv, eo, eo + t.E.size)
            cnt += 1
            if cnt >= 1000:
                break
        check(zero_ok, f"{field}: first {cnt} slots flagged 0b10 (marked for removal) contain all-zero Element bytes")
    return hdr, (empty, occ, marked, invalid)


def show_hashset(mv, state_t, field, n_show=5, render=None, verify_lookup=200):
    f = state_t.by_name[field]
    t, base = f.type, f.offset
    pop = u64(mv, base + t.off("_population"))
    mrc = u64(mv, base + t.off("_markRemovalCounter"))
    empty, occ, marked, invalid = flag2_counts(mv, base + t.off("_occupationFlags"), t.L)
    print(f"  {state_t.name}.{field}: {t.name}  @offset {base}  sizeof={t.size}")
    print(f"    _population={pop}  _markRemovalCounter={mrc}  flags: empty(00)={empty} occupied(01)={occ} "
          f"marked(10)={marked} invalid(11)={invalid}")
    check(pop == occ, f"{field}: _population ({pop}) == number of 0b01 flags ({occ})")
    check(invalid == 0, f"{field}: no 0b11 flags")
    key_is_id = isinstance(t.K, IdType)
    n = 0
    ok = True
    for i, ko in hashset_iter(mv, base, t):
        kraw = bytes(mv[ko:ko + t.K.size])
        if n < n_show:
            k = t.K.read(mv, ko)
            print(f"    slot {i}: key={render(k) if render else pretty(t.K, k)}")
        if n < verify_lookup:
            h = hash_u64_of_key(kraw, key_is_id)
            ok &= hashmap_lookup(mv, base, t, kraw, h) == i
        n += 1
    check(ok, f"{field}: re-lookup of first {min(n, verify_lookup)} live keys via "
              f"{'key.u64._0' if key_is_id else 'K12(key bytes)[0..8)'} finds the same slot")
    return pop, (empty, occ, marked, invalid)


def show_collection(mv, state_t, field, n_show=5, hints=None, pov_render=None, full_check=True):
    f = state_t.by_name[field]
    t, base = f.type, f.offset
    cv = CollectionView(mv, base, t)
    empty, occ, marked, invalid = cv.pov_counts()
    print(f"  {state_t.name}.{field}: {t.name}  @offset {base}  sizeof={t.size}  sizeof(Element)={t.E.size}")
    print(f"    _population={cv.population}  _markRemovalCounter={cv.markRemovalCounter}  "
          f"PoV flags: empty(00)={empty} occupied(01)={occ} marked(10)={marked} invalid(11)={invalid}")
    pov_render = pov_render or idstr

    # (1) dense prefix: all elements >= _population are zero
    t0 = time.time()
    tail_zero = is_zero(mv, cv.eoff(cv.population), cv.elems_off + t.L * t.E.size)
    check(tail_zero, f"{field}: _elements[_population .. L) is entirely zero -> live elements are the dense prefix [0, {cv.population})"
                     f"  ({(t.L - cv.population)} slots scanned in {time.time() - t0:.1f}s)")

    # (2) every element in the prefix references an occupied PoV; sum of PoV populations == _population
    sum_pop = 0
    n_povs = 0
    max_pop = (0, None)
    for p, pov in cv.iter_povs():
        sum_pop += pov["population"]
        n_povs += 1
        if pov["population"] > max_pop[0]:
            max_pop = (pov["population"], p)
    check(sum_pop == cv.population, f"{field}: sum of population over occupied PoVs ({sum_pop}, {n_povs} PoVs) == _population ({cv.population})")
    bad_ref = 0
    per_pov = {}
    for i in range(cv.population):
        p = cv.pov_index(i)
        if not (0 <= p < t.L) or cv.pov_state(p) != 1:
            bad_ref += 1
        else:
            per_pov[p] = per_pov.get(p, 0) + 1
    check(bad_ref == 0, f"{field}: every element i < _population has povIndex referencing an occupied (0b01) PoV slot")
    mism = sum(1 for p, pov in cv.iter_povs() if per_pov.get(p, 0) != pov["population"])
    check(mism == 0, f"{field}: per-PoV element counts (grouping elements by povIndex) == PoV.population for all {n_povs} PoVs")

    # (3) marked-for-removal PoVs have population 0 (value/head/tail/root are stale, NOT cleared)
    if marked:
        stale = 0
        all_zero_pop = True
        for p, pov in cv.iter_povs(want=2):
            all_zero_pop &= pov["population"] == 0
            stale += pov["value"] != bytes(32)
        check(all_zero_pop, f"{field}: all {marked} PoV slots flagged 0b10 have population == 0 "
                            f"({stale} of them still hold a stale non-zero PoV id)")

    # (4) priority-order traversal of PoVs: head -> next ... equals in-order BST traversal, priorities non-increasing
    t0 = time.time()
    order_ok = count_ok = head_ok = tail_ok = inorder_ok = root_ok = True
    checked = 0
    for p, pov in cv.iter_povs():
        if not full_check and checked >= 2000:
            break
        seq = list(cv.iter_pov_elements(pov))
        count_ok &= len(seq) == pov["population"]
        head_ok &= seq[0] == pov["headIndex"] and cv.prev_index(seq[0]) == NULL_INDEX
        tail_ok &= seq[-1] == pov["tailIndex"] and cv.next_index(seq[-1]) == NULL_INDEX
        root_ok &= cv.parent(pov["bstRootIndex"]) == NULL_INDEX
        pr = [cv.priority(i) for i in seq]
        order_ok &= all(pr[k] >= pr[k + 1] for k in range(len(pr) - 1))
        if pov["population"] <= 4096:
            inorder_ok &= seq == list(cv.iter_pov_elements_inorder(pov))
        checked += 1
    print(f"    (traversed {checked} PoVs in {time.time() - t0:.1f}s; largest PoV has {max_pop[0]} elements)")
    check(count_ok, f"{field}: head->nextElementIndex traversal visits exactly PoV.population elements")
    check(head_ok, f"{field}: PoV.headIndex is the first in-order element (no predecessor)")
    check(tail_ok, f"{field}: PoV.tailIndex is the last in-order element (no successor)")
    check(root_ok, f"{field}: element at PoV.bstRootIndex has bstParentIndex == NULL_INDEX")
    check(order_ok, f"{field}: priorities are non-increasing from head to tail (head = highest priority)")
    check(inorder_ok, f"{field}: successor-walk order == recursive in-order (left,node,right) order of the BST")

    # (5) evidence: first elements of the dense array, with their PoV
    print(f"    first {n_show} elements of _elements[] (array order) with their PoV:")
    for i in range(min(n_show, cv.population)):
        p = cv.pov_index(i)
        pov = cv.pov(p)
        v = pretty(t.T, cv.value(i), hints)
        print(f"      [{i}] priority={cv.priority(i)} povIndex={p} pov={pov_render(pov['value'])} "
              f"bst(parent={cv.parent(i)}, left={cv.left(i)}, right={cv.right(i)})")
        print(f"           value={v}")
    # (6) evidence: one PoV queue in priority order
    shown = 0
    for p, pov in cv.iter_povs():
        if pov["population"] >= 3:
            print(f"    PoV slot {p}: pov={pov_render(pov['value'])} population={pov['population']} head={pov['headIndex']} "
                  f"tail={pov['tailIndex']} bstRoot={pov['bstRootIndex']}; queue (head first, max {n_show}):")
            for i in cv.iter_pov_elements(pov, limit=n_show):
                print(f"      elem {i}: priority={cv.priority(i)} value={pretty(t.T, cv.value(i), hints)}")
            shown += 1
            if shown >= 2:
                break
    return cv


def qx_assetorders_pov(b):
    """QX composes the _assetOrders PoV as issuer id with u64._3 overwritten by the asset name (Qx.h:290-291)."""
    name = int.from_bytes(b[24:32], "little")
    return f"[issuer.u64._0.._2={b[:24].hex()} | assetName={asset_name_to_str(name)!r}]"


def main():
    t_start = time.time()

    # ---------------- QX -------------------------------------------------------------------------------------------
    path, mv = open_state(1)
    t = cs.QX()
    print(f"=== QX  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == t.size, "QX: file size == sizeof(QX::StateData)")
    hdr = {n: t.ftype(n).read(mv, t.off(n)) for n in ("_earnedAmount", "_distributedAmount", "_burnedAmount",
                                                       "_assetIssuanceFee", "_transferFee", "_tradeFee")}
    print(f"  scalars: {hdr}")
    check(hdr["_assetIssuanceFee"] == 1000000000 and hdr["_transferFee"] == 100 and hdr["_tradeFee"] == 3000000,
          "QX: fee scalars at offsets 24/28/32 hold exactly the values assigned in INITIALIZE (Qx.h:1140-1149: 1e9, 100, 3e6)")
    show_collection(mv, t, "_assetOrders", pov_render=qx_assetorders_pov)
    show_collection(mv, t, "_entityOrders", hints={"asset_name_fields": ("assetName",)})
    tail_off = t.off("_elementIndex")
    print(f"  trailing scalar block starts @{tail_off}: _tradeMessage = "
          f"{pretty(t.ftype('_tradeMessage'), t.ftype('_tradeMessage').read(mv, t.off('_tradeMessage')))}")

    # ---------------- QUOTTERY -------------------------------------------------------------------------------------
    path, mv = open_state(2)
    t = cs.QUOTTERY()
    print(f"\n=== QUOTTERY  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == t.size, "QUOTTERY: file size == sizeof(QUOTTERY::StateData)")
    show_hashmap(mv, t, "mEventInfo", n_show=3)
    show_hashmap(mv, t, "mEventResult")
    show_hashmap(mv, t, "mEventFinalFlag")
    show_hashmap(mv, t, "mPositionInfo")
    show_collection(mv, t, "mABOrders", pov_render=lambda b: f"[entity.u64._0.._2={b[:24].hex()} | u64._3=0x{int.from_bytes(b[24:], 'little'):016x}]")
    print(f"  mCurrentEventID = {u64(mv, t.off('mCurrentEventID'))}; mQtryGov = {pretty(t.ftype('mQtryGov'), t.ftype('mQtryGov').read(mv, t.off('mQtryGov')))}")
    op = t.ftype("mOperationParams")
    show_hashmap(mv[t.off("mOperationParams"):], op, "discountedFeeForUsers")  # nested struct: slice the view at its offset

    # ---------------- QBOND ----------------------------------------------------------------------------------------
    path, mv = open_state(17)
    t = cs.QBOND()
    print(f"\n=== QBOND  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == t.size, "QBOND: file size == sizeof(QBOND::StateData)")
    show_hashmap(mv, t, "_epochMbondInfoMap", hints={"asset_name_fields": ("name",)})
    show_hashmap(mv, t, "_userTotalStakedMap")
    show_hashset(mv, t, "_commissionFreeAddresses")
    for n in ("_adminAddress", "_devAddress"):
        print(f"  {n} = {idstr(id_read(mv, t.off(n)))}")
    show_collection(mv, t, "_askOrders")
    show_collection(mv, t, "_bidOrders")

    # ---------------- QIP ------------------------------------------------------------------------------------------
    path, mv = open_state(18)
    t = cs.QIP()
    print(f"\n=== QIP  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == t.size, "QIP: file size == sizeof(QIP::StateData)")
    show_hashset(mv, t, "activeIcoIndexes")
    show_hashmap(mv, t, "buyersInfo")

    # ---------------- NOST -----------------------------------------------------------------------------------------
    path, mv = open_state(14)
    t = cs.NOST()
    print(f"\n=== NOST  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == t.size, "NOST: file size == sizeof(NOST::StateData)")
    show_hashmap(mv, t, "users")
    show_hashmap(mv, t, "numberOfVotedProject")
    show_hashset(mv, t, "tokens", render=lambda v: f"{asset_name_to_str(v)!r} ({v})")
    show_hashmap(mv, t, "numberOfInvestedProjects")
    print(f"  teamAddress = {idstr(id_read(mv, t.off('teamAddress')))}")

    # ---------------- QRP (file is sizeof(IPO), StateData is the prefix) ---------------------------------------------
    path, mv = open_state(21)
    t = cs.QRP()
    print(f"\n=== QRP  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == cs.IPO.size, "QRP: file size == sizeof(IPO) (contractDescriptions uses sizeof(IPO) for QRP)")
    print(f"  teamAddress  = {idstr(id_read(mv, t.off('teamAddress')))}")
    print(f"  ownerAddress = {idstr(id_read(mv, t.off('ownerAddress')))}")
    show_hashset(mv, t, "allowedSmartContracts")
    check(is_zero(mv, t.size, len(mv)), f"QRP: bytes [{t.size}, {len(mv)}) behind StateData are all zero")

    # ---------------- ESCROW ---------------------------------------------------------------------------------------
    path, mv = open_state(27)
    t = cs.ESCROW()
    print(f"\n=== ESCROW  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == t.size, "ESCROW: file size == sizeof(ESCROW::StateData)")
    show_hashset(mv, t, "_earnedTokens")
    show_hashmap(mv, t, "_deals", n_show=2, hints={"asset_name_fields": ("name",)})
    show_hashmap(mv, t, "_dealIndexOwnerMap")
    show_hashset(mv, t, "_ownersSet")
    show_collection(mv, t, "_ownerDealIndexes")
    show_collection(mv, t, "_reservedAssets", hints={"asset_name_fields": ("name",)})

    # ---------------- GGWP -----------------------------------------------------------------------------------------
    path, mv = open_state(28)
    t = cs.WOLFPACK()
    print(f"\n=== GGWP  {path}  file size {len(mv)}  model sizeof {t.size}")
    check(len(mv) == t.size, "GGWP: file size == sizeof(WOLFPACK::StateData)")
    print(f"  adminAddress = {idstr(id_read(mv, t.off('adminAddress')))}")
    wp = t.ftype("wpToken").read(mv, t.off("wpToken"))
    print(f"  wpToken = issuer {idstr(wp['issuer'])} assetName {asset_name_to_str(wp['assetName'])!r} ({wp['assetName']})")
    hdr, _ = show_hashmap(mv, t, "holderBalances")
    hc = u64(mv, t.off("holderCount"))
    check(hc == hdr["population"], f"GGWP: scalar holderCount ({hc}) == holderBalances._population ({hdr['population']})")
    hdr, _ = show_hashmap(mv, t, "stakedBalances")
    sc = u64(mv, t.off("stakerCount"))
    check(sc == hdr["population"], f"GGWP: scalar stakerCount ({sc}) == stakedBalances._population ({hdr['population']})")
    show_hashmap(mv, t, "clanRanks")
    show_hashmap(mv, t, "govVoteMap")

    # ---------------- contract 0 + IPO-shaped files ------------------------------------------------------------------
    path, mv = open_state(0)
    fees = struct.unpack_from("<1024q", mv, 0)
    print(f"\n=== contract 0  {path}: contractFeeReserves[0..28] = {list(fees[:29])}")
    check(all(v == 0 for v in fees[29:]), "contract0: contractFeeReserves[29..1023] are zero (only 29 contracts exist in epoch 229)")

    print("\n=== contract ids rendered as identities (id(contractIndex,0,0,0))")
    for i in (0, 1, 2, 4, 9, 17, 28):
        print(f"  contract {i:>2}: {identity_from_pubkey(contract_id(i))}")

    n_fail = sum(1 for ok, _ in CHECKS if not ok)
    print(f"\nSUMMARY: {len(CHECKS) - n_fail} checks passed, {n_fail} failed, total time {time.time() - t_start:.1f}s")
    return 1 if n_fail else 0


def id_read(mv, off):
    return bytes(mv[off:off + 32])


if __name__ == "__main__":
    sys.exit(main())
