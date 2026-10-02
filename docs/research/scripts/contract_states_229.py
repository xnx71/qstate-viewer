#!/usr/bin/env python3
"""
Hand-transcribed StateData definitions of several contracts as of qubic core v1.303.2 (EPOCH 229), expressed
with the layout model of qpi_layout.py. Used to validate the layout formulas against the real state file sizes
and to decode real data.

Each definition cites the source file/line in the v1.303.2 snapshot (src/contracts/...).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qpi_layout import (Struct, CArray, Array, HashMap, HashSet, Collection, BitArray, id_, bit, Asset, DateAndTime,  # noqa
                        uint8, sint8, uint16, sint16, uint32, sint32, uint64, sint64, bool_)

X_MULTIPLIER = 1                    # qpi_types.h:232
NUMBER_OF_COMPUTORS = 676           # network_messages/common_def.h:6
MAX_NUMBER_OF_CONTRACTS = 1024      # network_messages/common_def.h:5

# ----------------------------------------------------------------------------------------------------------------------
# contract_def.h:365-374
Contract0State = Struct("Contract0State", [("contractFeeReserves", CArray(sint64, MAX_NUMBER_OF_CONTRACTS))])
IPO = Struct("IPO", [("publicKeys", CArray(id_, NUMBER_OF_COMPUTORS)), ("prices", CArray(sint64, NUMBER_OF_COMPUTORS))])


# ----------------------------------------------------------------------------------------------------------------------
# QX (index 1)  src/contracts/Qx.h:9-242
def QX():
    AssetOrder = Struct("AssetOrder", [("entity", id_), ("numberOfShares", sint64)])
    EntityOrder = Struct("EntityOrder", [("issuer", id_), ("assetName", uint64), ("numberOfShares", sint64)])
    TradeMessage = Struct("TradeMessage", [("_contractIndex", uint32), ("_type", uint32), ("issuer", id_),
                                           ("assetName", uint64), ("price", sint64), ("numberOfShares", sint64),
                                           ("_terminator", sint8)])
    NRS_in = Struct("_NumberOfReservedShares_input", [("issuer", id_), ("assetName", uint64)])
    NRS_out = Struct("_NumberOfReservedShares_output", [("numberOfShares", sint64)])
    AssetOrdersOrder = Struct("AssetAskOrders_output::Order", [("entity", id_), ("price", sint64), ("numberOfShares", sint64)])
    EntityOrdersOrder = Struct("EntityAskOrders_output::Order", [("issuer", id_), ("assetName", uint64), ("price", sint64),
                                                               ("numberOfShares", sint64)])
    return Struct("QX::StateData", [
        ("_earnedAmount", uint64), ("_distributedAmount", uint64), ("_burnedAmount", uint64),
        ("_assetIssuanceFee", uint32), ("_transferFee", uint32), ("_tradeFee", uint32),
        ("_assetOrders", Collection(AssetOrder, 2097152 * X_MULTIPLIER)),
        ("_entityOrders", Collection(EntityOrder, 2097152 * X_MULTIPLIER)),
        ("_elementIndex", sint64), ("_elementIndex2", sint64),
        ("_issuerAndAssetName", id_),
        ("_assetOrder", AssetOrder), ("_entityOrder", EntityOrder),
        ("_price", sint64), ("_fee", sint64),
        ("_assetAskOrder", AssetOrdersOrder), ("_assetBidOrder", AssetOrdersOrder),
        ("_entityAskOrder", EntityOrdersOrder), ("_entityBidOrder", EntityOrdersOrder),
        ("_tradeMessage", TradeMessage),
        ("_numberOfReservedShares_input", NRS_in), ("_numberOfReservedShares_output", NRS_out)])


# ----------------------------------------------------------------------------------------------------------------------
# QUOTTERY (index 2)  src/contracts/Quottery.h:2-224 (v1.303.2)
def QUOTTERY():
    MAX_EVENT = 4096 * X_MULTIPLIER
    MAX_USER = MAX_EVENT * 2048
    QtryEventInfo = Struct("QtryEventInfo", [("eid", uint64), ("openDate", DateAndTime), ("endDate", DateAndTime),
                                             ("desc", Array(id_, 4)), ("option0Desc", Array(id_, 2)), ("option1Desc", Array(id_, 2))])
    DepositInfo = Struct("DepositInfo", [("pubkey", id_), ("amount", sint64)])
    DisputeResolveInfo = Struct("DisputeResolveInfo", [("epochData", Array(uint16, 1024)), ("voteData", Array(sint8, 1024))])
    QtryOrder = Struct("QtryOrder", [("entity", id_), ("amount", sint64)])
    QtryGOV = Struct("QtryGOV", [("mShareHolderFee", uint64), ("mBurnFee", uint64), ("mOperationFee", uint64), ("mFeePerDay", sint64),
                                 ("mDepositAmountForDispute", sint64), ("mOperationId", id_)])
    proposalVoter = Struct("proposalVoter", [("publicKey", id_), ("proposed", QtryGOV), ("amountOfShares", uint64), ("proposedEpoch", uint16)])
    GovHolder = Struct("GovHolder", [("publicKey", id_), ("amount", sint64)])
    OperationParams = Struct("OperationParams", [("discountedFeeForUsers", HashMap(id_, uint64, 8192 * X_MULTIPLIER)), ("mAntiSpamAmount", sint64)])
    return Struct("QUOTTERY::StateData", [
        ("mEventInfo", HashMap(uint64, QtryEventInfo, MAX_EVENT)),
        ("mEventResult", HashMap(uint64, sint8, MAX_EVENT)),
        ("mEventResultPublishTickTime", HashMap(uint64, uint32, MAX_EVENT)),
        ("mEventFinalFlag", HashMap(uint64, bit, MAX_EVENT)),
        ("mDisputeInfo", HashMap(uint64, DepositInfo, MAX_EVENT)),
        ("mDisputeResolver", HashMap(uint64, DisputeResolveInfo, MAX_EVENT)),
        ("mGODepositInfo", HashMap(uint64, DepositInfo, MAX_EVENT)),
        ("mPositionInfo", HashMap(id_, QtryOrder, MAX_USER)),
        ("mABOrders", Collection(QtryOrder, 2097152 * X_MULTIPLIER)),
        ("mRecentActiveEvent", Array(uint64, MAX_EVENT)),
        ("mCurrentEventID", uint64),
        ("mShareholdersRevenue", uint64), ("mDistributedShareholdersRevenue", uint64),
        ("mOperationRevenue", uint64), ("mDistributedOperationRevenue", uint64), ("mBurnedAmount", uint64),
        ("mQUSDIdentifier", Asset), ("mQTRYGOVIdentifier", Asset),
        ("wholeSharePrice", sint64),
        ("mQtryGov", QtryGOV),
        ("mOperationParams", OperationParams),
        ("mGovVoters", Array(proposalVoter, 1024)),
        ("mVoteMap", HashMap(id_, sint32, 1024)),
        ("mGovArray", Array(GovHolder, 1024)),
        ("mAccumulatedSum", Array(sint64, 1024))])

# ----------------------------------------------------------------------------------------------------------------------
# NOST (index 14)  src/contracts/Nostromo.h:22-216
def NOST():
    MAX_USER = 262144
    MAX_PROJECT = 262144
    MAX_TOKEN = 262144
    MAX_INVEST = 128
    investInfo = Struct("investInfo", [("investedAmount", uint64), ("claimedAmount", uint64), ("indexOfFundraising", uint32)])
    projectInfo = Struct("projectInfo", [("creator", id_), ("tokenName", uint64), ("supplyOfToken", uint64),
                                         ("startDate", uint32), ("endDate", uint32), ("numberOfYes", uint32),
                                         ("numberOfNo", uint32), ("isCreatedFundarasing", bit)])
    fundaraisingInfo = Struct("fundaraisingInfo", [
        ("tokenPrice", uint64), ("soldAmount", uint64), ("requiredFunds", uint64), ("raisedFunds", uint64),
        ("indexOfProject", uint32), ("firstPhaseStartDate", uint32), ("firstPhaseEndDate", uint32),
        ("secondPhaseStartDate", uint32), ("secondPhaseEndDate", uint32), ("thirdPhaseStartDate", uint32),
        ("thirdPhaseEndDate", uint32), ("listingStartDate", uint32), ("cliffEndDate", uint32), ("vestingEndDate", uint32),
        ("threshold", uint8), ("TGE", uint8), ("stepOfVesting", uint8), ("isCreatedToken", bit)])
    return Struct("NOST::StateData", [
        ("users", HashMap(id_, uint8, MAX_USER)),
        ("voteStatus", HashMap(id_, Array(uint32, MAX_INVEST), MAX_USER)),
        ("numberOfVotedProject", HashMap(id_, uint32, MAX_USER)),
        ("tokens", HashSet(uint64, MAX_TOKEN)),
        ("investors", HashMap(id_, Array(investInfo, MAX_INVEST), MAX_USER)),
        ("numberOfInvestedProjects", HashMap(id_, uint32, MAX_USER)),
        ("tmpInvestedList", Array(investInfo, MAX_INVEST)),
        ("projects", Array(projectInfo, MAX_PROJECT)),
        ("fundaraisings", Array(fundaraisingInfo, MAX_PROJECT)),
        ("teamAddress", id_),
        ("transferRightsFee", sint64),
        ("epochRevenue", uint64), ("totalPoolWeight", uint64),
        ("numberOfRegister", uint32), ("numberOfCreatedProject", uint32), ("numberOfFundraising", uint32)])


# ----------------------------------------------------------------------------------------------------------------------
# QBOND (index 17)  src/contracts/QBond.h:3-78
def QBOND():
    MBondInfo = Struct("MBondInfo", [("name", uint64), ("stakersAmount", sint64), ("totalStaked", sint64)])
    Order = Struct("Order", [("owner", id_), ("epoch", sint64), ("numberOfMBonds", sint64), ("feeDebt", sint64)])
    return Struct("QBOND::StateData", [
        ("_epochMbondInfoMap", HashMap(uint16, MBondInfo, 1024)),
        ("_userTotalStakedMap", HashMap(id_, sint64, 524288)),
        ("_commissionFreeAddresses", HashSet(id_, 1024)),
        ("_qearnIncomeAmount", uint64), ("_totalEarnedAmount", uint64), ("_earnedAmountFromTrade", uint64),
        ("_distributedAmount", uint64),
        ("_adminAddress", id_), ("_devAddress", id_),
        ("_askOrders", Collection(Order, 1048576)),
        ("_bidOrders", Collection(Order, 1048576)),
        ("_cyclicMbondCounter", uint8)])


# ----------------------------------------------------------------------------------------------------------------------
# QIP (index 18)  src/contracts/QIP.h:3-108
def QIP():
    MAX_ICO = 16384
    BuyerInfo = Struct("BuyerInfo", [("toReceive", sint64), ("received", sint64), ("totalQuPayed", sint64), ("isReturned", bit)])
    IcoBuyerKey = Struct("IcoBuyerKey", [("creator", id_), ("issuer", id_), ("assetName", uint64), ("buyer", id_)])
    ICOInfo = Struct("ICOInfo", (
        [("creatorOfICO", id_), ("issuer", id_)] + [(f"address{i}", id_) for i in range(1, 11)] +
        [("assetName", uint64), ("price1", uint64), ("price2", uint64), ("price3", uint64),
         ("saleAmountForPhase1", uint64), ("saleAmountForPhase2", uint64), ("saleAmountForPhase3", uint64),
         ("remainingAmountForPhase1", uint64), ("remainingAmountForPhase2", uint64), ("remainingAmountForPhase3", uint64)] +
        [(f"percent{i}", uint32) for i in range(1, 11)] +
        [("startEpoch", uint32), ("burnRemainingTokens", bit), ("isVested", bit), ("vestingPeriod", uint32),
         ("distributedQu", uint64), ("quToDistribute", uint64), ("returnedQu", uint64)]))
    return Struct("QIP::StateData", [
        ("icos", Array(ICOInfo, MAX_ICO)),
        ("activeIcoIndexes", HashSet(uint64, MAX_ICO)),
        ("currentIcoIndex", uint32), ("transferRightsFee", uint32),
        ("developmentFundAddress", id_),
        ("buyersInfo", HashMap(IcoBuyerKey, BuyerInfo, 131072))])


# ----------------------------------------------------------------------------------------------------------------------
# QRP (index 21)  src/contracts/QReservePool.h:3-28   (NOTE: contractDescriptions uses sizeof(IPO) for this contract!)
def QRP():
    return Struct("QRP::StateData", [
        ("teamAddress", id_), ("ownerAddress", id_),
        ("allowedSmartContracts", HashSet(id_, 128))])


# ----------------------------------------------------------------------------------------------------------------------
# ESCROW (index 27)  src/contracts/Escrow.h:3-171
def ESCROW():
    MAX_DEALS = 262144 * X_MULTIPLIER
    MAX_ASSETS_IN_DEAL = 4
    MAX_RESERVED_ASSETS = MAX_DEALS * MAX_ASSETS_IN_DEAL
    AssetWithAmount = Struct("AssetWithAmount", [("issuer", id_), ("name", uint64), ("amount", uint64)])
    Deal = Struct("Deal", [("index", sint64), ("acceptorId", id_), ("offeredQU", uint64), ("offeredAssetsNumber", uint64),
                           ("offeredAssets", Array(AssetWithAmount, MAX_ASSETS_IN_DEAL)),
                           ("requestedQU", uint64), ("requestedAssetsNumber", uint64),
                           ("requestedAssets", Array(AssetWithAmount, MAX_ASSETS_IN_DEAL)),
                           ("creationEpoch", uint16)])
    EscrowAsset = Struct("EscrowAsset", [("issuer", id_), ("assetName", uint64)])
    NRS_in = Struct("_NumberOfReservedShares_input", [("owner", id_), ("issuer", id_), ("assetName", uint64)])
    NRS_out = Struct("_NumberOfReservedShares_output", [("amount", sint64)])
    return Struct("ESCROW::StateData", [
        ("_earnedAmount", uint64), ("_distributedAmount", uint64),
        ("_earnedTokens", HashSet(EscrowAsset, MAX_RESERVED_ASSETS)),
        ("_currentDealIndex", sint64),
        ("_deals", HashMap(sint64, Deal, MAX_DEALS)),
        ("_acceptorDealIndexes", Collection(sint64, MAX_DEALS)),
        ("_ownerDealIndexes", Collection(sint64, MAX_DEALS)),
        ("_dealIndexOwnerMap", HashMap(sint64, id_, MAX_DEALS)),
        ("_ownersSet", HashSet(id_, MAX_DEALS)),
        ("_reservedAssets", Collection(AssetWithAmount, MAX_RESERVED_ASSETS)),
        ("_devAddress", id_),
        ("_numberOfReservedShares_input", NRS_in), ("_numberOfReservedShares_output", NRS_out)])


# ----------------------------------------------------------------------------------------------------------------------
# GGWP / WOLFPACK (index 28)  src/contracts/GGWP.h:26-182
def WOLFPACK():
    MAX_HOLDERS = 16384
    MAX_SHAREHOLDERS = 1024
    MAX_CLAN_MEMBERS = 8192
    MAX_GOV_PROPOSALS = 8
    GovProposal = Struct("WolfpackGovProposal", [("proposedAddress", id_), ("proposalId", uint64), ("proposalEpoch", uint64),
                                                 ("status", uint8), ("targetType", uint8)])
    return Struct("WOLFPACK::StateData", [
        ("adminAddress", id_),
        ("wpToken", Asset),
        ("holderBalances", HashMap(id_, uint64, MAX_HOLDERS)),
        ("totalTokensSnapshot", uint64), ("holderCount", uint64),
        ("clanRanks", HashMap(id_, uint64, MAX_CLAN_MEMBERS)),
        ("clanMemberCount", uint64), ("clanWeightedTotal", uint64),
        ("pendingRevenue", uint64), ("reinvestmentFund", uint64), ("execReserveFund", uint64),
        ("totalDistributed", uint64), ("totalDeposited", uint64), ("lastDistributionEpoch", uint64), ("lastPayoutTick", uint64),
        ("excludeAddress1", id_), ("excludeAddress2", id_), ("reinvestAddress", id_),
        ("govProposals", Array(GovProposal, MAX_GOV_PROPOSALS)),
        ("govNextProposalId", uint64),
        ("govVoteMap", HashMap(id_, uint64, MAX_SHAREHOLDERS)),
        ("stakedBalances", HashMap(id_, uint64, MAX_HOLDERS)),
        ("totalStaked", uint64), ("stakerCount", uint64),
        ("unstakeAmounts", HashMap(id_, uint64, MAX_HOLDERS)),
        ("unstakeEpochs", HashMap(id_, uint64, MAX_HOLDERS)),
        ("unstakeCount", uint64),
        ("stakingRewardPool", uint64), ("totalStakingRewardsDistributed", uint64),
        ("pendingStakingRewards", HashMap(id_, uint64, MAX_HOLDERS))])


# (contract index, asset name, model builder, uses-sizeof(IPO)-as-stateSize)
CONTRACTS = [
    (0, "", lambda: Contract0State, False),
    (1, "QX", QX, False),
    (2, "QTRY", QUOTTERY, False),
    (5, "MLM", lambda: IPO, False),
    (7, "SWATCH", lambda: IPO, False),
    (14, "NOST", NOST, False),
    (17, "QBOND", QBOND, False),
    (18, "QIP", QIP, False),
    (21, "QRP", QRP, True),
    (27, "ESCROW", ESCROW, False),
    (28, "GGWP", WOLFPACK, False),
]

STATE_DIR = "/home/yeti/devwork/space/229"
EPOCH = 229


def state_path(index):
    return os.path.join(STATE_DIR, "contract%04d.%03d" % (index, EPOCH))


if __name__ == "__main__":
    bad = 0
    print(f"{'idx':>3} {'name':8} {'model sizeof':>14} {'file size':>14}  result")
    for idx, name, builder, ipo_sized in CONTRACTS:
        t = builder()
        fsize = os.path.getsize(state_path(idx))
        if ipo_sized:
            ok = fsize == IPO.size and t.size <= fsize
            note = f"file == sizeof(IPO) = {IPO.size}; StateData ({t.size} bytes) occupies the file prefix"
        else:
            ok = fsize == t.size
            note = "sizeof(StateData) == file size" if ok else "MISMATCH"
        bad += not ok
        print(f"{idx:>3} {name:8} {t.size:>14} {fsize:>14}  {'PASS' if ok else 'FAIL'}  {note}")
    sys.exit(1 if bad else 0)
