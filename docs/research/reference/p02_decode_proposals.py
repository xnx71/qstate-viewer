#!/usr/bin/env python3
"""
p02_decode_proposals.py -- decode QPI ProposalVoting<> storage from Qubic contract state files (python3 stdlib only).

Evidence script for research report 02-qpi-proposals.md. Every offset is computed from the generic layout formulas of
the report (class Layout), so the script doubles as executable pseudocode for the viewer's "proposal view":

  ProposalVoting<H, D>            = { H proposersAndVoters; ProposalWithAllVoteData<D, H::maxVotes> proposals[H::maxProposals]; }
  ProposalAndVotingByComputors<N> = { id currentProposalProposers[N]; }                         maxVotes = 676
  ProposalByAnyoneVotingByComputors<N> : ProposalAndVotingByComputors<N> {}                      (same layout)
  ProposalAndVotingByShareholders<N, asset>
                                  = { id currentProposalProposers[N]; id currentProposalShareholders[N][676]; }
  ProposalWithAllVoteData<D, V>   = D (base class) followed by the vote storage:
        D = ProposalDataYesNo     -> uint8  votes[(2*V+7)/8]   2 bits per vote, 3 = no vote   (partial specialization)
        D = ProposalDataV1<false> -> uint8  votes[V]           0xff = no vote
        D = ProposalDataV1<true>  -> sint64 votes[V]           0x8000000000000000 = no vote

Usage:
  p02_decode_proposals.py selftest
  p02_decode_proposals.py samples [--dir /home/yeti/devwork/space/229] [--epoch 229]
                                  [--contract gqmprop|ccf|qutil|all] [--limit N] [--votes]
  p02_decode_proposals.py blob --file F [--offset O] --handling computors|shareholders --slots N
                               --data YesNo|V1false|V1true [--state-epoch E] [--limit N] [--votes] [--canonical]

`--canonical` prints the dump format of p02_make_fixtures.cpp (which is produced by the real core code), so
`diff <(p02_decode_proposals.py blob ... --canonical) fixtureX.expected.txt` proves the decoder.
Large files (QUTIL: 402 MB) are never read completely: only the slice holding `proposals` is read (seek + read).
"""
import argparse
import os
import struct
import sys

NUMBER_OF_COMPUTORS = 676                          # src/qpi/qpi_types.h:28
QUORUM = NUMBER_OF_COMPUTORS * 2 // 3 + 1          # 451, src/qpi/qpi_types.h:29
NO_VOTE_VALUE = -0x8000000000000000                # src/qpi/qpi_proposals.h:9
MIN_SUPPORTED = -0x7FFFFFFFFFFFFFFF                # ProposalDataV1::Data::VariableScalar::minSupportedValue (:287)
MAX_SUPPORTED = 0x7FFFFFFFFFFFFFFF                 # ProposalDataV1::Data::VariableScalar::maxSupportedValue (:288)

# ----------------------------------------------------------------------------------------------------------------------
# KangarooTwelve (KT128) for short messages, pure python -- needed only for the 4 checksum letters of a Qubic identity
# ----------------------------------------------------------------------------------------------------------------------
_RC = [
    0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000, 0x000000000000808B,
    0x0000000080000001, 0x8000000080008081, 0x8000000000008009, 0x000000000000008A, 0x0000000000000088,
    0x0000000080008009, 0x000000008000000A, 0x000000008000808B, 0x800000000000008B, 0x8000000000008089,
    0x8000000000008003, 0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
]
_ROT = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61], [28, 55, 25, 21, 56], [27, 20, 39, 8, 14]]
_M64 = (1 << 64) - 1


def _rol(v, n):
    n %= 64
    return ((v << n) | (v >> (64 - n))) & _M64 if n else v


