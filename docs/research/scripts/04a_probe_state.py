#!/usr/bin/env python3
"""
04a_probe_state.py -- read selected fields of the epoch-229 contract state files using the offsets computed by
04a_contract_layouts.py (contract-layouts-a.json).  Serves as an end-to-end sanity check of the layout (values must be
plausible) and documents the per-contract semantics quoted in research/04-contract-states-a.md.

usage: 04a_probe_state.py [--json PATH] [--version v1.303.2] [idx:path ...]
  path grammar:  field | field.sub | field[3].sub | container._population | container._elements[5].value.x
  without arguments the built-in probe list is executed.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from qubic_identity import identity_from_pubkey, asset_name_to_str   # sibling research helper (stdlib only)
except Exception:  # pragma: no cover
    identity_from_pubkey = None

    def asset_name_to_str(v):
        return bytes([(v >> (8 * i)) & 0xff for i in range(8)]).rstrip(b"\0").decode("ascii", "replace")

SCRATCH = "/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad"
JSON_PATH = SCRATCH + "/research/contract-layouts-a.json"
STATE_DIR = "/home/yeti/devwork/space/229"

PRIM_FMT = {"bool": "?", "char": "b", "sint8": "b", "uint8": "B", "sint16": "h", "uint16": "H", "sint32": "i", "uint32": "I",
            "sint64": "q", "uint64": "Q"}


class Probe:
    def __init__(self, version, js):
        self.v = js["versions"][version]
        self.types = self.v["types"]
        self.files = {}

    def mv(self, idx):
        if idx not in self.files:
            p = os.path.join(STATE_DIR, "contract%04d.229" % idx)
            self.files[idx] = (open(p, "rb"), os.path.getsize(p))
        return self.files[idx]

    def resolve(self, idx, path):
        """-> (offset, canonical type name)"""
        c = self.v["contracts"][str(idx)]
        tname = c["stateTypeCanonical"]
        off = 0
        for comp in path.replace("]", "").split("."):
            name, *subs = comp.split("[")
            fld = self.find_field(tname, name)
            off += fld["offset"]
            tname = fld["canonicalType"]
            for s in subs:
                if not tname.endswith("]"):
                    # QPI::Array<T, L> / SlowAnySizeArray: index its _values member
                    fv = self.find_field(tname, "_values")
                    off += fv["offset"]
                    tname = fv["canonicalType"]
                elem, count = split_array(tname)
                esize = self.size_of(elem)
                off += int(s) * esize
                tname = elem
        return off, tname

    def find_field(self, tname, name):
        """Field lookup including base-class members (ProposalWithAllVoteData<...> : ProposalData...)."""
        t = self.types[tname]
        for f in t["fields"]:
            if f["name"] == name:
                return f
        if t.get("base"):
            return self.find_field(t["base"], name)
        raise KeyError("%s has no field %s" % (tname, name))

    def size_of(self, tname):
        if tname in PRIM_FMT:
            return struct.calcsize("<" + PRIM_FMT[tname])
        if tname == "id":
            return 32
        if tname.endswith("]"):
            elem, count = split_array(tname)
            return self.size_of(elem) * count
        return self.types[tname]["size"]

    def read(self, idx, path, raw=False):
        off, tname = self.resolve(idx, path)
        f, size = self.mv(idx)
        n = self.size_of(tname)
        f.seek(off)
        data = f.read(n)
        return off, tname, (data if raw else self.decode(tname, data))

    def decode(self, tname, data):
        if tname in PRIM_FMT:
            return struct.unpack("<" + PRIM_FMT[tname], data)[0]
        if tname == "id":
            return fmt_id(data)
        if tname.endswith("]"):
            elem, count = split_array(tname)
            es = self.size_of(elem)
            return [self.decode(elem, data[i * es:(i + 1) * es]) for i in range(count)]
        t = self.types[tname]
        if t["kind"] == "enum":
            val = struct.unpack("<" + PRIM_FMT[t["underlying"]], data)[0]
            names = [e["name"] for e in t["enumerators"] if e["value"] == val]
            return "%d(%s)" % (val, names[0] if names else "?")
        if tname == "DateAndTime":
            return fmt_datetime(struct.unpack("<Q", data)[0])
        if tname == "bit":
            return data[0]
        if tname == "Asset":
            return {"issuer": fmt_id(data[:32]), "assetName": fmt_asset(struct.unpack("<Q", data[32:40])[0])}
        out = {}
        for fld in t["fields"]:
            if fld["name"] in ("_values", "_elements", "_keys", "_povs", "_nodes", "_occupationFlags", "_povOccupationFlags", "_occupiedFlags"):
                out[fld["name"]] = "<%s, %d bytes>" % (fld["canonicalType"], fld["size"])
                continue
            out[fld["name"]] = self.decode(fld["canonicalType"], data[fld["offset"]:fld["offset"] + fld["size"]])
        return out


def split_array(tname):
    k = tname.rindex("[")
    return tname[:k], int(tname[k + 1:-1])


def fmt_id(b):
    if not any(b):
        return "NULL_ID"
    if identity_from_pubkey:
        try:
            return identity_from_pubkey(b)
        except Exception:
            pass
    return b.hex()


def fmt_asset(v):
    try:
        return "%d(%s)" % (v, asset_name_to_str(v))
    except Exception:
        return str(v)


def fmt_datetime(v):
    if v == 0:
        return "0(invalid)"
    y = v >> 46
    mo = (v >> 42) & 15
    d = (v >> 37) & 31
    h = (v >> 32) & 31
    mi = (v >> 26) & 63
    s = (v >> 20) & 63
    ms = (v >> 10) & 1023
    return "%d (%04d-%02d-%02d %02d:%02d:%02d.%03d)" % (v, y, mo, d, h, mi, s, ms)


def packed_date(v):
    """QBAY / NOST(v1.303.2) packed uint32 date: ((year-24)<<26)|(month<<22)|(day<<17)|(hour<<12)|(minute<<6)|second"""
    return "%d (20%02d-%02d-%02d %02d:%02d:%02d)" % (v, (v >> 26) + 24, (v >> 22) & 15, (v >> 17) & 31, (v >> 12) & 31, (v >> 6) & 63, v & 63)


DEFAULT_PROBES = [
    (1, "_earnedAmount"), (1, "_distributedAmount"), (1, "_burnedAmount"), (1, "_assetIssuanceFee"), (1, "_transferFee"), (1, "_tradeFee"),
    (1, "_assetOrders._population"), (1, "_assetOrders._markRemovalCounter"), (1, "_entityOrders._population"),
    (1, "_assetOrders._povs[0]"), (1, "_assetOrders._elements[0]"), (1, "_entityOrders._elements[0]"),
    (1, "_issuerAndAssetName"), (1, "_tradeMessage"), (1, "_entityOrder"),
    (2, "mCurrentEventID"), (2, "mShareholdersRevenue"), (2, "mDistributedShareholdersRevenue"), (2, "mOperationRevenue"), (2, "mBurnedAmount"),
    (2, "mQUSDIdentifier"), (2, "mQTRYGOVIdentifier"), (2, "wholeSharePrice"), (2, "mQtryGov"), (2, "mOperationParams.mAntiSpamAmount"),
    (2, "mEventInfo._population"), (2, "mEventResult._population"), (2, "mPositionInfo._population"), (2, "mABOrders._population"),
    (2, "mOperationParams.discountedFeeForUsers._population"), (2, "mVoteMap._population"), (2, "mRecentActiveEvent[0]"), (2, "mRecentActiveEvent[1]"),
    (3, "earnedAmount"), (3, "distributedAmount"), (3, "burnedAmount"), (3, "bitFee"), (3, "populations"), (3, "providers[0]"), (3, "collateralTiers[0]"),
    (3, "lockedCollateralAmounts[0]"), (3, "lastUpdateTick[0]"),
    (4, "total"), (4, "current_poll_id"), (4, "new_polls_this_epoch"), (4, "poll_ids[0]"), (4, "voter_counts[0]"), (4, "polls[0].poll_type"),
    (4, "polls[0].creator"), (4, "polls[0].num_assets"), (4, "smt1InvocationFee"), (4, "pollCreationFee"), (4, "pollVoteFee"),
    (4, "distributeQuToShareholderFeePerShareholder"), (4, "shareholderProposalFee"), (4, "dfMiningSeed"),
    (4, "proposals.proposersAndVoters.currentProposalProposers[0]"), (4, "proposals.proposals[0].epoch"), (4, "proposals.proposals[0].type"),
    (6, "proposals.proposals[0].epoch"), (6, "proposals.proposals[0].type"), (6, "proposals.proposals[0].tick"), (6, "proposals.proposersAndVoters.currentProposalProposers[0]"),
    (6, "revenueDonation[0]"), (6, "revenueDonation[1]"),
    (8, "setProposalFee"), (8, "lastTransfersNextOverwriteIdx"), (8, "lastRegularPaymentsNextOverwriteIdx"), (8, "latestTransfers[0].destination"),
    (8, "latestTransfers[0].amount"), (8, "latestTransfers[0].tick"), (8, "proposals.proposals[0].epoch"), (8, "proposals.proposals[0].tick"),
    (8, "activeSubscriptions[0].destination"),
    (9, "_earlyUnlockedCnt"), (9, "_fullyUnlockedCnt"), (9, "_epochIndex[228]"), (9, "_epochIndex[229]"), (9, "_initialRoundInfo[228]"), (9, "_currentRoundInfo[228]"),
    (9, "statsInfo[228]"), (9, "locker[0]"),
    (10, "QCAP_ISSUER"), (10, "numberOfStaker"), (10, "numberOfVotingPower"), (10, "totalVotingPower"), (10, "totalStakedQcapAmount"), (10, "qcapSoldAmount"),
    (10, "quorumPercent"), (10, "numberOfGP"), (10, "numberOfQCP"), (10, "numberOfIPOP"), (10, "numberOfQEarnP"), (10, "numberOfFundP"), (10, "numberOfMKTP"),
    (10, "numberOfAlloP"), (10, "vote._population"), (10, "countOfVote._population"), (10, "userEpochActionsFlags._population"), (10, "staker[0]"),
    (10, "GP[0].proposer"), (10, "GP[0].proposedEpoch"), (10, "GP[0].result"), (10, "shareholderDividend"), (10, "QCAPHolderPermille"), (10, "reinvestingPermille"),
    (10, "burnPermille"), (10, "transferRightsFee"), (10, "revenuePerShare[0]"),
    (11, "numberOfActiveVaults"), (11, "totalRevenue"), (11, "totalDistributedToShareholders"), (11, "burnedAmount"), (11, "feeVotesAddrCount"), (11, "uniqueFeeVotesCount"),
    (11, "liveRegisteringFee"), (11, "liveReleaseFee"), (11, "liveReleaseResetFee"), (11, "liveHoldingFee"), (11, "liveDepositFee"), (11, "liveBurnFee"),
    (11, "vaults[0].vaultName"), (11, "vaults[0].numberOfOwners"), (11, "vaults[0].requiredApprovals"), (11, "vaults[0].isActive"), (11, "vaults[0].qubicBalance"),
    (11, "vaults[0].owners[0]"), (11, "vaultAssetParts[0].numberOfAssetTypes"),
    (12, "priceOfCFB"), (12, "priceOfQubic"), (12, "numberOfNFTIncoming"), (12, "earnedQubic"), (12, "earnedCFB"), (12, "collectedShareHoldersFee"), (12, "transferRightsFee"),
    (12, "numberOfCollection"), (12, "numberOfNFT"), (12, "cfbIssuer"), (12, "marketPlaceOwner"), (12, "statusOfMarketPlace"), (12, "Collections[0]"), (12, "NFTs[0].creator"),
    (12, "NFTs[0].possessor"), (12, "NFTs[0].salePrice"), (12, "NFTs[0].startTimeOfAuction"), (12, "NFTs[0].URI"),
    (13, "swapFeeRate"), (13, "investRewardsFeeRate"), (13, "shareholderFeeRate"), (13, "poolCreationFeeRate"), (13, "investRewardsId"), (13, "investRewardsEarnedFee"),
    (13, "shareholderEarnedFee"), (13, "shareholderDistributedAmount"), (13, "qxFeeRate"), (13, "burnFeeRate"), (13, "qxEarnedFee"), (13, "burnedAmount"),
    (13, "cachedIssuanceFee"), (13, "cachedTransferFee"), (13, "mLiquidities._population"), (13, "mPoolBasicStates[0]"), (13, "mLiquidities._elements[0]"),
    (14, "users._population"), (14, "voteStatus._population"), (14, "numberOfVotedProject._population"), (14, "tokens._population"), (14, "investors._population"),
    (14, "numberOfInvestedProjects._population"), (14, "teamAddress"), (14, "transferRightsFee"), (14, "epochRevenue"), (14, "totalPoolWeight"),
    (14, "numberOfRegister"), (14, "numberOfCreatedProject"), (14, "numberOfFundraising"), (14, "projects[0]"), (14, "fundaraisings[0]"),
    (15, "_participantCount"), (15, "_pot"), (15, "_lastDrawHour"), (15, "_lastWinner"), (15, "_lastWinAmount"), (15, "_owner"), (15, "_participants[0]"),
]


def main():
    args = sys.argv[1:]
    jp = JSON_PATH
    version = "v1.303.2"
    while args and args[0].startswith("--"):
        if args[0] == "--json":
            jp = args[1]
        elif args[0] == "--version":
            version = args[1]
        args = args[2:]
    js = json.load(open(jp))
    pr = Probe(version, js)
    probes = [(int(a.split(":")[0]), a.split(":", 1)[1]) for a in args] or DEFAULT_PROBES
    last = None
    for idx, path in probes:
        if idx != last:
            c = pr.v["contracts"][str(idx)]
            print("\n## %d %s (%s, %d bytes)" % (idx, c["assetName"], c["stateTypeCanonical"], c["size"]))
            last = idx
        try:
            off, tname, val = pr.read(idx, path)
        except Exception as e:  # noqa
            print("  %-60s ERROR %s" % (path, e))
            continue
        extra = ""
        if tname == "uint32" and ("Date" in path or "Time" in path) and val:
            extra = "  packed-date=" + packed_date(val)
        print("  %-60s @%-11d %-28s %s%s" % (path, off, tname, json.dumps(val) if not isinstance(val, str) else val, extra))


if __name__ == "__main__":
    main()