def _keccak_p1600_12(a):
    """Keccak-p[1600, 12 rounds] (= last 12 rounds of Keccak-f[1600]); a = 5x5 lanes a[x][y]."""
    for rnd in range(12, 24):
        c = [a[x][0] ^ a[x][1] ^ a[x][2] ^ a[x][3] ^ a[x][4] for x in range(5)]
        d = [c[(x - 1) % 5] ^ _rol(c[(x + 1) % 5], 1) for x in range(5)]
        a = [[a[x][y] ^ d[x] for y in range(5)] for x in range(5)]
        b = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                b[y][(2 * x + 3 * y) % 5] = _rol(a[x][y], _ROT[x][y])
        a = [[b[x][y] ^ ((~b[(x + 1) % 5][y]) & b[(x + 2) % 5][y]) for y in range(5)] for x in range(5)]
        a[0][0] ^= _RC[rnd]
    return a


def _turboshake128(msg, domain, outlen):
    rate = 168
    buf = bytearray(msg) + bytes([domain])
    buf += b"\x00" * (-len(buf) % rate)
    buf[-1] ^= 0x80
    a = [[0] * 5 for _ in range(5)]
    for off in range(0, len(buf), rate):
        for i in range(rate // 8):
            a[i % 5][i // 5] ^= struct.unpack_from("<Q", buf, off + 8 * i)[0]
        a = _keccak_p1600_12(a)
    out = b""
    while len(out) < outlen:
        out += b"".join(struct.pack("<Q", a[i % 5][i // 5]) for i in range(rate // 8))
        if len(out) < outlen:
            a = _keccak_p1600_12(a)
    return out[:outlen]


def k12(msg, outlen):
    """KangarooTwelve with empty customization string, valid for len(msg) <= 8191 (single leaf)."""
    assert len(msg) <= 8191
    return _turboshake128(bytes(msg) + b"\x00", 0x07, outlen)


def identity(pk, lower=False):
    """Qubic 60-letter identity of a 32-byte public key (core: getIdentity() in src/four_q.h:1777)."""
    base = ord("a") if lower else ord("A")
    out = []
    for i in range(4):
        frag = struct.unpack_from("<Q", pk, 8 * i)[0]
        for _ in range(14):
            out.append(chr(base + frag % 26))
            frag //= 26
    chk = int.from_bytes(k12(pk, 3), "little") & 0x3FFFF
    for _ in range(4):
        out.append(chr(base + chk % 26))
        chk //= 26
    return "".join(out)


def id_str(pk):
    """Readable id: NULL_ID, small 'contract index' ids (id(n,0,0,0)), else the 60-letter identity."""
    if pk == b"\x00" * 32:
        return "NULL_ID"
    q = struct.unpack("<4Q", pk)
    if q[1] == q[2] == q[3] == 0 and q[0] < 1024:
        return "%s (= id(%d,0,0,0), contract index %d)" % (identity(pk), q[0], q[0])
    return identity(pk)


def id_key(pk):
    """Sort key equal to m256i operator< (src/platform/m256.h:295): compare u64 limbs 0,1,2,3 in this order."""
    return struct.unpack("<4Q", pk)


# ----------------------------------------------------------------------------------------------------------------------
# Proposal type helpers (src/qpi/qpi_proposals.h:137-234)
# ----------------------------------------------------------------------------------------------------------------------
CLASS_NAMES = {0x000: "GeneralOptions", 0x100: "Transfer", 0x200: "Variable", 0x300: "MultiVariables",
               0x400: "TransferInEpoch"}


def type_str(t):
    cls, opts = t & 0xFF00, t & 0x00FF
    name = CLASS_NAMES.get(cls, "UnknownClass(0x%x)" % cls)
    if t == 0:
        return "0x0000 (Invalid/unset)"
    if cls == 0x200 and opts == 0:
        return "0x%04x Variable, scalar voting (VariableScalarMean)" % t
    return "0x%04x %s, %d options" % (t, name, opts)


def url_str(b):
    return b.split(b"\x00", 1)[0].decode("utf-8", "replace")


def align_up(v, a):
    return (v + a - 1) // a * a


def c_div(a, b):
    """C++ integer division (truncation toward zero)."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def c_mod(a, b):
    return a - b * c_div(a, b)


# ----------------------------------------------------------------------------------------------------------------------
# Layout (generic formulas; every involved class has alignment 8)
# ----------------------------------------------------------------------------------------------------------------------
class Layout:
    def __init__(self, handling, slots, data_type, max_votes=NUMBER_OF_COMPUTORS):
        self.handling, self.slots, self.data_type, self.max_votes = handling, slots, data_type, max_votes
        # --- ProposerAndVoterHandlingT
        self.off_proposers = 0
        if handling == "computors":            # ProposalAndVotingByComputors / ProposalByAnyoneVotingByComputors
            self.size_handling = 32 * slots
            self.off_shareholders = None
        elif handling == "shareholders":       # ProposalAndVotingByShareholders
            self.off_shareholders = 32 * slots
            self.size_handling = 32 * slots + 32 * slots * NUMBER_OF_COMPUTORS
        else:
            raise ValueError(handling)
        # --- ProposalDataT (base class of the array element)
        if data_type == "YesNo":
            self.size_data = 256 + 8 + 40      # static_assert src/qpi/qpi_proposals.h:437
            self.vote_bytes = (2 * max_votes + 7) // 8
        elif data_type == "V1false":
            self.size_data = 256 + 8 + 64      # static_assert src/qpi/qpi_proposals.h:363
            self.vote_bytes = max_votes
        elif data_type == "V1true":
            self.size_data = 256 + 8 + 64
            self.vote_bytes = 8 * max_votes
        else:
            raise ValueError(data_type)
        self.off_votes = self.size_data        # base has no tail padding; votes start right behind it
        self.size_elem = align_up(self.size_data + self.vote_bytes, 8)
        self.off_proposals = self.size_handling
        self.size = self.size_handling + slots * self.size_elem

    def describe(self):
        return ("ProposalVoting<%s<%d>, %s>: sizeof=%d  proposersAndVoters@0 size %d  proposals@%d = %d x %d  "
                "(elem: data %d bytes, votes@%d %d bytes)" %
                (self.handling, self.slots, self.data_type, self.size, self.size_handling, self.off_proposals,
                 self.slots, self.size_elem, self.size_data, self.off_votes, self.vote_bytes))


def get_votes(lay, elem):
    """All vote values with getVoteValue() semantics (qpi_proposals_impl.h:456-476 / 531-544)."""
    raw = elem[lay.off_votes:lay.off_votes + lay.vote_bytes]
    if lay.data_type == "YesNo":
        res = []
        for i in range(lay.max_votes):
            v = (raw[i >> 2] >> ((i & 3) * 2)) & 3
            res.append(NO_VOTE_VALUE if v == 3 else v)
        return res
    if lay.data_type == "V1false":
        return [NO_VOTE_VALUE if b == 0xFF else b for b in raw]
    return list(struct.unpack("<%dq" % lay.max_votes, raw))


def decode_data(lay, typ, d):
    """Decode the `data` union according to the proposal type class. Returns list of 'name = value' strings."""
    cls, opts = typ & 0xFF00, typ & 0xFF
    out = []
    if cls == 0x100:      # Transfer
        out.append("data.transfer.destination = " + id_str(d[0:32]))
        if lay.data_type == "YesNo":
            out.append("data.transfer.amount = %d" % struct.unpack_from("<q", d, 32)[0])
        else:
            am = struct.unpack_from("<4q", d, 32)
            out.append("data.transfer.amounts = %s   (first %d used: options 1..%d)" % (list(am), max(opts - 1, 0), max(opts - 1, 0)))
    elif cls == 0x400 and lay.data_type != "YesNo":    # TransferInEpoch (ProposalDataV1 only)
        out.append("data.transferInEpoch.destination = " + id_str(d[0:32]))
        out.append("data.transferInEpoch.amount = %d" % struct.unpack_from("<q", d, 32)[0])
        out.append("data.transferInEpoch.targetEpoch = %d" % struct.unpack_from("<H", d, 40)[0])
    elif cls == 0x200:    # Variable
        if opts == 0 and lay.data_type != "YesNo":     # VariableScalarMean (ProposalDataV1<true> only)
            var, mn, mx, pv = struct.unpack_from("<Qqqq", d, 0)
            out.append("data.variableScalar = {variable %d, minValue %d, maxValue %d, proposedValue %d}" % (var, mn, mx, pv))
        elif lay.data_type == "YesNo":
            var, val = struct.unpack_from("<Qq", d, 0)
            out.append("data.variableOptions = {variable %d, value %d}" % (var, val))
        else:
            var = struct.unpack_from("<Q", d, 0)[0]
            vals = struct.unpack_from("<4q", d, 8)
            out.append("data.variableOptions = {variable %d, values %s}   (first %d used: options 1..%d)" %
                       (var, list(vals), max(opts - 1, 0), max(opts - 1, 0)))
    elif cls in (0x000, 0x300):
        out.append("data: unused by this class (%s)" % ("all zero" if not any(d) else "NON-ZERO: " + d.hex()))
    else:
        out.append("data (unknown class) = " + d.hex())
    return out


def summarize(lay, typ, votes, data):
    """QpiContextProposalFunctionCall::getVotingSummary (qpi_proposals_impl.h:959-1004) +
    ProposalSummarizedVotingDataV1::getMostVotedOption/getAcceptedOption (qpi_proposals.h:90-118)."""
    opts = typ & 0xFF
    res = {"authorized": lay.max_votes, "optionCount": opts}
    if typ == 0x200:      # VariableScalarMean -> __getVotingSummaryScalarVotes (qpi_proposals_impl.h:884-946)
        if lay.data_type != "V1true":
            return None   # getVotingSummary returns false
        _var, mn, mx, _pv = struct.unpack_from("<Qqqq", data, 0)
        vals = [v for v in votes if v != NO_VOTE_VALUE]
        n = len(vals)
        acc = 0
        if mx > c_div(MAX_SUPPORTED, lay.max_votes) or mn < c_div(MIN_SUPPORTED, lay.max_votes):
            if n:         # overflow-safe mean
                acc2 = 0
                for v in vals:
                    acc += c_div(v, n)
                    acc2 += c_mod(v, n)
                acc += c_div(acc2, n)
        else:
            acc = sum(vals)
            if n:
                acc = c_div(acc, n)
        res.update(casted=n, scalar=acc, most_voted=-1, accepted=-1)
        return res
    hist = [0] * 8
    casted = 0
    for v in votes:
        if v != NO_VOTE_VALUE and 0 <= v < opts:
            hist[v] += 1
            casted += 1
    most = -1
    if opts:
        most = 0
        for i in range(1, opts):
            if hist[most] < hist[i]:
                most = i
    accepted = most if (casted >= QUORUM and most >= 0 and hist[most] > QUORUM // 2) else -1
    res.update(casted=casted, hist=hist, most_voted=most, accepted=accepted)
    return res


def voter_runs(lay, blob, slot):
    """Shareholder voting: consecutive runs of equal ids in currentProposalShareholders[slot] = (id, firstVoteIndex, count)."""
    sh0 = lay.off_shareholders + slot * NUMBER_OF_COMPUTORS * 32
    runs = []
    for v in range(NUMBER_OF_COMPUTORS):
        h = blob[sh0 + 32 * v: sh0 + 32 * v + 32]
        if runs and runs[-1][0] == h:
            runs[-1][2] += 1
        else:
            runs.append([h, v, 1])
    return runs


def compact(vals):
    out = ["-" if v == NO_VOTE_VALUE else str(v) for v in vals]
    return "".join(out) if all(len(o) == 1 for o in out) else ",".join(out)


def read_blob(path, file_off, size):
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        fsize = f.tell()
        f.seek(file_off)
        blob = f.read(size)
    if len(blob) != size:
        raise SystemExit("short read: %s offset %d size %d (file size %d)" % (path, file_off, size, fsize))
    return blob, fsize


def slots_iter(lay, blob):
    for i in range(lay.slots):
        proposer = blob[lay.off_proposers + 32 * i: lay.off_proposers + 32 * i + 32]
        e0 = lay.off_proposals + i * lay.size_elem
        elem = blob[e0:e0 + lay.size_elem]
        epoch, typ, tick = struct.unpack_from("<HHI", elem, 256)
        yield i, proposer, elem, epoch, typ, tick


def dump_canonical(lay, blob, out=sys.stdout):
    """Same text as Fixture::dump() in p02_make_fixtures.cpp (which uses the real core read functions)."""
    out.write("sizeof %d maxProposals %d maxVotes %d\n" % (lay.size, lay.slots, lay.max_votes))
    for i, proposer, elem, epoch, typ, tick in slots_iter(lay, blob):
        if epoch == 0:
            continue
        data = elem[264:lay.size_data]
        votes = get_votes(lay, elem)
        out.write("slot %d\n" % i)
        out.write("proposer %s\n" % proposer.hex())
        out.write("url %s\n" % url_str(elem[0:256]))
        out.write("epoch %d type 0x%04x tick %d\n" % (epoch, typ, tick))
        out.write("data %s\n" % data.hex())
        out.write("votes %s\n" % ",".join("-" if v == NO_VOTE_VALUE else str(v) for v in votes))
        s = summarize(lay, typ, votes, data)
        if "scalar" in s:
            out.write("summary authorized=%d casted=%d optionCount=0 scalar=%d\n" % (s["authorized"], s["casted"], s["scalar"]))
        else:
            out.write("summary authorized=%d casted=%d optionCount=%d hist=%s\n" %
                      (s["authorized"], s["casted"], s["optionCount"], ",".join(str(h) for h in s["hist"])))
        out.write("mostVoted=%d accepted=%d\n" % (s["most_voted"], s["accepted"]))
        if lay.off_shareholders is not None:
            out.write("voters %s\n" % "".join("%sx%d@%d;" % (h.hex(), cnt, first) for h, first, cnt in voter_runs(lay, blob, i)))


def dump_voting(name, path, file_off, lay, state_epoch, limit, show_votes):
    blob, fsize = read_blob(path, file_off, lay.size)
    print("=" * 120)
    print("%s  file=%s (size %d)  proposals@%d" % (name, path, fsize, file_off))
    print(lay.describe())
    used = printed = garbage = 0
    for i, proposer, elem, epoch, typ, tick in slots_iter(lay, blob):
        if epoch == 0:
            # cleared / never used slot: clearProposal() zeroes the element and the proposer (and shareholder row)
            sh = b""
            if lay.off_shareholders is not None:
                sh0 = lay.off_shareholders + i * NUMBER_OF_COMPUTORS * 32
                sh = blob[sh0:sh0 + NUMBER_OF_COMPUTORS * 32]
            if any(elem) or any(proposer) or any(sh):
                garbage += 1
            continue
        used += 1
        if limit and printed >= limit:
            continue
        printed += 1
        data = elem[264:lay.size_data]
        votes = get_votes(lay, elem)
        s = summarize(lay, typ, votes, data)
        if state_epoch is None:
            status = "epoch %d" % epoch
        elif epoch == state_epoch:
            status = "ACTIVE (epoch == epoch of the state file: open for voting)"
        else:
            status = "finished (epoch %d)" % epoch
        print("-" * 120)
        print("proposal[%d]  %s" % (i, status))
        print("  proposer (proposersAndVoters.currentProposalProposers[%d]) = %s" % (i, id_str(proposer)))
        print("  url   = %r" % url_str(elem[0:256]))
        print("  epoch = %d   tick = %d   type = %s" % (epoch, tick, type_str(typ)))
        for line in decode_data(lay, typ, data):
            print("  " + line)
        tail = elem[lay.off_votes + lay.vote_bytes:]
        print("  votes: storage %d bytes @%d, tail padding %d bytes %s" % (lay.vote_bytes, lay.off_votes, len(tail), tail.hex()))
        if s is None:
            print("  result: n/a (scalar type in non-scalar storage)")
        elif "hist" in s:
            labels = ["opt%d%s=%d" % (k, " (no / no change)" if k == 0 else "", n) for k, n in enumerate(s["hist"][:s["optionCount"]])]
            print("  result: casted %d / %d authorized; %s; most voted option %d; getAcceptedOption() = %d "
                  "(needs casted >= %d and votes(most voted) > %d)" %
                  (s["casted"], s["authorized"], ", ".join(labels), s["most_voted"], s["accepted"], QUORUM, QUORUM // 2))
        else:
            print("  result: casted %d / %d authorized; scalar mean %d" % (s["casted"], s["authorized"], s["scalar"]))
        if lay.off_shareholders is not None:
            runs = voter_runs(lay, blob, i)
            is_sorted = all(id_key(runs[k][0]) < id_key(runs[k + 1][0]) for k in range(len(runs) - 1))
            print("  shareholders snapshot: %d distinct voters over %d votes; strictly ascending (m256i operator<): %s" %
                  (len(runs), NUMBER_OF_COMPUTORS, is_sorted))
            if show_votes:
                for h, first, cnt in runs:
                    print("    voteIndex %3d..%3d (%3d shares) %s votes=%s" %
                          (first, first + cnt - 1, cnt, id_str(h), compact(votes[first:first + cnt])))
        elif show_votes:
            casted = [(v, x) for v, x in enumerate(votes) if x != NO_VOTE_VALUE]
            print("    votes as computorIndex:value (voteIndex == computor index in epoch %d): %s" %
                  (epoch, " ".join("%d:%d" % p for p in casted)))
    print("-" * 120)
    print("%s: %d used proposal slots of %d (%d printed), %d free slots with non-zero residue" % (name, used, lay.slots, printed, garbage))
    return blob


def selftest():
    # KT128("", "", 32) from the KangarooTwelve reference / RFC 9861 test vectors
    assert k12(b"", 32).hex() == "1ac2d450fc3b4205d19da7bfca1b37513c0803577ac7167f06fe2ce1f0ef39e5", k12(b"", 32).hex()
    # ARBITRATOR identity from src/public_settings.h:84 -> decode the first 56 letters to a public key and re-encode
    arb = "AFZPUAIYVPNUYGJRQVLUKOPPVLHAZQTGLYAAUUNBXFTVTAMSBKQBLEIEPCVJ"
    pk = b""
    for i in range(4):
        frag = 0
        for j in reversed(range(14)):
            frag = frag * 26 + (ord(arb[i * 14 + j]) - 65)
        pk += struct.pack("<Q", frag)
    assert identity(pk) == arb, identity(pk)
    # sizeof test vectors (g++ oracle p02_oracle_proposals.cpp + doc/contracts_proposals.md:71)
    assert Layout("computors", 676, "V1false").size == 703040          # GQMPROP::ProposalVotingT
    assert Layout("computors", 100, "YesNo").size == 51200             # CCF::ProposalVotingT
    assert Layout("shareholders", 8, "YesNo").size == 177152 == 8 * 22144   # QUTIL::ProposalVotingT
    assert Layout("shareholders", 16, "YesNo").size == 354304          # TESTEXA::ProposalVotingT
    assert Layout("shareholders", 16, "V1true").size == 438400         # TESTEXB::ProposalVotingT
    assert Layout("computors", 200, "V1true").size == 1153600          # test/qpi.cpp
    assert Layout("computors", 200, "V1false").size == 208000          # test/qpi.cpp
    assert [Layout("computors", 1, d, 42).size_elem for d in ("YesNo", "V1false", "V1true")] == [320, 376, 664]
    assert c_div(-7, 2) == -3 and c_mod(-7, 2) == -1 and c_div(7, -2) == -3
    print("selftest ok: K12 vector, identity checksum (ARBITRATOR), 10 sizeof test vectors")


def cmd_samples(a):
    def path(idx):
        return os.path.join(a.dir, "contract%04d.%03d" % (idx, a.epoch))

    if a.contract in ("gqmprop", "all"):
        # GQMPROP::StateData { ProposalVotingT proposals @0; Array<RevenueDonationEntry,128> revenueDonation @703040; }
        lay = Layout("computors", NUMBER_OF_COMPUTORS, "V1false")
        p = path(6)
        assert os.path.getsize(p) == lay.size + 128 * 48 == 709184
        dump_voting("GQMPROP (contract 6)", p, 0, lay, a.epoch, a.limit, a.votes)
        rd, _ = read_blob(p, lay.size, 128 * 48)
        print("GQMPROP revenueDonation (Array<RevenueDonationEntry,128> @%d; entry = id + sint64 + uint16 -> 48 bytes):" % lay.size)
        for i in range(128):
            e = rd[48 * i:48 * i + 48]
            if not any(e[:32]):
                print("  [%d] NULL_ID -> end of table" % i)
                break
            amt, ep = struct.unpack_from("<qH", e, 32)
            print("  [%d] %s  millionthAmount=%d (%.4f%%)  firstEpoch=%d" % (i, id_str(e[:32]), amt, amt / 10000.0, ep))

    if a.contract in ("ccf", "all"):
        # CCF::StateData { ProposalVotingT proposals @0 (51200); LatestTransfersT latestTransfers @51200; ... } = 493584
        lay = Layout("computors", 100, "YesNo")
        p = path(8)
        assert os.path.getsize(p) == 493584
        blob = dump_voting("CCF (contract 8)", p, 0, lay, a.epoch, a.limit, a.votes)
        rest, _ = read_blob(p, 51200, 38912 + 8)
        idx = rest[38912]
        fee = struct.unpack_from("<I", rest, 38912 + 4)[0]
        print("CCF lastTransfersNextOverwriteIdx=%d setProposalFee=%d" % (idx, fee))
        # cross-check: every latestTransfers entry {id destination; Array<uint8,256> url; sint64 amount; uint32 tick; bit success}
        # (304 bytes) should correspond to a stored proposal with the same url+destination+amount (unless the slot was reused).
        props = {}
        for i, proposer, elem, epoch, typ, tick in slots_iter(lay, blob):
            if epoch:
                props[(url_str(elem[0:256]), elem[264:296], struct.unpack_from("<q", elem, 296)[0])] = (i, epoch)
        n = matched = 0
        for k in range(128):
            e = rest[304 * k:304 * k + 304]
            if not any(e):
                continue
            n += 1
            key = (url_str(e[32:288]), e[0:32], struct.unpack_from("<q", e, 288)[0])
            if key in props:
                matched += 1
        print("CCF latestTransfers cross-check: %d non-empty log entries, %d match a stored proposal (url+destination+amount)" % (n, matched))

    if a.contract in ("qutil", "all"):
        # QUTIL::StateData: `ProposalVotingT proposals` is the LAST member @402717952 (177152 bytes); file size 402895104
        lay = Layout("shareholders", 8, "YesNo")
        p = path(4)
        fsize = os.path.getsize(p)
        off = fsize - lay.size
        assert fsize == 402895104 and off == 402717952
        dump_voting("QUTIL (contract 4)", p, off, lay, a.epoch, a.limit, a.votes)
        fees, _ = read_blob(p, 402717864, 40)
        print("QUTIL fee variables 0..4 (smt1InvocationFee, pollCreationFee, pollVoteFee, distributeQuToShareholderFeePerShareholder, "
              "shareholderProposalFee) @402717864 = %s" % (list(struct.unpack("<5q", fees)),))


def cmd_blob(a):
    lay = Layout(a.handling, a.slots, a.data)
    if a.canonical:
        blob, _ = read_blob(a.file, a.offset, lay.size)
        dump_canonical(lay, blob)
    else:
        dump_voting(os.path.basename(a.file), a.file, a.offset, lay, a.state_epoch, a.limit, a.votes)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("selftest")
    s = sub.add_parser("samples")
    s.add_argument("--dir", default="/home/yeti/devwork/space/229")
    s.add_argument("--epoch", type=int, default=229)
    s.add_argument("--contract", default="all", choices=["gqmprop", "ccf", "qutil", "all"])
    s.add_argument("--limit", type=int, default=0, help="max proposals printed per contract (0 = all)")
    s.add_argument("--votes", action="store_true", help="list individual votes")
    b = sub.add_parser("blob")
    b.add_argument("--file", required=True)
    b.add_argument("--offset", type=int, default=0)
    b.add_argument("--handling", required=True, choices=["computors", "shareholders"])
    b.add_argument("--slots", type=int, required=True)
    b.add_argument("--data", required=True, choices=["YesNo", "V1false", "V1true"])
    b.add_argument("--state-epoch", type=int, default=None)
    b.add_argument("--limit", type=int, default=0)
    b.add_argument("--votes", action="store_true")
    b.add_argument("--canonical", action="store_true")
    a = ap.parse_args()
    if a.cmd == "selftest":
        selftest()
    elif a.cmd == "blob":
        cmd_blob(a)
    elif a.cmd == "samples":
        selftest()
        cmd_samples(a)
    else:
        ap.print_help()


if __name__ == "__main__":
    main()
