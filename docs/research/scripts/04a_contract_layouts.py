#!/usr/bin/env python3
"""
04a_contract_layouts.py -- reference layout calculator for the Qubic contract states 1..15
(QX, QUOTTERY, RANDOM, QUTIL, MLM, GQMPROP, SWATCH, CCF, QEARN, QVAULT, MSVAULT, QBAY, QSWAP, NOST, QDRAW)
in two core versions (v1.303.2 / epoch 229 and HEAD = v1.306.0 / epoch 233).

Python 3 stdlib only.  The script is DATA-DRIVEN:

  * DEFS (below) is a hand-transcribed description of every type that is reachable from the state structs, written
    in a tiny DSL whose member lines are the *verbatim C++ declaration statements* (minus the ';').
  * A generic engine (constant-expression evaluator, type-expression parser, template instantiation incl. partial /
    explicit specialisation, x86-64 layout rules) turns the DSL into sizes and offsets.
  * verify_sources() re-reads the real headers of both checkouts and proves that every DSL struct lists exactly the
    data members (same order, same token sequence) of the corresponding struct body in the header, and that every
    constant has the same initialiser.  It also records file:line for everything.
  * The computed sizeof(StateData) is compared with the size of /home/yeti/devwork/space/229/contractNNNN.229.
  * --emit-oracle writes a C++ translation unit that includes the REAL headers and prints sizeof/alignof/offsetof
    for every type and member known to this script; --check-oracle compares the g++ output with the Python results.

Layout rules implemented (x86-64, identical for MSVC x64 and the Itanium ABI for the constructs that occur here):
  * primitive: align == size (1, 2, 4, 8); bool/char 1; m256i (= QPI::id) size 32, ALIGN 8 (union of integer arrays)
  * array T[N]: size N*sizeof(T), align alignof(T)
  * struct: members in declaration order, each at the next multiple of its alignment; align = max member align;
            size = end of last member rounded up to the struct alignment; empty struct: size 1, align 1
  * union: all members at offset 0; size = max member size rounded up to max member align
  * base class: laid out first (offset 0) with its full sizeof (no tail-padding reuse -- none of the bases that
            occur here has tail padding, so MSVC and Itanium agree; the script asserts this)
  * enum with explicit underlying type: layout of the underlying type
  * static / constexpr members, typedefs, member functions, access specifiers: no storage
"""
import argparse
import json
import os
import re
import sys

SCRATCH = "/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad"
ROOTS = {
    "v1.303.2": SCRATCH + "/core-v1.303.2",
    "HEAD": "/home/yeti/devwork/space/core",
}
VERSION_INFO = {
    "v1.303.2": {"label": "v1.303.2", "epoch": 229},
    "HEAD": {"label": "v1.306.0", "epoch": 233},
}
STATE_DIR = "/home/yeti/devwork/space/229"
STATE_EPOCH = 229
JSON_OUT = SCRATCH + "/research/contract-layouts-a.json"

# ----------------------------------------------------------------------------------------------------------------------
# Primitive types.  name -> (canonical name, size, align).
#   QPI integer typedefs: src/qpi/qpi_types.h:11-18;  <cstdint> names: lib/platform_common/qstdint.h
#   m256i: src/platform/m256.h:9 (union of int8/16/32/64 arrays + anonymous structs, static_assert sizeof == 32);
#          widest scalar member is uint64 -> alignment 8 (NOT 32: there is no __m256i member).
# ----------------------------------------------------------------------------------------------------------------------
PRIMS = {
    "bool": ("bool", 1, 1),
    "char": ("char", 1, 1),
    "signed char": ("sint8", 1, 1), "sint8": ("sint8", 1, 1), "int8_t": ("sint8", 1, 1),
    "unsigned char": ("uint8", 1, 1), "uint8": ("uint8", 1, 1), "uint8_t": ("uint8", 1, 1),
    "short": ("sint16", 2, 2), "signed short": ("sint16", 2, 2), "sint16": ("sint16", 2, 2), "int16_t": ("sint16", 2, 2),
    "unsigned short": ("uint16", 2, 2), "uint16": ("uint16", 2, 2), "uint16_t": ("uint16", 2, 2),
    "int": ("sint32", 4, 4), "signed int": ("sint32", 4, 4), "sint32": ("sint32", 4, 4), "int32_t": ("sint32", 4, 4),
    "unsigned int": ("uint32", 4, 4), "unsigned": ("uint32", 4, 4), "uint32": ("uint32", 4, 4), "uint32_t": ("uint32", 4, 4),
    "long long": ("sint64", 8, 8), "signed long long": ("sint64", 8, 8), "sint64": ("sint64", 8, 8), "int64_t": ("sint64", 8, 8),
    "unsigned long long": ("uint64", 8, 8), "uint64": ("uint64", 8, 8), "uint64_t": ("uint64", 8, 8),
    "m256i": ("id", 32, 8), "id": ("id", 32, 8),
}
PRIM_WORDS = {"unsigned", "signed", "char", "short", "int", "long", "bool"}

# ----------------------------------------------------------------------------------------------------------------------
# DSL data.  Syntax:
#   header <path relative to core root>
#   const NAME = <expr>                       constexpr at file / namespace scope (verified against the header)
#   define NAME = <expr>                      #define (verified)
#   typedef SCOPE::NAME = <type>              typedef / using alias in SCOPE (verified; "@macro <text>": produced by
#                                             a macro invocation with that text inside SCOPE's body)
#   enum SCOPE::NAME : <underlying>           (enumerators are read from the header)
#   [template <params>] struct|class|union QNAME[<spec args>] [: public BASE]
#     <verbatim C++ member declaration statement without ';'>
#     static NAME = <expr>                    static constexpr data member (no storage; usable in extents)
#     typedef <type> NAME                     member typedef
#     struct|union NAME {...} declarators     member whose type is defined inline (the type itself is given as a
#                                             separate "struct OUTER::NAME" entry)
# Every block is tagged with the versions it applies to.
# ----------------------------------------------------------------------------------------------------------------------
DEFS = []


def defs(versions, text):
    DEFS.append((versions, text))


BOTH = ("v1.303.2", "HEAD")

defs(BOTH, r"""
header src/network_messages/common_def.h
define MAX_NUMBER_OF_CONTRACTS = 1024

header src/qpi/qpi_types.h
const X_MULTIPLIER = 1ULL
define NUMBER_OF_COMPUTORS = 676
typedef QPI::uint128 = uint128_t
typedef QPI::id = m256i
struct QPI::bit
  char charValue
struct QPI::Entity
  id publicKey
  sint64 incomingAmount, outgoingAmount
  uint32 numberOfIncomingTransfers, numberOfOutgoingTransfers
  uint32 latestIncomingTransferTick, latestOutgoingTransferTick
struct QPI::Asset
  id issuer
  uint64 assetName
struct QPI::NoData
struct QPI::ContractBase
struct QPI::ContractBase::StateData

header src/platform/uint128.h
class uint128_t
  uint64_t low
  uint64_t high

header src/qpi/qpi_date_time.h
struct QPI::DateAndTime
  uint64 value

header src/qpi/qpi_containers.h
template <uint64 L> struct QPI::BitArray
  static _bits = L
  static _elements = ((L + 63) / 64)
  uint64 _values[_elements]
typedef QPI::bit_2 = BitArray<2>
typedef QPI::bit_4 = BitArray<4>
typedef QPI::bit_8 = BitArray<8>
typedef QPI::bit_16 = BitArray<16>
typedef QPI::bit_32 = BitArray<32>
typedef QPI::bit_64 = BitArray<64>
typedef QPI::bit_128 = BitArray<128>
typedef QPI::bit_256 = BitArray<256>
typedef QPI::bit_512 = BitArray<512>
typedef QPI::bit_1024 = BitArray<1024>
typedef QPI::bit_2048 = BitArray<2048>
typedef QPI::bit_4096 = BitArray<4096>
template <typename T, uint64 L> struct QPI::Array
  T _values[L]
typedef QPI::sint8_2 = Array<sint8, 2>
typedef QPI::sint8_4 = Array<sint8, 4>
typedef QPI::sint8_8 = Array<sint8, 8>
typedef QPI::uint8_2 = Array<uint8, 2>
typedef QPI::uint8_4 = Array<uint8, 4>
typedef QPI::uint8_8 = Array<uint8, 8>
typedef QPI::sint16_2 = Array<sint16, 2>
typedef QPI::sint16_4 = Array<sint16, 4>
typedef QPI::sint16_8 = Array<sint16, 8>
typedef QPI::uint16_2 = Array<uint16, 2>
typedef QPI::uint16_4 = Array<uint16, 4>
typedef QPI::uint16_8 = Array<uint16, 8>
typedef QPI::sint32_2 = Array<sint32, 2>
typedef QPI::sint32_4 = Array<sint32, 4>
typedef QPI::sint32_8 = Array<sint32, 8>
typedef QPI::uint32_2 = Array<uint32, 2>
typedef QPI::uint32_4 = Array<uint32, 4>
typedef QPI::uint32_8 = Array<uint32, 8>
typedef QPI::sint64_2 = Array<sint64, 2>
typedef QPI::sint64_4 = Array<sint64, 4>
typedef QPI::sint64_8 = Array<sint64, 8>
typedef QPI::uint64_2 = Array<uint64, 2>
typedef QPI::uint64_4 = Array<uint64, 4>
typedef QPI::uint64_8 = Array<uint64, 8>
typedef QPI::id_2 = Array<id, 2>
typedef QPI::id_4 = Array<id, 8>
typedef QPI::id_8 = Array<id, 8>
template <typename T, uint64 L> struct QPI::SlowAnySizeArray
  T _values[L]
template <typename KeyT, typename ValueT, uint64 L, typename HashFunc = HashFunction<KeyT>> class QPI::HashMap
  struct Element {...} _elements[L]
  uint64 _occupationFlags[(L * 2 + 63) / 64]
  uint64 _population
  uint64 _markRemovalCounter
struct QPI::HashMap::Element
  KeyT key
  ValueT value
template <typename KeyT, uint64 L, typename HashFunc = HashFunction<KeyT>> class QPI::HashSet
  KeyT _keys[L]
  uint64 _occupationFlags[(L * 2 + 63) / 64]
  uint64 _population
  uint64 _markRemovalCounter
template <typename T, uint64 L> struct QPI::Collection
  struct PoV {...} _povs[L]
  uint64 _povOccupationFlags[(L * 2 + 63) / 64]
  struct Element {...} _elements[L]
  uint64 _population
  uint64 _markRemovalCounter
struct QPI::Collection::PoV
  id value
  uint64 population
  sint64 headIndex, tailIndex
  sint64 bstRootIndex
struct QPI::Collection::Element
  T value
  sint64 priority
  sint64 povIndex
  sint64 bstParentIndex
  sint64 bstLeftIndex
  sint64 bstRightIndex
template <typename T, uint64 L> class QPI::LinkedList
  struct Node {...} _nodes[L]
  uint64 _occupiedFlags[(L + 63) / 64]
  sint64 _headIndex
  sint64 _tailIndex
  sint64 _freeHeadIndex
  uint64 _nextUnusedIndex
  uint64 _population
struct QPI::LinkedList::Node
  T value
  sint64 nextIndex
  sint64 prevIndex

header src/qpi/qpi_proposals.h
template <bool SupportScalarVotes> struct QPI::ProposalDataV1
  Array<uint8, 256> url
  uint16 epoch
  uint16 type
  uint32 tick
  union Data {...} data
  static supportScalarVotes = SupportScalarVotes
union QPI::ProposalDataV1::Data
  struct Transfer {...} transfer
  struct TransferInEpoch {...} transferInEpoch
  struct VariableOptions {...} variableOptions
  struct VariableScalar {...} variableScalar
struct QPI::ProposalDataV1::Data::Transfer
  id destination
  Array<sint64, 4> amounts
struct QPI::ProposalDataV1::Data::TransferInEpoch
  id destination
  sint64 amount
  uint16 targetEpoch
struct QPI::ProposalDataV1::Data::VariableOptions
  uint64 variable
  Array<sint64, 4> values
struct QPI::ProposalDataV1::Data::VariableScalar
  uint64 variable
  sint64 minValue
  sint64 maxValue
  sint64 proposedValue
struct QPI::ProposalDataYesNo
  Array<uint8, 256> url
  uint16 epoch
  uint16 type
  uint32 tick
  union Data {...} data
  static supportScalarVotes = false
union QPI::ProposalDataYesNo::Data
  struct Transfer {...} transfer
  struct VariableOptions {...} variableOptions
struct QPI::ProposalDataYesNo::Data::Transfer
  id destination
  sint64 amount
struct QPI::ProposalDataYesNo::Data::VariableOptions
  uint64 variable
  sint64 value
template <typename ProposerAndVoterHandlingT, typename ProposalDataT> class QPI::ProposalVoting
  static maxProposals = ProposerAndVoterHandlingT::maxProposals
  static maxVotes = ProposerAndVoterHandlingT::maxVotes
  typedef ProposerAndVoterHandlingT ProposerAndVoterHandlingType
  typedef ProposalDataT ProposalDataType
  typedef ProposalWithAllVoteData<ProposalDataT, maxVotes> ProposalAndVotesDataType
  ProposerAndVoterHandlingType proposersAndVoters
  ProposalAndVotesDataType proposals[maxProposals]

header src/qpi/impl/qpi_proposals_impl.h
template <uint16 proposalSlotCount> struct QPI::ProposalAndVotingByComputors
  static maxProposals = proposalSlotCount
  static maxVotes = NUMBER_OF_COMPUTORS
  id currentProposalProposers[maxProposals]
template <uint16 proposalSlotCount> struct QPI::ProposalByAnyoneVotingByComputors : public ProposalAndVotingByComputors<proposalSlotCount>
template <uint16 proposalSlotCount, uint64 contractAssetName> struct QPI::ProposalAndVotingByShareholders
  static maxProposals = proposalSlotCount
  static maxVotes = NUMBER_OF_COMPUTORS
  id currentProposalProposers[maxProposals]
  id currentProposalShareholders[maxProposals][NUMBER_OF_COMPUTORS]
template <bool scalarVotesSupported> struct QPI::__VoteStorageTypeSelector
  typedef uint8 type
template <> struct QPI::__VoteStorageTypeSelector<true>
  typedef sint64 type
template <typename ProposalDataType, uint32 numOfVotes> struct QPI::ProposalWithAllVoteData : public ProposalDataType
  static supportScalarVotes = ProposalDataType::supportScalarVotes
  typedef __VoteStorageTypeSelector<supportScalarVotes>::type VoteStorageType
  VoteStorageType votes[numOfVotes]
template <uint32 numOfVotes> struct QPI::ProposalWithAllVoteData<ProposalDataYesNo, numOfVotes> : public ProposalDataYesNo
  uint8 votes[(2 * numOfVotes + 7) / 8]

header src/contract_core/contract_def.h
struct Contract0State
  long long contractFeeReserves[MAX_NUMBER_OF_CONTRACTS]
struct IPO
  m256i publicKeys[NUMBER_OF_COMPUTORS]
  long long prices[NUMBER_OF_COMPUTORS]
""")

# ---- 1 QX ------------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/Qx.h
struct QX::AssetOrder
  id entity
  sint64 numberOfShares
struct QX::EntityOrder
  id issuer
  uint64 assetName
  sint64 numberOfShares
struct QX::TradeMessage
  unsigned int _contractIndex
  unsigned int _type
  id issuer
  uint64 assetName
  sint64 price
  sint64 numberOfShares
  sint8 _terminator
struct QX::_NumberOfReservedShares_input
  id issuer
  uint64 assetName
struct QX::_NumberOfReservedShares_output
  sint64 numberOfShares
struct QX::AssetAskOrders_output::Order
  id entity
  sint64 price
  sint64 numberOfShares
struct QX::AssetBidOrders_output::Order
  id entity
  sint64 price
  sint64 numberOfShares
struct QX::EntityAskOrders_output::Order
  id issuer
  uint64 assetName
  sint64 price
  sint64 numberOfShares
struct QX::EntityBidOrders_output::Order
  id issuer
  uint64 assetName
  sint64 price
  sint64 numberOfShares
struct QX::StateData
  uint64 _earnedAmount
  uint64 _distributedAmount
  uint64 _burnedAmount
  uint32 _assetIssuanceFee
  uint32 _transferFee
  uint32 _tradeFee
  Collection<AssetOrder, 2097152 * X_MULTIPLIER> _assetOrders
  Collection<EntityOrder, 2097152 * X_MULTIPLIER> _entityOrders
  sint64 _elementIndex, _elementIndex2
  id _issuerAndAssetName
  AssetOrder _assetOrder
  EntityOrder _entityOrder
  sint64 _price
  sint64 _fee
  AssetAskOrders_output::Order _assetAskOrder
  AssetBidOrders_output::Order _assetBidOrder
  EntityAskOrders_output::Order _entityAskOrder
  EntityBidOrders_output::Order _entityBidOrder
  TradeMessage _tradeMessage
  _NumberOfReservedShares_input _numberOfReservedShares_input
  _NumberOfReservedShares_output _numberOfReservedShares_output
""")

# ---- 2 QUOTTERY ------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/Quottery.h
const QUOTTERY_INITIAL_MAX_EVENT = 4096
const QUOTTERY_MAX_CONCURRENT_EVENT = QUOTTERY_INITIAL_MAX_EVENT * X_MULTIPLIER
const QUOTTERY_MAX_NUMBER_OF_USER = QUOTTERY_MAX_CONCURRENT_EVENT * 2048
struct QUOTTERY::QtryEventInfo
  uint64 eid
  DateAndTime openDate
  DateAndTime endDate
  Array<id, 4> desc
  Array<id, 2> option0Desc
  Array<id, 2> option1Desc
struct QUOTTERY::DepositInfo
  id pubkey
  sint64 amount
struct QUOTTERY::DisputeResolveInfo
  Array<uint16, 1024> epochData
  Array<sint8, 1024> voteData
struct QUOTTERY::QtryOrder
  id entity
  sint64 amount
struct QUOTTERY::QtryGOV
  uint64 mShareHolderFee
  uint64 mBurnFee
  uint64 mOperationFee
  sint64 mFeePerDay
  sint64 mDepositAmountForDispute
  id mOperationId
struct QUOTTERY::proposalVoter
  id publicKey
  QtryGOV proposed
  uint64 amountOfShares
  uint16 proposedEpoch
struct QUOTTERY::GovHolder
  id publicKey
  sint64 amount
struct QUOTTERY::StateData::OperationParams
  HashMap<id, uint64, 8192 * X_MULTIPLIER> discountedFeeForUsers
  sint64 mAntiSpamAmount
struct QUOTTERY::StateData
  HashMap<uint64, QtryEventInfo, QUOTTERY_MAX_CONCURRENT_EVENT> mEventInfo
  HashMap<uint64, sint8, QUOTTERY_MAX_CONCURRENT_EVENT> mEventResult
  HashMap<uint64, uint32, QUOTTERY_MAX_CONCURRENT_EVENT> mEventResultPublishTickTime
  HashMap<uint64, bit, QUOTTERY_MAX_CONCURRENT_EVENT> mEventFinalFlag
  HashMap<uint64, DepositInfo, QUOTTERY_MAX_CONCURRENT_EVENT> mDisputeInfo
  HashMap<uint64, DisputeResolveInfo, QUOTTERY_MAX_CONCURRENT_EVENT> mDisputeResolver
  HashMap<uint64, DepositInfo, QUOTTERY_MAX_CONCURRENT_EVENT> mGODepositInfo
  HashMap<id, QtryOrder, QUOTTERY_MAX_NUMBER_OF_USER> mPositionInfo
  Collection<QtryOrder, 2097152 * X_MULTIPLIER> mABOrders
  Array<uint64, QUOTTERY_MAX_CONCURRENT_EVENT> mRecentActiveEvent
  uint64 mCurrentEventID
  uint64 mShareholdersRevenue
  uint64 mDistributedShareholdersRevenue
  uint64 mOperationRevenue
  uint64 mDistributedOperationRevenue
  uint64 mBurnedAmount
  Asset mQUSDIdentifier
  Asset mQTRYGOVIdentifier
  sint64 wholeSharePrice
  QtryGOV mQtryGov
  struct OperationParams {...} mOperationParams
  Array< proposalVoter, 1024> mGovVoters
  HashMap<id, sint32, 1024> mVoteMap
  Array<GovHolder, 1024> mGovArray
  Array<sint64, 1024> mAccumulatedSum
""")

# ---- 3 RANDOM --------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/Random.h
const RANDOM_STREAM_CAPACITY = 1365
const RANDOM_MAX_PROVIDERS = 4096
struct RANDOM::StateData
  uint64 earnedAmount
  uint64 distributedAmount
  uint64 burnedAmount
  uint32 bitFee
  Array<uint32, 4> populations
  Array<id, RANDOM_MAX_PROVIDERS> providers
  Array<uint64, RANDOM_MAX_PROVIDERS> collateralTiers
  Array<id, RANDOM_MAX_PROVIDERS> commits
  Array<bit_4096, RANDOM_MAX_PROVIDERS> reveals
  bit_4096 revealOrCommitFlags
  Array<bit_4096, 32> entropy
  Array<uint64, RANDOM_MAX_PROVIDERS> lockedCollateralAmounts
  bit_4096 revealedThisTickFlags
  bit_4096 contributedToEntropyFlags
  Array<uint32, RANDOM_MAX_PROVIDERS> lastUpdateTick
""")

# ---- 4 QUTIL ---------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/QUtil.h
const QUTIL_CONTRACT_ASSET_NAME = 327647778129
const QUTIL_MAX_POLL = 64
const QUTIL_MAX_VOTERS_PER_POLL = 131072
const QUTIL_TOTAL_VOTERS = QUTIL_MAX_POLL * QUTIL_MAX_VOTERS_PER_POLL
const QUTIL_MAX_OPTIONS = 64
const QUTIL_MAX_ASSETS_PER_POLL = 16
const QUTIL_POLL_GITHUB_URL_MAX_SIZE = 256
const QUTIL_MAX_NEW_POLL = div(QUTIL_MAX_POLL, 4ULL)
typedef QUTIL::ProposalDataT = ProposalDataYesNo @macro DEFINE_SHAREHOLDER_PROPOSAL_TYPES(8, QUTIL_CONTRACT_ASSET_NAME)
typedef QUTIL::ProposersAndVotersT = ProposalAndVotingByShareholders<8, QUTIL_CONTRACT_ASSET_NAME> @macro DEFINE_SHAREHOLDER_PROPOSAL_TYPES(8, QUTIL_CONTRACT_ASSET_NAME)
typedef QUTIL::ProposalVotingT = ProposalVoting<ProposersAndVotersT, ProposalDataT> @macro DEFINE_SHAREHOLDER_PROPOSAL_TYPES(8, QUTIL_CONTRACT_ASSET_NAME)
struct QUTIL::Poll
  id poll_name
  uint64 poll_type
  uint64 min_amount
  uint64 is_active
  id creator
  Array<Asset, QUTIL_MAX_ASSETS_PER_POLL> allowed_assets
  uint64 num_assets
struct QUTIL::Voter
  id address
  uint64 amount
  uint64 chosen_option
struct QUTIL::StateData
  sint32 _i0, _i1, _i2, _i3
  sint64 _i64_0, _i64_1, _i64_2, _i64_3
  uint64 _r0, _r1, _r2, _r3
  sint64 total
  Array<Poll, QUTIL_MAX_POLL> polls
  Array<Voter, QUTIL_TOTAL_VOTERS> voters
  Array<uint64, QUTIL_MAX_POLL> poll_ids
  Array<uint64, QUTIL_MAX_POLL> voter_counts
  Array<Array<uint8, QUTIL_POLL_GITHUB_URL_MAX_SIZE>, QUTIL_MAX_POLL> poll_links
  uint64 current_poll_id
  uint64 new_polls_this_epoch
  m256i dfMiningSeed
  m256i dfCurrentState
  sint64 smt1InvocationFee
  sint64 pollCreationFee
  sint64 pollVoteFee
  sint64 distributeQuToShareholderFeePerShareholder
  sint64 shareholderProposalFee
  sint64 _futureFeePlaceholder0
  sint64 _futureFeePlaceholder1
  sint64 _futureFeePlaceholder2
  sint64 _futureFeePlaceholder3
  sint64 _futureFeePlaceholder4
  sint64 _futureFeePlaceholder5
  ProposalVotingT proposals
""")

# ---- 5 MLM / 7 SWATCH: no StateData of their own (state type in contractDescriptions is IPO) ------------------------
defs(BOTH, r"""
header src/contracts/MyLastMatch.h
struct MLM
header src/contracts/SupplyWatcher.h
struct SWATCH
""")

# ---- 6 GQMPROP -------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/GeneralQuorumProposal.h
typedef GQMPROP::ProposalDataT = ProposalDataV1<false>
typedef GQMPROP::ProposersAndVotersT = ProposalAndVotingByComputors<NUMBER_OF_COMPUTORS>
typedef GQMPROP::ProposalVotingT = ProposalVoting<ProposersAndVotersT, ProposalDataT>
struct GQMPROP::RevenueDonationEntry
  id destinationPublicKey
  sint64 millionthAmount
  uint16 firstEpoch
typedef GQMPROP::RevenueDonationT = Array<RevenueDonationEntry, 128>
struct GQMPROP::StateData
  ProposalVotingT proposals
  RevenueDonationT revenueDonation
""")

# ---- 8 CCF -----------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/ComputorControlledFund.h
const CCF_MAX_SUBSCRIPTIONS = 1024
typedef CCF::ProposalDataT = ProposalDataYesNo
typedef CCF::ProposersAndVotersT = ProposalAndVotingByComputors<100>
typedef CCF::ProposalVotingT = ProposalVoting<ProposersAndVotersT, ProposalDataT>
struct CCF::LatestTransfersEntry
  id destination
  Array<uint8, 256> url
  sint64 amount
  uint32 tick
  bit success
typedef CCF::LatestTransfersT = Array<LatestTransfersEntry, 128>
struct CCF::SubscriptionProposalData
  id proposerId
  id destination
  Array<uint8, 256> url
  uint8 weeksPerPeriod
  Array<uint8, 1> _padding0
  Array<uint8, 2> _padding1
  uint32 numberOfPeriods
  uint64 amountPerPeriod
  uint32 startEpoch
struct CCF::SubscriptionData
  id destination
  Array<uint8, 256> url
  uint8 weeksPerPeriod
  Array<uint8, 1> _padding1
  Array<uint8, 2> _padding2
  uint32 numberOfPeriods
  uint64 amountPerPeriod
  uint32 startEpoch
  sint32 currentPeriod
typedef CCF::SubscriptionProposalsT = Array<SubscriptionProposalData, 128>
typedef CCF::ActiveSubscriptionsT = Array<SubscriptionData, CCF_MAX_SUBSCRIPTIONS>
struct CCF::RegularPaymentEntry
  id destination
  Array<uint8, 256> url
  sint64 amount
  uint32 tick
  sint32 periodIndex
  bit success
  Array<uint8, 1> _padding0
  Array<uint8, 2> _padding1
typedef CCF::RegularPaymentsT = Array<RegularPaymentEntry, 128>
struct CCF::StateData
  ProposalVotingT proposals
  LatestTransfersT latestTransfers
  uint8 lastTransfersNextOverwriteIdx
  uint32 setProposalFee
  RegularPaymentsT regularPayments
  SubscriptionProposalsT subscriptionProposals
  ActiveSubscriptionsT activeSubscriptions
  uint8 lastRegularPaymentsNextOverwriteIdx
""")

# ---- 9 QEARN ---------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/Qearn.h
const QEARN_MAX_LOCKS = 4194304
const QEARN_MAX_EPOCHS = 4096
const QEARN_MAX_USERS = 131072
const QEARN_INITIAL_EPOCH = 138
struct QEARN::RoundInfo
  uint64 _totalLockedAmount
  uint64 _epochBonusAmount
struct QEARN::EpochIndexInfo
  uint32 startIndex
  uint32 endIndex
struct QEARN::LockInfo
  uint64 _lockedAmount
  id ID
  uint32 _lockedEpoch
struct QEARN::HistoryInfo
  uint64 _unlockedAmount
  uint64 _rewardedAmount
  id _unlockedID
struct QEARN::StatsInfo
  uint64 burnedAmount
  uint64 boostedAmount
  uint64 rewardedAmount
struct QEARN::StateData
  Array<RoundInfo, QEARN_MAX_EPOCHS> _initialRoundInfo
  Array<RoundInfo, QEARN_MAX_EPOCHS> _currentRoundInfo
  Array<EpochIndexInfo, QEARN_MAX_EPOCHS> _epochIndex
  Array<LockInfo, QEARN_MAX_LOCKS> locker
  Array<HistoryInfo, QEARN_MAX_USERS> earlyUnlocker
  Array<HistoryInfo, QEARN_MAX_USERS> fullyUnlocker
  uint32 _earlyUnlockedCnt
  uint32 _fullyUnlockedCnt
  Array<StatsInfo, QEARN_MAX_EPOCHS> statsInfo
""")

# ---- 10 QVAULT -------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/QVAULT.h
const QVAULT_QCAP_ASSETNAME = 1346454353
const QVAULT_QVAULT_ASSETNAME = 92686824592977
const QVAULT_MAX_NUMBER_OF_PROPOSAL = 65536
const QVAULT_X_MULTIPLIER = 1048576
const QVAULT_MAX_URLS_COUNT = 256
const QVAULT_MAX_USER_VOTES = 16
enum QVAULT::StakeEpochFlags : uint8
struct QVAULT::stakingInfo
  id stakerAddress
  uint32 amount
struct QVAULT::GPInfo
  id proposer
  uint32 currentTotalVotingPower
  uint32 numberOfYes
  uint32 numberOfNo
  uint32 proposedEpoch
  uint32 currentQuorumPercent
  Array<uint8, QVAULT_MAX_URLS_COUNT> url
  uint8 result
struct QVAULT::QCPInfo
  id proposer
  uint32 currentTotalVotingPower
  uint32 numberOfYes
  uint32 numberOfNo
  uint32 proposedEpoch
  uint32 currentQuorumPercent
  uint32 newQuorumPercent
  Array<uint8, QVAULT_MAX_URLS_COUNT> url
  uint8 result
struct QVAULT::IPOPInfo
  id proposer
  uint64 totalWeight
  uint64 assignedFund
  uint32 currentTotalVotingPower
  uint32 numberOfYes
  uint32 numberOfNo
  uint32 proposedEpoch
  uint32 ipoContractIndex
  uint32 currentQuorumPercent
  Array<uint8, QVAULT_MAX_URLS_COUNT> url
  uint8 result
struct QVAULT::QEarnPInfo
  id proposer
  uint64 amountOfInvestPerEpoch
  uint64 assignedFundPerEpoch
  uint32 currentTotalVotingPower
  uint32 numberOfYes
  uint32 numberOfNo
  uint32 proposedEpoch
  uint32 currentQuorumPercent
  Array<uint8, QVAULT_MAX_URLS_COUNT> url
  uint8 numberOfEpoch
  uint8 result
struct QVAULT::FundPInfo
  id proposer
  uint64 pricePerOneQcap
  uint32 currentTotalVotingPower
  uint32 numberOfYes
  uint32 numberOfNo
  uint32 amountOfQcap
  uint32 restSaleAmount
  uint32 proposedEpoch
  uint32 currentQuorumPercent
  Array<uint8, QVAULT_MAX_URLS_COUNT> url
  uint8 result
struct QVAULT::MKTPInfo
  id proposer
  uint64 amountOfQubic
  uint64 shareName
  uint32 currentTotalVotingPower
  uint32 numberOfYes
  uint32 numberOfNo
  uint32 amountOfQcap
  uint32 currentQuorumPercent
  uint32 proposedEpoch
  uint32 shareIndex
  uint32 amountOfShare
  Array<uint8, QVAULT_MAX_URLS_COUNT> url
  uint8 result
struct QVAULT::AlloPInfo
  id proposer
  uint32 currentTotalVotingPower
  uint32 numberOfYes
  uint32 numberOfNo
  uint32 proposedEpoch
  uint32 currentQuorumPercent
  uint32 reinvested
  uint32 distributed
  uint32 burnQcap
  Array<uint8, QVAULT_MAX_URLS_COUNT> url
  uint8 result
struct QVAULT::voteStatusInfo
  uint64 priceOfIPO
  uint32 proposalId
  uint8 proposalType
  bit decision
struct QVAULT::StateData
  Array<stakingInfo, QVAULT_X_MULTIPLIER> staker
  Array<stakingInfo, QVAULT_X_MULTIPLIER> votingPower
  Array<GPInfo, QVAULT_MAX_NUMBER_OF_PROPOSAL> GP
  Array<QCPInfo, QVAULT_MAX_NUMBER_OF_PROPOSAL> QCP
  Array<IPOPInfo, QVAULT_MAX_NUMBER_OF_PROPOSAL> IPOP
  Array<QEarnPInfo, QVAULT_MAX_NUMBER_OF_PROPOSAL> QEarnP
  Array<FundPInfo, QVAULT_MAX_NUMBER_OF_PROPOSAL> FundP
  Array<MKTPInfo, QVAULT_MAX_NUMBER_OF_PROPOSAL> MKTP
  Array<AlloPInfo, QVAULT_MAX_NUMBER_OF_PROPOSAL> AlloP
  id QCAP_ISSUER
  HashMap<id, Array<voteStatusInfo, QVAULT_MAX_USER_VOTES>, QVAULT_X_MULTIPLIER> vote
  HashMap<id, uint8, QVAULT_X_MULTIPLIER> countOfVote
  uint64 proposalCreateFund
  uint64 reinvestingFund
  uint64 totalEpochRevenue
  uint64 fundForBurn
  uint64 totalHistoryRevenue
  uint64 rasiedFundByQcap
  uint64 lastRoundPriceOfQcap
  uint64 revenueByQearn
  Array<uint64, 65536> revenueInQcapPerEpoch
  Array<uint64, 65536> revenueForOneQcapPerEpoch
  Array<uint64, 65536> revenueForOneQvaultPerEpoch
  Array<uint64, 65536> revenueForReinvestPerEpoch
  Array<uint64, 1024> revenuePerShare
  Array<uint32, 65536> burntQcapAmPerEpoch
  uint32 totalVotingPower
  uint32 totalStakedQcapAmount
  uint32 qcapSoldAmount
  uint32 shareholderDividend
  uint32 QCAPHolderPermille
  uint32 reinvestingPermille
  uint32 burnPermille
  uint32 qcapBurnPermille
  uint32 totalQcapBurntAmount
  uint32 numberOfStaker
  uint32 numberOfVotingPower
  uint32 numberOfGP
  uint32 numberOfQCP
  uint32 numberOfIPOP
  uint32 numberOfQEarnP
  uint32 numberOfFundP
  uint32 numberOfMKTP
  uint32 numberOfAlloP
  uint32 transferRightsFee
  uint32 quorumPercent
  HashMap<id, uint8, 16384> userEpochActionsFlags
""")

# ---- 11 MSVAULT ------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/MsVault.h
const MSVAULT_MAX_OWNERS = 16
const MSVAULT_MAX_COOWNER = 8
const MSVAULT_INITIAL_MAX_VAULTS = 131072ULL
const MSVAULT_MAX_VAULTS = MSVAULT_INITIAL_MAX_VAULTS * X_MULTIPLIER
const MSVAULT_ASSET_NAME = 23727827095802701
const MSVAULT_MAX_ASSET_TYPES = 8
const MSVAULT_MAX_FEE_VOTES = 64
struct MSVAULT::AssetBalance
  Asset asset
  uint64 balance
struct MSVAULT::Vault
  id vaultName
  Array<id, MSVAULT_MAX_OWNERS> owners
  Array<uint64, MSVAULT_MAX_OWNERS> releaseAmounts
  Array<id, MSVAULT_MAX_OWNERS> releaseDestinations
  uint64 qubicBalance
  uint8 numberOfOwners
  uint8 requiredApprovals
  bit isActive
struct MSVAULT::VaultAssetPart
  Array<AssetBalance, MSVAULT_MAX_ASSET_TYPES> assetBalances
  uint8 numberOfAssetTypes
  Array<Asset, MSVAULT_MAX_OWNERS> releaseAssets
  Array<uint64, MSVAULT_MAX_OWNERS> releaseAssetAmounts
  Array<id, MSVAULT_MAX_OWNERS> releaseAssetDestinations
struct MSVAULT::MsVaultFeeVote
  uint64 registeringFee
  uint64 releaseFee
  uint64 releaseResetFee
  uint64 holdingFee
  uint64 depositFee
  uint64 burnFee
struct MSVAULT::StateData
  Array<Vault, MSVAULT_MAX_VAULTS> vaults
  uint64 numberOfActiveVaults
  uint64 totalRevenue
  uint64 totalDistributedToShareholders
  uint64 burnedAmount
  Array<MsVaultFeeVote, MSVAULT_MAX_FEE_VOTES> feeVotes
  Array<id, MSVAULT_MAX_FEE_VOTES> feeVotesOwner
  Array<uint64, MSVAULT_MAX_FEE_VOTES> feeVotesScore
  uint64 feeVotesAddrCount
  Array<MsVaultFeeVote, MSVAULT_MAX_FEE_VOTES> uniqueFeeVotes
  Array<uint64, MSVAULT_MAX_FEE_VOTES> uniqueFeeVotesRanking
  uint64 uniqueFeeVotesCount
  uint64 liveRegisteringFee
  uint64 liveReleaseFee
  uint64 liveReleaseResetFee
  uint64 liveHoldingFee
  uint64 liveDepositFee
  uint64 liveBurnFee
  Array<VaultAssetPart, MSVAULT_MAX_VAULTS> vaultAssetParts
""")

# ---- 12 QBAY ---------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/Qbay.h
const QBAY_MAX_NUMBER_NFT = 2097152
const QBAY_MAX_COLLECTION = 32768
const QBAY_LENGTH_OF_URI = 59
const QBAY_CFB_NAME = 4343363
struct QBAY::InfoOfCollection
  id creator
  uint64 priceForDropMint
  uint32 maxSizeHoldingPerOneId
  sint16 currentSize
  Array<uint8, 64> URI
  uint8 royalty
  uint8 typeOfCollection
struct QBAY::InfoOfNFT
  id creator
  id possessor
  id askUser
  id creatorOfAuction
  uint64 salePrice
  uint64 askMaxPrice
  uint64 currentPriceOfAuction
  uint32 startTimeOfAuction
  uint32 endTimeOfAuction
  uint32 royalty
  uint32 NFTidForExchange
  Array<uint8, 64> URI
  uint8 statusOfAuction
  bit statusOfSale
  bit statusOfAsk
  bit paymentMethodOfAsk
  bit statusOfExchange
  bit paymentMethodOfAuction
struct QBAY::StateData
  uint64 priceOfCFB
  uint64 priceOfQubic
  uint64 numberOfNFTIncoming
  uint64 earnedQubic
  uint64 earnedCFB
  uint64 collectedShareHoldersFee
  sint64 transferRightsFee
  uint32 numberOfCollection
  uint32 numberOfNFT
  id cfbIssuer
  id marketPlaceOwner
  bit statusOfMarketPlace
  Array<InfoOfCollection, QBAY_MAX_COLLECTION> Collections
  Array<InfoOfNFT, QBAY_MAX_NUMBER_NFT> NFTs
""")

# ---- 13 QSWAP --------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/Qswap.h
const QSWAP_INITIAL_MAX_POOL = 8192
const QSWAP_MAX_POOL = QSWAP_INITIAL_MAX_POOL * X_MULTIPLIER
const QSWAP_MAX_USER_PER_POOL = 256
struct QSWAP::PoolBasicState
  id poolID
  sint64 reservedQuAmount
  sint64 reservedAssetAmount
  sint64 totalLiquidity
  uint128 accFeePerLPX64
struct QSWAP::LiquidityInfo
  sint64 liquidity
  uint128 feeDebtX64
  uint64 accumulatedFee
struct QSWAP::StateData
  uint32 swapFeeRate
  uint32 investRewardsFeeRate
  uint32 shareholderFeeRate
  uint32 poolCreationFeeRate
  id investRewardsId
  uint64 investRewardsEarnedFee
  uint64 investRewardsDistributedAmount
  uint64 shareholderEarnedFee
  uint64 shareholderDistributedAmount
  Array<PoolBasicState, QSWAP_MAX_POOL> mPoolBasicStates
  Collection<LiquidityInfo, QSWAP_MAX_POOL* QSWAP_MAX_USER_PER_POOL> mLiquidities
  uint32 qxFeeRate
  uint32 burnFeeRate
  uint64 qxEarnedFee
  uint64 qxDistributedAmount
  uint64 burnEarnedFee
  uint64 burnedAmount
  uint32 cachedIssuanceFee
  uint32 cachedTransferFee
""")

# ---- 14 NOST (v1.303.2: launchpad) ----------------------------------------------------------------------------------
defs(("v1.303.2",), r"""
header src/contracts/Nostromo.h
const NOSTROMO_MAX_USER = 262144
const NOSTROMO_MAX_NUMBER_PROJECT = 262144
const NOSTROMO_MAX_NUMBER_TOKEN = 262144
const NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST = 128
struct NOST::investInfo
  uint64 investedAmount
  uint64 claimedAmount
  uint32 indexOfFundraising
struct NOST::projectInfo
  id creator
  uint64 tokenName
  uint64 supplyOfToken
  uint32 startDate
  uint32 endDate
  uint32 numberOfYes
  uint32 numberOfNo
  bit isCreatedFundarasing
struct NOST::fundaraisingInfo
  uint64 tokenPrice
  uint64 soldAmount
  uint64 requiredFunds
  uint64 raisedFunds
  uint32 indexOfProject
  uint32 firstPhaseStartDate
  uint32 firstPhaseEndDate
  uint32 secondPhaseStartDate
  uint32 secondPhaseEndDate
  uint32 thirdPhaseStartDate
  uint32 thirdPhaseEndDate
  uint32 listingStartDate
  uint32 cliffEndDate
  uint32 vestingEndDate
  uint8 threshold
  uint8 TGE
  uint8 stepOfVesting
  bit isCreatedToken
struct NOST::StateData
  HashMap<id, uint8, NOSTROMO_MAX_USER> users
  HashMap<id, Array<uint32, NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST>, NOSTROMO_MAX_USER> voteStatus
  HashMap<id, uint32, NOSTROMO_MAX_USER> numberOfVotedProject
  HashSet<uint64, NOSTROMO_MAX_NUMBER_TOKEN> tokens
  HashMap<id, Array<investInfo, NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST>, NOSTROMO_MAX_USER> investors
  HashMap<id, uint32, NOSTROMO_MAX_USER> numberOfInvestedProjects
  Array<investInfo, NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST> tmpInvestedList
  Array<projectInfo, NOSTROMO_MAX_NUMBER_PROJECT> projects
  Array<fundaraisingInfo, NOSTROMO_MAX_NUMBER_PROJECT> fundaraisings
  id teamAddress
  sint64 transferRightsFee
  uint64 epochRevenue, totalPoolWeight
  uint32 numberOfRegister, numberOfCreatedProject, numberOfFundraising
""")

# ---- 14 NOST (HEAD: auction house; old launchpad state kept as OldStateData for MIGRATE at epoch 230) ---------------
defs(("HEAD",), r"""
header src/contracts/Nostromo.h
const NOST_AUCTION_NUM = 2048
const NOST_AUCTION_HISTORY_NUM = 1024
const NOST_AUCTION_METADATA_CID_LENGTH = 64
const NOST_AUCTION_PARTICIPANT_NUM = 4096
const NOST_PENDING_PAYOUT_NUM = 8192
const NOST_INVALID_PARTICIPANT_SLOT = NOST_AUCTION_PARTICIPANT_NUM
const NOST_AUCTION_LOT_ITEM_NUM = 4
const NOST_AUCTION_ALLOWED_WALLET_NUM = 16
const NOST_AUCTION_REQUIRED_ACCESS_ASSET_NUM = 4
const NOSTROMO_MAX_USER_OLD = 262144
const NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST_OLD = 128
const NOSTROMO_MAX_NUMBER_TOKEN_OLD = 262144
const NOSTROMO_MAX_NUMBER_PROJECT_OLD = 262144
enum NOST::EProcedureId : uint8
enum NOST::EFunctionId : uint16
enum NOST::EAuctionType : uint8
enum NOST::EAuctionVisibility : uint8
enum NOST::EAuctionStatus : uint8
enum NOST::EAuctionError : uint8
struct NOST::AuctionParticipantData
  uint64 auctionIndex
  uint64 bidIndex
  uint64 escrowedAmount
  uint64 requestedQuantity
  uint64 allocatedQuantity
  uint64 bidAmount
  id participant
  DateAndTime lastBidTime
  uint8 isUsed
  uint8 isActive
  uint8 isWinningBid
struct NOST::AuctionAssetEntry
  Asset asset
  sint64 quantity
struct NOST::AuctionCore
  Array<AuctionAssetEntry, NOST_AUCTION_LOT_ITEM_NUM> auctionLotItems
  Array<uint8, NOST_AUCTION_METADATA_CID_LENGTH> metadataIpfsCid
  id seller
  id highestBidder
  DateAndTime createdAt
  DateAndTime lastBidAt
  DateAndTime sellerDecisionDeadline
  DateAndTime settledAt
  uint64 quantityForSale
  uint64 allocatedQuantity
  uint64 minimumPurchaseQuantity
  uint64 initialPrice
  uint64 salePrice
  uint64 minimumBidIncrement
  uint64 buyNowPrice
  uint64 highestBidPrice
  uint64 highestBidQuantity
  uint64 highestBidAmount
  uint64 auctionDurationSeconds
  uint64 auctionIndex
  uint64 nextBidIndex
  uint64 highestBidSlotIndex
  EAuctionType type
  EAuctionVisibility visibility
  EAuctionStatus status
struct NOST::AuctionData
  AuctionCore core
  HashSet<id, NOST_AUCTION_ALLOWED_WALLET_NUM> allowedBidderWallets
  HashMap<Asset, sint64, NOST_AUCTION_REQUIRED_ACCESS_ASSET_NUM> requiredAccessAssets
struct NOST::OldStateData::investInfo
  uint64 investedAmount
  uint64 claimedAmount
  uint32 indexOfFundraising
struct NOST::OldStateData::projectInfo
  id creator
  uint64 tokenName
  uint64 supplyOfToken
  uint32 startDate
  uint32 endDate
  uint32 numberOfYes
  uint32 numberOfNo
  bit isCreatedFundarasing
struct NOST::OldStateData::fundaraisingInfo
  uint64 tokenPrice
  uint64 soldAmount
  uint64 requiredFunds
  uint64 raisedFunds
  uint32 indexOfProject
  uint32 firstPhaseStartDate
  uint32 firstPhaseEndDate
  uint32 secondPhaseStartDate
  uint32 secondPhaseEndDate
  uint32 thirdPhaseStartDate
  uint32 thirdPhaseEndDate
  uint32 listingStartDate
  uint32 cliffEndDate
  uint32 vestingEndDate
  uint8 threshold
  uint8 TGE
  uint8 stepOfVesting
  bit isCreatedToken
struct NOST::OldStateData
  HashMap<id, uint8, NOSTROMO_MAX_USER_OLD> users
  HashMap<id, Array<uint32, NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST_OLD>, NOSTROMO_MAX_USER_OLD> voteStatus
  HashMap<id, uint32, NOSTROMO_MAX_USER_OLD> numberOfVotedProject
  HashSet<uint64, NOSTROMO_MAX_NUMBER_TOKEN_OLD> tokens
  HashMap<id, Array<investInfo, NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST_OLD>, NOSTROMO_MAX_USER_OLD> investors
  HashMap<id, uint32, NOSTROMO_MAX_USER_OLD> numberOfInvestedProjects
  Array<investInfo, NOSTROMO_MAX_NUMBER_OF_PROJECT_USER_INVEST_OLD> tmpInvestedList
  Array<projectInfo, NOSTROMO_MAX_NUMBER_PROJECT_OLD> projects
  Array<fundaraisingInfo, NOSTROMO_MAX_NUMBER_PROJECT_OLD> fundaraisings
  id teamAddress
  sint64 transferRightsFee
  uint64 epochRevenue, totalPoolWeight
  uint32 numberOfRegister, numberOfCreatedProject, numberOfFundraising
struct NOST::NostromoFeePool
  uint64 shareholderDividendTier1Amount
  uint64 shareholderDividendTier2Amount
  uint64 shareholderDividendTier3Amount
  uint64 shareholderDividendTier4Amount
  uint64 commonServiceFeeAmount
  uint64 shareholderDividendAmount
  uint64 managementAmount
  uint64 developmentAmount
  uint64 takeoverCoordinatorAmount
struct NOST::StateData
  sint64 privateAuctionFee
  sint64 publicAuctionCreationFee
  uint64 auctionCancellationFeeBasisPoints
  uint64 auctionShareholderDividendPool
  uint64 managementFeeBasisPoints
  uint64 developmentFeeBasisPoints
  uint64 takeoverCoordinatorFeeBasisPoints
  uint64 shareholderDividendBasisPoints
  uint64 shareholderFeeBasisPointsTier1
  uint64 shareholderFeeBasisPointsTier2
  uint64 shareholderFeeBasisPointsTier3
  uint64 shareholderFeeBasisPointsTier4
  DateAndTime auctionTimerPauseStartedAt
  DateAndTime auctionTimerPauseEndsAt
  uint32 maxAuctionDurationDays
  uint32 qxTransferFee
  uint8 isPostBeginEpochPauseArmed
  uint8 isAuctionTimerPaused
  uint8 routeAllFeesToDevelopment
  id management
  id development
  id takeoverCoordinator
  uint64 totalAuctionsCreated
  Array<AuctionData, NOST_AUCTION_HISTORY_NUM> closedAuctionHistory
  uint64 closedAuctionHistoryCounter
  HashMap<uint64, AuctionData, NOST_AUCTION_NUM> auctionList
  Array<AuctionParticipantData, NOST_AUCTION_PARTICIPANT_NUM> participants
  Array<AuctionParticipantData, NOST_AUCTION_PARTICIPANT_NUM> participantHistory
  uint64 participantHistoryCounter
  HashMap<id, uint64, NOST_PENDING_PAYOUT_NUM> pendingQuPayouts
  uint64 totalPendingQuPayouts
  uint64 pendingPayoutScanCursor
  uint64 totalFinalizedAuctions
  uint64 totalCancelledAuctions
  NostromoFeePool feePool
  uint64 feeReserveGuardDropBasisPoints
  uint64 feeReserveGuardWindowSeconds
  sint64 feeReserveBaseline
  DateAndTime feeReserveBaselineAt
  DateAndTime emergencyPausedAt
  uint8 isEmergencyPaused
""")

# ---- 15 QDRAW --------------------------------------------------------------------------------------------------------
defs(BOTH, r"""
header src/contracts/Qdraw.h
const QDRAW_TICKET_PRICE = 1000000LL
const QDRAW_MAX_PARTICIPANTS = 1024 * X_MULTIPLIER
struct QDRAW::StateData
  Array<id, QDRAW_MAX_PARTICIPANTS> _participants
  uint64 _participantCount
  sint64 _pot
  uint8 _lastDrawHour
  id _lastWinner
  sint64 _lastWinAmount
  id _owner
""")

# ----------------------------------------------------------------------------------------------------------------------
# Contract table (verified against src/contract_core/contract_def.h of each version by verify_contract_def()).
#   state: the type whose sizeof() is used in contractDescriptions[]
# ----------------------------------------------------------------------------------------------------------------------
CONTRACTS = [
    dict(index=1, asset="QX", struct="QX", header="src/contracts/Qx.h", epoch=66, state="QX::StateData"),
    dict(index=2, asset="QTRY", struct="QUOTTERY", header="src/contracts/Quottery.h", epoch=72, state="QUOTTERY::StateData"),
    dict(index=3, asset="RANDOM", struct="RANDOM", header="src/contracts/Random.h", epoch=88, state="RANDOM::StateData"),
    dict(index=4, asset="QUTIL", struct="QUTIL", header="src/contracts/QUtil.h", epoch=99, state="QUTIL::StateData"),
    dict(index=5, asset="MLM", struct="MLM", header="src/contracts/MyLastMatch.h", epoch=112, state="IPO"),
    dict(index=6, asset="GQMPROP", struct="GQMPROP", header="src/contracts/GeneralQuorumProposal.h", epoch=123, state="GQMPROP::StateData"),
    dict(index=7, asset="SWATCH", struct="SWATCH", header="src/contracts/SupplyWatcher.h", epoch=123, state="IPO"),
    dict(index=8, asset="CCF", struct="CCF", header="src/contracts/ComputorControlledFund.h", epoch=127, state="CCF::StateData"),
    dict(index=9, asset="QEARN", struct="QEARN", header="src/contracts/Qearn.h", epoch=137, state="QEARN::StateData"),
    dict(index=10, asset="QVAULT", struct="QVAULT", header="src/contracts/QVAULT.h", epoch=138, state="QVAULT::StateData"),
    dict(index=11, asset="MSVAULT", struct="MSVAULT", header="src/contracts/MsVault.h", epoch=149, state="MSVAULT::StateData"),
    dict(index=12, asset="QBAY", struct="QBAY", header="src/contracts/Qbay.h", epoch=154, state="QBAY::StateData"),
    dict(index=13, asset="QSWAP", struct="QSWAP", header="src/contracts/Qswap.h", epoch=171, state="QSWAP::StateData"),
    dict(index=14, asset="NOST", struct="NOST", header="src/contracts/Nostromo.h", epoch=172, state="NOST::StateData"),
    dict(index=15, asset="QDRAW", struct="QDRAW", header="src/contracts/Qdraw.h", epoch=179, state="QDRAW::StateData"),
]
# additional (non-state) root types that are laid out, cross-checked and exported as well
EXTRA_ROOTS = {
    "v1.303.2": ["Contract0State", "IPO", "QPI::Entity"],
    "HEAD": ["Contract0State", "IPO", "QPI::Entity", "NOST::OldStateData"],
}


# ======================================================================================================================
# Tokenizer / constant expressions
# ======================================================================================================================
TOKEN_RE = re.compile(r"""
    (?P<ws>\s+)
  | (?P<ph>\{\.\.\.\})
  | (?P<num>0[xX][0-9a-fA-F]+[uUlL]*|0[bB][01]+[uUlL]*|\d+[uUlL]*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>::|<<|>>|->|[-+*/%<>(),\[\]=?:&|!~^.;{}@#"'\\])
""", re.X)


def tokenize(s):
    out = []
    pos = 0
    while pos < len(s):
        m = TOKEN_RE.match(s, pos)
        if not m:
            raise ValueError("cannot tokenize %r at %r" % (s, s[pos:pos + 20]))
        pos = m.end()
        if m.lastgroup != "ws":
            out.append(m.group())
    return out


def is_ident(t):
    return bool(re.match(r"[A-Za-z_]\w*$", t))


def is_num(t):
    return bool(re.match(r"\d", t))


def num_value(t):
    t = t.rstrip("uUlL")
    if t[:2] in ("0x", "0X"):
        return int(t, 16)
    if t[:2] in ("0b", "0B"):
        return int(t[2:], 2)
    return int(t, 10)


def cdiv(a, b):
    """C++ integer division (truncation toward zero)."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


class Ctx:
    """Resolution context: C++ scope (qualified name of the enclosing definition), template environment (name -> int or Ty),
    and display names of the enclosing instantiations (definition qname -> instance name)."""

    def __init__(self, scope, env=None, disp=None):
        self.scope = scope
        self.env = env or {}
        self.disp = disp or {}


# ======================================================================================================================
# Type model
# ======================================================================================================================
class Ty:
    def __init__(self, name, kind, size, align):
        self.name = name          # canonical (instantiated) name, valid C++ when prefixed appropriately
        self.kind = kind          # prim | struct | union | array | enum
        self.size = size
        self.align = align
        self.dsize = size         # size without tail padding
        self.fields = []          # struct/union: list of Field
        self.base = None
        self.elem = None          # array
        self.count = None         # array
        self.qname = None         # definition name (struct/union/enum)
        self.tmpl = None          # template name if instantiated from a template
        self.targs = None         # template args (list of int | Ty)
        self.statics = {}         # static constexpr members (name -> int)
        self.member_types = {}    # member typedefs (name -> Ty)
        self.underlying = None    # enum
        self.enumerators = None   # enum: list of (name, value)
        self.src = None           # (file, line) of the definition in the version being processed

    def __repr__(self):
        return "<Ty %s size=%d align=%d>" % (self.name, self.size, self.align)


class Field:
    def __init__(self, name, ty, offset, decl, stmt_index, inline=False):
        self.name = name
        self.ty = ty
        self.offset = offset
        self.decl = decl            # verbatim declared type text (DSL statement type part, incl. array extents)
        self.stmt_index = stmt_index
        self.line = None
        self.inline = inline


def round_up(x, a):
    return (x + a - 1) // a * a


# ======================================================================================================================
# DSL parsing
# ======================================================================================================================
class StructDef:
    def __init__(self, kind, qname, header):
        self.kind = kind            # struct | class | union
        self.qname = qname
        self.header = header
        self.base = None            # base type expression (string)
        self.lines = []             # member lines (raw strings)
        self.tparams = None         # list of (kind, name, ptype) for templates / specialisations
        self.spec_args = None       # raw spec arg token lists for specialisations
        self.src = None             # (file, line) filled by verification
        self.member_lines = {}      # stmt index -> source line


class TemplateDef:
    def __init__(self, qname):
        self.qname = qname
        self.primary = None
        self.specs = []


class Model:
    def __init__(self, version):
        self.version = version
        self.root = ROOTS[version]
        self.consts = {}        # name -> dict(expr, header, kind, line, value)
        self.typedefs = {}      # qname -> dict(type, scope, header, macro, line)
        self.enums = {}         # qname -> dict(underlying, header, line, enumerators, scoped)
        self.structs = {}       # qname -> StructDef (non-template or nested-in-template)
        self.templates = {}     # qname -> TemplateDef
        self.cache = {}
        self.all_types = {}     # canonical name -> Ty (struct/union/enum instances, in creation order)
        self.problems = []
        self.checks = 0
        for versions, text in DEFS:
            if version in versions:
                self._parse(text)

    # ---- DSL ---------------------------------------------------------------------------------------------------------
    def _parse(self, text):
        header = None
        cur = None
        for raw in text.splitlines():
            if not raw.strip():
                continue
            if raw.startswith("  "):
                cur.lines.append(raw.strip())
                continue
            line = raw.strip()
            cur = None
            if line.startswith("header "):
                header = line[7:].strip()
            elif line.startswith("const ") or line.startswith("define "):
                kind, rest = line.split(" ", 1)
                name, expr = [x.strip() for x in rest.split("=", 1)]
                self.consts[name] = dict(expr=expr, header=header, kind=kind, line=None, value=None)
            elif line.startswith("typedef "):
                rest = line[8:]
                macro = None
                if " @macro " in rest:
                    rest, macro = rest.split(" @macro ", 1)
                qname, ty = [x.strip() for x in rest.split("=", 1)]
                scope = qname.rsplit("::", 1)[0] if "::" in qname else ""
                self.typedefs[qname] = dict(type=ty, scope=scope, header=header, macro=macro, line=None)
            elif line.startswith("enum "):
                m = re.match(r"enum\s+([\w:]+)\s*:\s*(\w+)$", line)
                self.enums[m.group(1)] = dict(underlying=m.group(2), header=header, line=None, enumerators=None, scoped=None)
            else:
                m = re.match(r"(?:template\s*<(?P<tp>.*?)>\s+)?(?P<kind>struct|class|union)\s+(?P<name>[\w:]+)"
                             r"(?:\s*<(?P<spec>.*)>)?(?:\s*:\s*public\s+(?P<base>.+))?$", line)
                if not m:
                    raise ValueError("bad DSL line: " + line)
                sd = StructDef(m.group("kind"), m.group("name"), header)
                sd.base = m.group("base")
                if m.group("tp") is not None:
                    sd.tparams = []
                    for p in split_top(tokenize(m.group("tp"))):
                        if not p:
                            continue
                        if "=" in p:
                            p = p[:p.index("=")]
                        sd.tparams.append(("type" if p[0] in ("typename", "class") else "value", p[-1], " ".join(p[:-1])))
                    if m.group("spec") is not None:
                        sd.spec_args = split_top(tokenize(m.group("spec")))
                        self.templates[sd.qname].specs.append(sd)
                    else:
                        td = self.templates.setdefault(sd.qname, TemplateDef(sd.qname))
                        td.primary = sd
                else:
                    self.structs[sd.qname] = sd
                cur = sd

    # ---- constants ---------------------------------------------------------------------------------------------------
    def const_value(self, name):
        c = self.consts[name]
        if c["value"] is None:
            c["value"] = self.eval(tokenize(c["expr"]), Ctx(""))
        return c["value"]

    def eval(self, toks, ctx):
        pos = [0]

        def peek():
            return toks[pos[0]] if pos[0] < len(toks) else None

        def take():
            t = toks[pos[0]]
            pos[0] += 1
            return t

        def primary():
            t = take()
            if t == "(":
                v = expr()
                assert take() == ")"
                return v
            if t == "-":
                return -primary()
            if t == "+":
                return primary()
            if is_num(t):
                return num_value(t)
            if t == "true":
                return 1
            if t == "false":
                return 0
            if is_ident(t):
                if peek() == "(":  # constexpr function call: QPI::div(a, b) (qpi.h:54) / QPI::mod (qpi.h:61)
                    take()
                    args = [expr()]
                    while peek() == ",":
                        take()
                        args.append(expr())
                    assert take() == ")"
                    if t == "div":
                        return cdiv(args[0], args[1]) if args[1] else 0
                    if t == "mod":
                        return (args[0] - cdiv(args[0], args[1]) * args[1]) if args[1] else 0
                    raise ValueError("unknown constexpr function " + t)
                if peek() == "::":  # Type::staticMember
                    take()
                    member = take()
                    ty = self.lookup(t, None, ctx)
                    return ty.statics[member]
                if t in ctx.env and isinstance(ctx.env[t], int):
                    return ctx.env[t]
                if t in self.consts:
                    return self.const_value(t)
                raise KeyError("unknown constant %s (scope %s)" % (t, ctx.scope))
            raise ValueError("bad token %r in constant expression %r" % (t, toks))

        def mul():
            v = primary()
            while peek() in ("*", "/", "%"):
                op = take()
                r = primary()
                if op == "*":
                    v = v * r
                elif op == "/":
                    v = cdiv(v, r)
                else:
                    v = v - cdiv(v, r) * r
            return v

        def add():
            v = mul()
            while peek() in ("+", "-"):
                op = take()
                r = mul()
                v = v + r if op == "+" else v - r
            return v

        def shift():
            v = add()
            while peek() in ("<<", ">>"):
                op = take()
                r = add()
                v = v << r if op == "<<" else v >> r
            return v

        def band():
            v = shift()
            while peek() == "&":
                take()
                v &= shift()
            return v

        def expr():
            v = band()
            while peek() == "|":
                take()
                v |= band()
            return v

        v = expr()
        if pos[0] != len(toks):
            raise ValueError("trailing tokens in constant expression %r" % (toks,))
        return v

    # ---- type expressions --------------------------------------------------------------------------------------------
    def parse_type(self, toks, i=0):
        """Parse a type expression from toks[i:].  Returns (parts, next_index); parts = [(ident, targs or None), ...]."""
        while i < len(toks) and toks[i] in ("const", "volatile", "struct", "class", "typename"):
            i += 1
        if toks[i] in PRIM_WORDS:
            words = []
            while i < len(toks) and toks[i] in PRIM_WORDS:
                words.append(toks[i])
                i += 1
            return [(" ".join(words), None)], i
        parts = []
        while True:
            ident = toks[i]
            if not is_ident(ident):
                raise ValueError("type expected at %r in %r" % (ident, toks))
            i += 1
            args = None
            if i < len(toks) and toks[i] == "<":
                depth = 0
                j = i + 1
                while True:
                    if toks[j] == "<":
                        depth += 1
                    elif toks[j] == ">":
                        if depth == 0:
                            break
                        depth -= 1
                    j += 1
                args = split_top(toks[i + 1:j])
                i = j + 1
            parts.append((ident, args))
            if i < len(toks) and toks[i] == "::":
                i += 1
                continue
            break
        return parts, i

    def resolve(self, text_or_toks, ctx):
        toks = tokenize(text_or_toks) if isinstance(text_or_toks, str) else text_or_toks
        parts, i = self.parse_type(toks)
        if i != len(toks):
            raise ValueError("trailing tokens in type %r" % (toks,))
        return self.resolve_parts(parts, ctx)

    def resolve_parts(self, parts, ctx):
        # fast path: fully qualified nested name without template args (e.g. AssetAskOrders_output::Order)
        if len(parts) > 1 and all(a is None for _, a in parts):
            joined = "::".join(p for p, _ in parts)
            for s in self.scope_chain(ctx.scope):
                q = (s + "::" + joined) if s else joined
                if q in self.structs:
                    return self.inst_struct(self.structs[q], {}, q, ctx)
        ty = None
        for k, (ident, args) in enumerate(parts):
            if k == 0:
                ty = self.lookup(ident, args, ctx)
            else:
                if ident in ty.member_types:
                    ty = ty.member_types[ident]
                else:
                    q = ty.qname + "::" + ident
                    ty = self.inst_struct(self.structs[q], {}, q, ctx)
        return ty

    @staticmethod
    def scope_chain(scope):
        chain = []
        s = scope
        while s:
            chain.append(s)
            s = s.rsplit("::", 1)[0] if "::" in s else ""
        chain.append("")
        if "QPI" not in chain:
            chain.append("QPI")   # contracts: "using namespace QPI;" + inheritance from QPI::ContractBase
        return chain

    def prim(self, name):
        canon, size, align = PRIMS[name]
        key = ("prim", canon)
        if key not in self.cache:
            self.cache[key] = Ty(canon, "prim", size, align)
        return self.cache[key]

    def lookup(self, ident, args, ctx):
        if ident in ctx.env and isinstance(ctx.env[ident], Ty):
            return ctx.env[ident]
        for s in self.scope_chain(ctx.scope):
            q = (s + "::" + ident) if s else ident
            if q in self.typedefs:
                td = self.typedefs[q]
                return self.resolve(td["type"], Ctx(td["scope"]))
            if q in self.templates:
                return self.inst_template(self.templates[q], args, ctx)
            if q in self.structs:
                # nested struct of a template instance inherits the template environment
                if s in ctx.disp:
                    return self.inst_struct(self.structs[q], ctx.env, ctx.disp[s] + "::" + ident, ctx)
                return self.inst_struct(self.structs[q], {}, q, ctx)
            if q in self.enums:
                return self.inst_enum(q)
        if ident in PRIMS:
            return self.prim(ident)
        raise KeyError("unknown type %s (scope %s)" % (ident, ctx.scope))

    def inst_enum(self, q):
        key = ("enum", q)
        if key not in self.cache:
            e = self.enums[q]
            u = self.prim(e["underlying"])
            ty = Ty(q, "enum", u.size, u.align)
            ty.qname = q
            ty.underlying = u
            self.cache[key] = ty
            self.all_types[ty.name] = ty
        return self.cache[key]

    def array(self, elem, count):
        key = ("array", elem.name, count, id(elem))
        if key not in self.cache:
            ty = Ty("%s[%d]" % (elem.name, count), "array", elem.size * count, elem.align)
            ty.elem = elem
            ty.count = count
            self.cache[key] = ty
        return self.cache[key]

    def inst_template(self, td, args, ctx):
        prim_def = td.primary
        if args is None:
            raise ValueError("template %s used without arguments" % td.qname)
        vals = []
        for (kind, pname, ptype), a in zip(prim_def.tparams, args):
            vals.append(self.resolve(a, ctx) if kind == "type" else self.eval(a, ctx))
        short = td.qname.split("::")[-1]

        def show(kind_ptype, v):
            if isinstance(v, Ty):
                return v.name
            if kind_ptype == "bool":
                return "true" if v else "false"
            return str(v)

        name = "%s<%s>" % (short, ",".join(show(p[2], v) for p, v in zip(prim_def.tparams, vals)))
        key = ("tmpl", name)
        if key in self.cache:
            return self.cache[key]
        chosen, env = prim_def, {p[1]: v for p, v in zip(prim_def.tparams, vals)}
        for spec in td.specs:
            m = self.match_spec(spec, vals, ctx)
            if m is not None:
                chosen, env = spec, m
                break
        ty = self.inst_struct(chosen, env, name, Ctx(td.qname.rsplit("::", 1)[0] if "::" in td.qname else ""), cache_key=key)
        ty.tmpl = short
        ty.targs = vals
        return ty

    def match_spec(self, spec, vals, ctx):
        env = {}
        pnames = {p[1]: p for p in spec.tparams}
        for a, v in zip(spec.spec_args, vals):
            if len(a) == 1 and a[0] in pnames:
                env[a[0]] = v
            elif isinstance(v, Ty):
                if self.resolve(a, Ctx("QPI")) is not v:
                    return None
            else:
                if self.eval(a, Ctx("QPI")) != v:
                    return None
        return env

    def inst_struct(self, sd, env, name, ctx, cache_key=None):
        if name.startswith("QPI::"):
            name = name[5:]     # display without namespace (contracts have "using namespace QPI;" at file scope)
        key = cache_key or ("struct", name)
        if key in self.cache:
            return self.cache[key]
        ty = Ty(name, "union" if sd.kind == "union" else "struct", 0, 1)
        ty.qname = sd.qname
        self.cache[key] = ty
        env = dict(env)
        disp = dict(ctx.disp)
        disp[sd.qname] = name
        c = Ctx(sd.qname, env, disp)
        off = 0
        align = 1
        if sd.base:
            base = self.resolve(sd.base, c)
            ty.base = base
            off = base.size
            align = base.align
            ty.statics.update(base.statics)
            ty.member_types.update(base.member_types)
            for k, v in base.statics.items():
                env.setdefault(k, v)
            if base.dsize != base.size:
                self.problems.append("ABI NOTE: base %s of %s has tail padding (dsize %d < size %d): Itanium may reuse it, MSVC does not"
                                     % (base.name, name, base.dsize, base.size))
        usize = 0
        for idx, line in enumerate(sd.lines):
            toks = tokenize(line)
            if toks[0] == "static":
                val = self.eval(toks[3:], c)
                env[toks[1]] = val
                ty.statics[toks[1]] = val
                continue
            if toks[0] == "typedef":
                t = self.resolve(toks[1:-1], c)
                env[toks[-1]] = t
                ty.member_types[toks[-1]] = t
                continue
            inline = False
            if toks[0] in ("struct", "union", "class") and len(toks) > 2 and toks[2] == "{...}":
                inline = True
                mty = self.lookup(toks[1], None, c)
                decl_type = line[:line.index("{...}") + 5]
                rest = toks[3:]
            else:
                parts, i = self.parse_type(toks)
                mty = self.resolve_parts(parts, c)
                rest = toks[i:]
                decl_type = self.type_text(line, rest)
            for d in split_top(rest):
                dname = d[0]
                dims = []
                j = 1
                while j < len(d):
                    assert d[j] == "["
                    k = d.index("]", j)
                    dims.append(self.eval(d[j + 1:k], c))
                    j = k + 1
                t = mty
                for dim in reversed(dims):
                    t = self.array(t, dim)
                suffix = "".join("[%s]" % x for x in self.dim_texts(d))
                if sd.kind == "union":
                    o = 0
                    usize = max(usize, t.size)
                else:
                    o = round_up(off, t.align)
                    off = o + t.size
                align = max(align, t.align)
                ty.fields.append(Field(dname, t, o, decl_type + suffix, idx, inline))
        if sd.kind == "union":
            off = usize
        ty.dsize = off
        ty.align = align
        ty.size = round_up(off, align) if off else 1   # empty class: sizeof == 1
        if off == 0:
            ty.dsize = 0
        ty.statics = dict(ty.statics)
        self.all_types[name] = ty
        return ty

    @staticmethod
    def dim_texts(d):
        out = []
        j = 1
        while j < len(d):
            k = d.index("]", j)
            out.append(" ".join(d[j + 1:k]))
            j = k + 1
        return out

    @staticmethod
    def type_text(line, rest_tokens):
        """Verbatim type part of a DSL member line = line minus the declarator list."""
        if not rest_tokens:
            return line
        # find the position of the first declarator token sequence at the end of the line
        decl = rest_tokens[0]
        # the declarator list is the tail of the line: search the last occurrence that leaves a matching token tail
        for m in reversed(list(re.finditer(r"\b%s\b" % re.escape(decl), line))):
            if tokenize(line[m.start():]) == rest_tokens:
                return line[:m.start()].strip()
        return line

    def state_type(self, name):
        return self.resolve(name, Ctx(""))


def split_top(toks):
    """Split a token list at top-level commas (outside <>, (), [])."""
    out = []
    cur = []
    depth = 0
    for t in toks:
        if t in ("<", "(", "["):
            depth += 1
        elif t in (">", ")", "]"):
            depth -= 1
        if t == "," and depth == 0:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur or out:
        out.append(cur)
    return out


# ======================================================================================================================
# Source verification: prove that the DSL equals the real headers
# ======================================================================================================================
_src_cache = {}


def load_source(path):
    """Return (clean_text, depth) -- comments, string/char literals and '#if 0' blocks blanked (newlines kept)."""
    if path in _src_cache:
        return _src_cache[path]
    raw = open(path, encoding="utf-8", errors="replace").read()
    out = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if raw.startswith("//", i):
            j = raw.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif raw.startswith("/*", i):
            j = raw.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in raw[i:j]))
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and raw[i - 1].isalnum())):
            j = i + 1
            while j < n and raw[j] != c:
                j += 2 if raw[j] == "\\" else 1
            j += 1
            out.append(c + " " * (j - i - 2) + c)
            i = j
        else:
            out.append(c)
            i += 1
    text = "".join(out)
    # blank "#if 0 ... #endif" blocks (no nesting occurs in the files handled here)
    def blank(m):
        return "".join(ch if ch == "\n" else " " for ch in m.group())
    text = re.sub(r"^[ \t]*#[ \t]*if[ \t]+0\b.*?^[ \t]*#[ \t]*endif\b[^\n]*", blank, text, flags=re.S | re.M)
    depth = [0] * (len(text) + 1)
    d = 0
    for k, ch in enumerate(text):
        if ch == "{":
            depth[k] = d
            d += 1
        elif ch == "}":
            d -= 1
            depth[k] = d
        else:
            depth[k] = d
    depth[len(text)] = d
    _src_cache[path] = (text, depth)
    return _src_cache[path]


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def match_brace(text, i):
    d = 0
    for k in range(i, len(text)):
        if text[k] == "{":
            d += 1
        elif text[k] == "}":
            d -= 1
            if d == 0:
                return k
    raise ValueError("unbalanced braces")


def find_def(text, depth, name, lo, hi, want_depth, spec=None, kinds="struct|class|union"):
    """Find 'struct NAME [: base] {' whose keyword sits at brace depth want_depth inside text[lo:hi].
    Returns (keyword_pos, body_open_pos, body_close_pos) or None."""
    if spec is None:
        pat = r"\b(?:%s)\s+%s\b(?!\s*<)\s*(?::[^;{()]*)?\{" % (kinds, re.escape(name))
    else:
        pat = r"\b(?:%s)\s+%s\s*<\s*%s\s*>\s*(?::[^;{()]*)?\{" % (kinds, re.escape(name), r"\s*".join(re.escape(t) for t in spec))
    for m in re.finditer(pat, text[lo:hi]):
        p = lo + m.start()
        if depth[p] == want_depth:
            o = lo + m.end() - 1
            return p, o, match_brace(text, o)
    return None


def locate(text, depth, qname, spec=None):
    """Locate the body of the (possibly nested) definition qname.  Namespace components (QPI) are searched as namespaces."""
    comps = qname.split("::")
    lo, hi, d = 0, len(text), 0
    pos = None
    for k, comp in enumerate(comps):
        last = k == len(comps) - 1
        if comp == "QPI" and k == 0:
            m = None
            for mm in re.finditer(r"\bnamespace\s+QPI\s*\{", text):
                o = mm.end() - 1
                c = match_brace(text, o)
                # choose the namespace block that contains the next component
                nxt = find_def(text, depth, comps[1], o + 1, c, depth[o] + 1, spec if len(comps) == 2 else None)
                if nxt:
                    m = (o, c)
                    break
            if not m:
                return None
            lo, hi, d = m[0] + 1, m[1], depth[m[0]] + 1
            continue
        r = find_def(text, depth, comp, lo, hi, d, spec if last else None)
        if not r:
            return None
        pos, o, c = r
        lo, hi, d = o + 1, c, depth[o] + 1
    return pos, lo, hi


def scan_body(text, lo, hi):
    """Split a class body text[lo:hi] into member statements.
    Returns list of (kind, tokens, pos) with kind in data|static|typedef|using|func|typedef_like|enum|typedef|nested|other."""
    stmts = []
    buf = []
    start = None
    i = lo
    while i < hi:
        c = text[i]
        if c == "{":
            head = "".join(buf).strip()
            head = re.sub(r"^(?:(?:public|protected|private)\s*:(?!:)\s*)+", "", head)
            j = match_brace(text, i)
            if re.match(r"(?:template\s*<[^{}]*>\s*)?(?:struct|class|union|enum)\b", head) and "(" not in head:
                buf.append("{...}")
                i = j + 1
                continue
            if "(" in head:
                stmts.append(("func", head, start))
                buf = []
                start = None
                i = j + 1
                continue
            buf.append("{...}")   # brace initialiser
            i = j + 1
            continue
        if c == ";":
            s = "".join(buf).strip()
            if s:
                stmts.append(("stmt", s, start))
            buf = []
            start = None
            i += 1
            continue
        if start is None and not c.isspace():
            start = i
        buf.append(c)
        i += 1
    out = []
    for kind, s, pos in stmts:
        s2 = s
        while True:
            m = re.match(r"(?:public|protected|private)\s*:(?!:)\s*", s2)
            if not m:
                break
            pos += len(s2[:m.end()]) if False else 0
            s2 = s2[m.end():]
        if s2 != s:
            # recompute start position of the real statement
            pos = pos + s.index(s2[:20]) if s2 else pos
        if not s2:
            continue
        try:
            toks = tokenize(s2)
        except ValueError:
            out.append(("other", [s2], pos))
            continue
        if kind == "func":
            out.append(("func", toks, pos))
        elif toks[0] == "typedef":
            out.append(("typedef", toks, pos))
        elif toks[0] == "using":
            out.append(("using", toks, pos))
        elif toks[0] in ("friend", "static_assert", "template"):
            out.append(("other", toks, pos))
        elif toks[0] == "enum":
            out.append(("enum", toks, pos))
        elif toks[0] in ("struct", "class", "union") and "{...}" in toks:
            k = toks.index("{...}")
            out.append(("nested" if k == len(toks) - 1 else "data", toks, pos))
        elif toks[0] in ("struct", "class", "union") and len(toks) == 2:
            out.append(("other", toks, pos))    # forward declaration
        elif toks[0] in ("static", "constexpr", "inline"):
            out.append(("static", toks, pos))   # static data member (with '=') or static member function declaration
        elif "(" in toks and not _paren_only_in_brackets(toks):
            out.append(("func", toks, pos))
        else:
            out.append(("data", toks, pos))
    return out


def _paren_only_in_brackets(toks):
    """True if every '(' occurs inside an array extent [...] or template args (e.g. 'uint64 f[(L + 63) / 64]')."""
    depth = 0
    for t in toks:
        if t in ("[", "<"):
            depth += 1
        elif t in ("]", ">"):
            depth -= 1
        elif t == "(" and depth == 0:
            return False
    return True


def static_name_expr(toks):
    """('static', 'constexpr', type..., NAME, '=', expr...) -> (NAME, expr tokens)"""
    k = toks.index("=")
    return toks[k - 1], toks[k + 1:]


def verify_sources(model):
    """Check every DSL entry against the header of model.version.  Fills line numbers.  Returns list of problems."""
    probs = model.problems
    root = model.root

    def src(header):
        return load_source(os.path.join(root, header))

    # constants
    for name, c in model.consts.items():
        text, depth = src(c["header"])
        if c["kind"] == "define":
            m = re.search(r"^[ \t]*#[ \t]*define[ \t]+%s[ \t]+(.+?)[ \t]*$" % re.escape(name), text, re.M)
        else:
            m = re.search(r"\bconstexpr\s+[\w \t]+?\b%s\s*=\s*([^;]+);" % re.escape(name), text)
        model.checks += 1
        if not m:
            probs.append("%s: constant %s not found in %s" % (model.version, name, c["header"]))
            continue
        if tokenize(m.group(1)) != tokenize(c["expr"]):
            probs.append("%s: constant %s is %r in %s, DSL says %r" % (model.version, name, m.group(1), c["header"], c["expr"]))
        c["line"] = line_of(text, m.start())
    # typedefs
    for qname, td in model.typedefs.items():
        text, depth = src(td["header"])
        scope, name = (qname.rsplit("::", 1) + [None])[:2] if "::" in qname else ("", qname)
        model.checks += 1
        if scope in ("", "QPI"):
            lo, hi = 0, len(text)
        else:
            r = locate(text, depth, scope)
            if not r:
                probs.append("%s: scope %s of typedef %s not found" % (model.version, scope, qname))
                continue
            lo, hi = r[1], r[2]
        if td["macro"]:
            m = re.search(r"\s*".join(re.escape(t) for t in tokenize(td["macro"])), text[lo:hi])
            if not m:
                probs.append("%s: macro invocation %r for typedef %s not found" % (model.version, td["macro"], qname))
                continue
            td["line"] = line_of(text, lo + m.start())
            continue
        found = False
        for m in re.finditer(r"\btypedef\s+([^;]+?)\s*\b%s\s*;" % re.escape(name), text[lo:hi]):
            if tokenize(m.group(1)) == tokenize(td["type"]):
                td["line"] = line_of(text, lo + m.start())
                found = True
                break
        if not found:
            for m in re.finditer(r"\busing\s+%s\s*=\s*([^;]+?)\s*;" % re.escape(name), text[lo:hi]):
                if tokenize(m.group(1)) == tokenize(td["type"]):
                    td["line"] = line_of(text, lo + m.start())
                    found = True
                    break
        if not found:
            probs.append("%s: typedef %s = %s not found in %s" % (model.version, qname, td["type"], td["header"]))
    # enums
    for qname, e in model.enums.items():
        text, depth = src(e["header"])
        scope, name = qname.rsplit("::", 1)
        r = locate(text, depth, scope)
        model.checks += 1
        m = re.search(r"\benum\s+(class\s+|struct\s+)?%s\s*:\s*(\w+)\s*\{([^}]*)\}" % re.escape(name), text[r[1]:r[2]]) if r else None
        if not m:
            probs.append("%s: enum %s not found" % (model.version, qname))
            continue
        if m.group(2) != e["underlying"]:
            probs.append("%s: enum %s underlying type is %s, DSL says %s" % (model.version, qname, m.group(2), e["underlying"]))
        e["line"] = line_of(text, r[1] + m.start())
        e["scoped"] = bool(m.group(1))
        vals = []
        nxt = 0
        for item in m.group(3).split(","):
            item = item.strip()
            if not item:
                continue
            if "=" in item:
                n, x = [t.strip() for t in item.split("=", 1)]
                nxt = model.eval(tokenize(x), Ctx(scope))
            else:
                n = item
            vals.append((n, nxt))
            nxt += 1
        e["enumerators"] = vals
    # structs
    alldefs = list(model.structs.values())
    for td in model.templates.values():
        alldefs.append(td.primary)
        alldefs.extend(td.specs)
    for sd in alldefs:
        text, depth = src(sd.header)
        model.checks += 1
        r = locate(text, depth, sd.qname, [t for a in sd.spec_args for t in (a + [","])][:-1] if sd.spec_args else None)
        if not r:
            probs.append("%s: definition of %s not found in %s" % (model.version, sd.qname, sd.header))
            continue
        pos, lo, hi = r
        sd.src = (sd.header, line_of(text, pos))
        # base class check
        head = text[pos:lo]
        mb = re.search(r":\s*(?:public\s+)?([^{]+?)\s*\{", head)
        have_base = tokenize(mb.group(1)) if mb else None
        want_base = tokenize(sd.base) if sd.base else None
        if sd.qname not in ("MLM", "SWATCH") and have_base != want_base:
            probs.append("%s: base of %s is %r, DSL says %r" % (model.version, sd.qname, have_base, want_base))
        stmts = scan_body(text, lo, hi)
        data = [(toks, p) for kind, toks, p in stmts if kind == "data"]
        want = [(i, tokenize(l)) for i, l in enumerate(sd.lines) if not l.startswith(("static ", "typedef "))]
        if [t for t, _ in data] != [t for _, t in want]:
            probs.append("%s: data members of %s differ.\n   header: %s\n   DSL:    %s" % (
                model.version, sd.qname, [" ".join(t) for t, _ in data], [" ".join(t) for _, t in want]))
        else:
            for (toks, p), (i, _) in zip(data, want):
                sd.member_lines[i] = line_of(text, p)
        for i, l in enumerate(sd.lines):
            toks = tokenize(l)
            if toks[0] == "static":
                ok = False
                for kind, st, p in stmts:
                    if kind == "static" and "=" in st and static_name_expr(st) == (toks[1], toks[3:]):
                        ok = True
                        sd.member_lines[i] = line_of(text, p)
                if not ok:
                    probs.append("%s: static member %r of %s not found" % (model.version, l, sd.qname))
            elif toks[0] == "typedef":
                ok = False
                for kind, st, p in stmts:
                    if kind == "typedef" and st == toks:
                        ok = True
                        sd.member_lines[i] = line_of(text, p)
                if not ok:
                    probs.append("%s: member typedef %r of %s not found" % (model.version, l, sd.qname))
    return probs


def verify_contract_def(model):
    """Check CONTRACTS against contract_def.h (index define, include, contractDescriptions entry)."""
    path = os.path.join(model.root, "src/contract_core/contract_def.h")
    raw = open(path).read()
    info = {}
    descs = re.findall(r'\{\s*"(\w*)"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*sizeof\(([\w:]+)\)\s*\}', raw)
    for c in CONTRACTS:
        model.checks += 1
        m = re.search(r"#define\s+%s_CONTRACT_INDEX\s+(\d+)\s*\n#define CONTRACT_INDEX %s_CONTRACT_INDEX\s*\n"
                      r"#define CONTRACT_STATE_TYPE (\w+)\s*\n#define CONTRACT_STATE2_TYPE (\w+)\s*\n#include \"([^\"]+)\"" % (c["struct"], c["struct"]), raw)
        if not m:
            model.problems.append("%s: contract %s not found in contract_def.h" % (model.version, c["struct"]))
            continue
        d = descs[c["index"]]
        line = raw[:raw.index('{"%s"' % c["asset"])].count("\n") + 1
        ok = (int(m.group(1)) == c["index"] and m.group(2) == c["struct"] and "src/" + m.group(4) == c["header"]
              and d[0] == c["asset"] and int(d[1]) == c["epoch"] and d[3] == c["state"])
        if not ok:
            model.problems.append("%s: contract table mismatch for %s: %r %r" % (model.version, c["struct"], m.groups(), d))
        info[c["index"]] = dict(defLine=raw[:m.start()].count("\n") + 1, descLine=line, destructionEpoch=int(d[2]))
    m = re.search(r"^constexpr ContractStateChangeInfo contractStateChangeInfos\[\]\s*=\s*\{(.*)\};", raw, re.M)
    changes = re.findall(r"\{\s*(\w+)_CONTRACT_INDEX\s*,\s*(\w+)\s*,\s*(\d+)\s*\}", m.group(1)) if m else []
    return info, [dict(contract=a, change=b, epoch=int(e)) for a, b, e in changes], raw[:m.start()].count("\n") + 1 if m else None


# ======================================================================================================================
# Output helpers
# ======================================================================================================================
def attach_lines(model):
    """Copy file:line information from the definitions to the instantiated types."""
    defs_by_q = dict(model.structs)
    for ty in model.all_types.values():
        if ty.kind == "enum":
            e = model.enums[ty.qname]
            ty.src = (e["header"], e["line"])
            ty.enumerators = e["enumerators"]
            continue
        sd = None
        if ty.tmpl:
            td = model.templates[[q for q in model.templates if q.split("::")[-1] == ty.tmpl][0]]
            sd = td.primary
            for spec in td.specs:
                if model.match_spec(spec, ty.targs, Ctx("QPI")) is not None:
                    sd = spec
                    break
        else:
            sd = defs_by_q.get(ty.qname)
        if sd is None:
            continue
        ty.src = sd.src
        for f in ty.fields:
            f.line = sd.member_lines.get(f.stmt_index)


def type_json(ty):
    d = dict(kind=ty.kind, size=ty.size, align=ty.align)
    if ty.src:
        d["file"] = ty.src[0]
        d["line"] = ty.src[1]
    if ty.kind == "enum":
        d["underlying"] = ty.underlying.name
        d["enumerators"] = [dict(name=n, value=v) for n, v in (ty.enumerators or [])]
        return d
    if ty.base:
        d["base"] = ty.base.name
    if ty.tmpl:
        d["template"] = ty.tmpl
        d["templateArgs"] = [a.name if isinstance(a, Ty) else a for a in ty.targs]
    if ty.statics:
        d["statics"] = ty.statics
    d["dataSize"] = ty.dsize
    d["fields"] = [field_json(f) for f in ty.fields]
    return d


def field_json(f):
    d = dict(name=f.name, type=f.decl, canonicalType=f.ty.name, offset=f.offset, size=f.ty.size, align=f.ty.align)
    if f.line:
        d["line"] = f.line
    return d


def reachable(ty, seen):
    """Collect all named (struct/union/enum) types reachable from ty, depth first, in order."""
    if ty.kind == "array":
        reachable(ty.elem, seen)
        return
    if ty.kind == "prim" or ty.name in seen:
        return
    seen[ty.name] = ty
    if ty.base:
        reachable(ty.base, seen)
    for f in ty.fields:
        reachable(f.ty, seen)


def build(version):
    model = Model(version)
    # constants first (so that problems show up early)
    for name in model.consts:
        model.const_value(name)
    results = {}
    for c in CONTRACTS:
        results[c["index"]] = model.state_type(c["state"])
    extra = {name: model.state_type(name) for name in EXTRA_ROOTS[version]}
    # instantiate every remaining non-template struct of the DSL as well (keeps the oracle cross-check complete)
    for q, sd in model.structs.items():
        if any(q.startswith(t + "::") for t in model.templates):
            continue
        model.inst_struct(sd, {}, q, Ctx(q.rsplit("::", 1)[0] if "::" in q else ""))
    verify_sources(model)
    cinfo, changes, changes_line = verify_contract_def(model)
    attach_lines(model)
    return model, results, extra, cinfo, changes, changes_line


def file_size(index):
    p = os.path.join(STATE_DIR, "contract%04d.%d" % (index, STATE_EPOCH))
    return os.path.getsize(p) if os.path.exists(p) else None


def make_json(built):
    out = {
        "generator": "research/scripts/04a_contract_layouts.py",
        "abi": {
            "target": "x86-64, little endian, natural alignment (MSVC x64 == Itanium for all constructs in these states)",
            "id": {"size": 32, "align": 8},
            "note": "offsets/sizes in bytes; 'type' is the declared type verbatim from the header; 'canonicalType' is the instantiated type name used as key in 'types'",
        },
        "stateFiles": {"dir": STATE_DIR, "epoch": STATE_EPOCH},
        "versions": {},
    }
    for version, (model, results, extra, cinfo, changes, changes_line) in built.items():
        v = dict(VERSION_INFO[version])
        v["root"] = ROOTS[version]
        v["contractStateChangeInfos"] = dict(file="src/contract_core/contract_def.h", line=changes_line, entries=changes)
        v["constants"] = {n: dict(value=c["value"], expr=c["expr"], file=c["header"], line=c["line"]) for n, c in model.consts.items()}
        v["typedefs"] = {q: dict(type=t["type"], file=t["header"], line=t["line"], **({"viaMacro": t["macro"]} if t["macro"] else {}))
                         for q, t in model.typedefs.items() if not q.startswith("QPI::") or q in ("QPI::id", "QPI::uint128", "QPI::bit_4096", "QPI::id_4")}
        v["contracts"] = {}
        for c in CONTRACTS:
            ty = results[c["index"]]
            entry = dict(index=c["index"], assetName=c["asset"], struct=c["struct"], header=c["header"],
                         constructionEpoch=c["epoch"], stateType=c["state"], stateTypeCanonical=ty.name,
                         contractDefLine=cinfo[c["index"]]["defLine"], contractDescriptionLine=cinfo[c["index"]]["descLine"],
                         size=ty.size, align=ty.align)
            if ty.src:
                entry["stateTypeFile"], entry["stateTypeLine"] = ty.src
            if version == "v1.303.2":
                fs = file_size(c["index"])
                entry["fileSize"] = fs
                entry["fileSizeMatch"] = (fs == ty.size)
            entry["fields"] = [field_json(f) for f in ty.fields]
            v["contracts"][str(c["index"])] = entry
        seen = {}
        for ty in list(results.values()) + list(extra.values()):
            reachable(ty, seen)
        v["types"] = {name: type_json(t) for name, t in seen.items()}
        out["versions"][version] = v
    return out


# ======================================================================================================================
# g++ oracle (real headers) -- emit and check
# ======================================================================================================================
ORACLE_PREAMBLE = r"""// GENERATED by 04a_contract_layouts.py --emit-oracle -- do not edit.
// Compiles the REAL Qubic core headers (contracts 1..15) with g++ and prints sizeof/alignof/offsetof for every type and
// member known to the Python reference, in the format "S <type>|<size>|<align>" / "O <type>|<member>|<offset>".
// Build:  g++ -std=c++20 -mavx2 -fno-access-control -Wno-invalid-offsetof -w -I <overlay> -I <core>/src -I <core> oracle.cpp
//   <overlay>/qpi_proposals_impl_patched.h = src/qpi/impl/qpi_proposals_impl.h with "pv.maxVotes" -> "ProposalVotingType::maxVotes"
//   (a template argument inside a member-function body; g++ 13 lacks P2280 "use of this in constant expression").
#define NO_UEFI
#include <cstdio>
#include <cstddef>
#include "platform/memory.h"
#include "contract_core/pre_qpi_def.h"
#include "qpi/qpi.h"
#include "qpi_proposals_impl_patched.h"
#include "oracle_core/oracle_interfaces_def.h"
#include "oc_core/oc_interfaces_def.h"
// glibc: uint64_t is 'unsigned long' but QPI::uint64 is 'unsigned long long' -> QPI::mod(T,T) deduction conflict in Qdraw.h
namespace QPI { inline constexpr unsigned long long mod(unsigned long a, unsigned long long b) { return b ? (a % b) : 0; } }
"""


def emit_oracle(version, built, path):
    model, results, extra, cinfo, changes, changes_line = built[version]
    lines = [ORACLE_PREAMBLE]
    for c in CONTRACTS:
        lines.append("#define %s_CONTRACT_INDEX %d" % (c["struct"], c["index"]))
        lines.append("#define CONTRACT_INDEX %s_CONTRACT_INDEX" % c["struct"])
        lines.append("#define CONTRACT_STATE_TYPE %s" % c["struct"])
        lines.append("#define CONTRACT_STATE2_TYPE %s2" % c["struct"])
        lines.append('#include "%s"' % c["header"][4:])
        lines.append("#undef CONTRACT_INDEX\n#undef CONTRACT_STATE_TYPE\n#undef CONTRACT_STATE2_TYPE")
    lines.append("struct Contract0State { long long contractFeeReserves[MAX_NUMBER_OF_CONTRACTS]; };")
    lines.append("struct IPO { m256i publicKeys[NUMBER_OF_COMPUTORS]; long long prices[NUMBER_OF_COMPUTORS]; };")
    lines.append("#define S(T) printf(\"S %s|%zu|%zu\\n\", #T, sizeof(T), alignof(T));")
    lines.append("#define O(T, M) printf(\"O %s|%s|%zu\\n\", #T, #M, offsetof(T, M));")
    lines.append("int main() {")
    n = 0
    for name, ty in model.all_types.items():
        if ty.kind == "enum":
            lines.append("  S(%s)" % cpp_name(name))
            n += 1
            continue
        lines.append("  { typedef %s T_; printf(\"S %s|%%zu|%%zu\\n\", sizeof(T_), alignof(T_));" % (cpp_name(name), name))
        n += 1
        for f in ty.fields:
            lines.append("    printf(\"O %s|%s|%%zu\\n\", offsetof(T_, %s));" % (name, f.name, f.name))
            n += 1
        lines.append("  }")
    for c in CONTRACTS:
        lines.append("  printf(\"C %d|%%zu\\n\", sizeof(%s));" % (c["index"], c["state"]))
    lines.append("  return 0;\n}")
    open(path, "w").write("\n".join(lines) + "\n")
    return n


def cpp_name(name):
    # canonical names are valid C++ except that integer template args of bool kind print as true/false (fine)
    return name


def check_oracle(version, built, path):
    model, results, extra, cinfo, changes, changes_line = built[version]
    sizes = {}
    offs = {}
    csize = {}
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("S "):
            t, s, a = line[2:].rsplit("|", 2)
            sizes[t] = (int(s), int(a))
        elif line.startswith("O "):
            t, m, o = line[2:].rsplit("|", 2)
            offs[(t, m)] = int(o)
        elif line.startswith("C "):
            i, s = line[2:].split("|")
            csize[int(i)] = int(s)
    bad = 0
    n = 0
    for name, ty in model.all_types.items():
        n += 1
        if sizes.get(name) != (ty.size, ty.align):
            bad += 1
            print("  MISMATCH size/align %s: python %s, g++ %s" % (name, (ty.size, ty.align), sizes.get(name)))
        for f in ty.fields:
            n += 1
            if offs.get((name, f.name)) != f.offset:
                bad += 1
                print("  MISMATCH offset %s.%s: python %d, g++ %s" % (name, f.name, f.offset, offs.get((name, f.name))))
    for c in CONTRACTS:
        n += 1
        if csize.get(c["index"]) != results[c["index"]].size:
            bad += 1
            print("  MISMATCH sizeof(%s): python %d, g++ %s" % (c["state"], results[c["index"]].size, csize.get(c["index"])))
    print("%s: g++ oracle cross-check: %d values compared, %d mismatches" % (version, n, bad))
    return bad


# ======================================================================================================================
# Markdown rendering (used to fill the report template)
# ======================================================================================================================
def md_fields_table(ty, with_line=True):
    rows = ["| # | offset | size | align | name | declared type | line |", "|--:|--:|--:|--:|---|---|--:|"]
    for i, f in enumerate(ty.fields):
        rows.append("| %d | %d | %d | %d | `%s` | `%s` | %s |" % (i, f.offset, f.ty.size, f.ty.align, f.name, f.decl.replace("|", "\\|"), f.line or ""))
    end = ty.fields[-1].offset + ty.fields[-1].ty.size if ty.fields else 0
    rows.append("| | **%d** | | **%d** | *sizeof* (tail padding %d) | | |" % (ty.size, ty.align, ty.size - end if ty.fields else 0))
    return "\n".join(rows)


def md_type_block(ty):
    """One-line-per-field compact description of a helper struct."""
    if ty.kind == "enum":
        return "- `%s` -- enum%s : `%s` (size %d) at %s:%s; enumerators: %s" % (
            ty.name, "", ty.underlying.name, ty.size, ty.src[0], ty.src[1], ", ".join("%s=%d" % e for e in ty.enumerators))
    parts = []
    last_end = 0
    for f in ty.fields:
        if ty.kind != "union" and f.offset > last_end:
            parts.append("*pad %d*" % (f.offset - last_end))
        parts.append("`%s %s` @%d(+%d)" % (f.decl, f.name, f.offset, f.ty.size))
        last_end = max(last_end, f.offset + f.ty.size)
    if ty.size > last_end and ty.fields:
        parts.append("*tail pad %d*" % (ty.size - last_end))
    where = " at %s:%s" % ty.src if ty.src and ty.src[1] else ""
    base = " : %s" % ty.base.name if ty.base else ""
    return "- `%s`%s -- %s, size **%d**, align %d%s: %s" % (ty.name, base, ty.kind, ty.size, ty.align, where, "; ".join(parts) if parts else "(no data members)")


def render(template_path, out_path, built):
    """Fill the {{PLACEHOLDER: args}} markers of the report template (REPORT_TEMPLATE below, or a file) with generated tables."""
    text = open(template_path).read() if template_path else REPORT_TEMPLATE

    def sub(m):
        kind, arg = m.group(1), m.group(2)
        a = [x.strip() for x in arg.split(",")]
        if kind == "FIELDS":       # {{FIELDS: version, canonical type}}
            ty = built[a[0]][0].all_types[a[1]]
            return md_fields_table(ty)
        if kind == "TYPES":        # {{TYPES: version, root type}} -> helper types reachable from root (excluding root)
            model = built[a[0]][0]
            seen = {}
            reachable(model.all_types[a[1]], seen)
            return "\n".join(md_type_block(t) for n, t in seen.items() if n != a[1])
        if kind == "TYPE":         # {{TYPE: version, canonical type}}
            return md_type_block(built[a[0]][0].all_types[a[1]])
        if kind == "SIZE":
            return str(built[a[0]][0].all_types[a[1]].size)
        if kind == "DEFLINE":      # {{DEFLINE: version, canonical type}} -> file:line of the definition
            ty = built[a[0]][0].all_types[a[1]]
            return "%s:%s" % ty.src if ty.src else "?"
        if kind == "CONSTVAL":     # {{CONSTVAL: version, NAME}}
            return str(built[a[0]][0].const_value(a[1]))
        if kind == "CONSTS":       # {{CONSTS: version, header}}
            model = built[a[0]][0]
            rows = ["| constant | value | initialiser | line |", "|---|--:|---|--:|"]
            for n, c in model.consts.items():
                if c["header"] == a[1]:
                    rows.append("| `%s` | %d | `%s` | %s |" % (n, c["value"], c["expr"], c["line"]))
            return "\n".join(rows)
        if kind == "SUMMARY":
            return md_summary(built)
        raise ValueError("unknown placeholder " + kind)

    text = re.sub(r"\{\{(\w+):?\s*([^}]*)\}\}", sub, text)
    open(out_path, "w").write(text)


def md_summary(built):
    rows = ["| idx | asset | struct | header | constr. epoch | state type | sizeof v1.303.2 | file size (.229) | match | sizeof HEAD | changed |",
            "|--:|---|---|---|--:|---|--:|--:|---|--:|---|"]
    for c in CONTRACTS:
        a = built["v1.303.2"][1][c["index"]]
        b = built["HEAD"][1][c["index"]]
        fs = file_size(c["index"])
        rows.append("| %d | %s | `%s` | `%s` | %d | `%s` | %d | %s | %s | %d | %s |" % (
            c["index"], c["asset"], c["struct"], c["header"], c["epoch"], c["state"], a.size, fs,
            "MATCH" if fs == a.size else "MISMATCH", b.size, "no" if same_layout(a, b) else "**YES**"))
    return "\n".join(rows)


def same_layout(a, b, strict_decl=True):
    """Same field names, offsets and sizes (and, if strict_decl, the same declared type text)."""
    if a.size != b.size or len(a.fields) != len(b.fields):
        return False
    return all((x.name, x.offset, x.ty.size) == (y.name, y.offset, y.ty.size) and (not strict_decl or x.decl == y.decl)
               for x, y in zip(a.fields, b.fields))


# ======================================================================================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", default=JSON_OUT, help="output JSON path")
    ap.add_argument("--emit-oracle", nargs=2, metavar=("VERSION", "OUT.cpp"))
    ap.add_argument("--check-oracle", nargs=2, metavar=("VERSION", "ORACLE.out"), action="append")
    ap.add_argument("--render", metavar="OUT.md", help="render the research report (section tables generated from the verified data)")
    ap.add_argument("--template", metavar="TEMPLATE.md", help="alternative report template (default: REPORT_TEMPLATE embedded below)")
    ap.add_argument("--dump", metavar="TYPE", help="print the field table of a canonical type (v1.303.2 and HEAD)")
    args = ap.parse_args()

    built = {v: build(v) for v in ROOTS}
    rc = 0
    for v, (model, results, extra, cinfo, changes, changes_line) in built.items():
        print("== %s (%s, epoch %d): %d source checks, %d problems" % (v, VERSION_INFO[v]["label"], VERSION_INFO[v]["epoch"], model.checks, len(model.problems)))
        for p in model.problems:
            print("   PROBLEM: " + p)
            rc = 1
        print("   contractStateChangeInfos (contract_def.h:%s): %s" % (changes_line, changes))
    print()
    print("%-3s %-8s %-22s %14s %14s %-8s %14s %s" % ("idx", "asset", "state type", "sizeof@229", "file size", "result", "sizeof@HEAD", "HEAD vs 229"))
    for c in CONTRACTS:
        a = built["v1.303.2"][1][c["index"]]
        b = built["HEAD"][1][c["index"]]
        fs = file_size(c["index"])
        ok = fs == a.size
        rc |= 0 if ok else 1
        print("%-3d %-8s %-22s %14d %14s %-8s %14d %s" % (c["index"], c["asset"], c["state"], a.size, fs, "MATCH" if ok else "MISMATCH", b.size,
                                                    "same" if same_layout(a, b) else "CHANGED"))
    # NOST: HEAD keeps the pre-migration layout as NOST::OldStateData (input of MIGRATE at epoch 230); it must equal the
    # v1.303.2 NOST::StateData field by field (same names, offsets, sizes) for the epoch-229 file to be decodable by HEAD.
    old_new = same_layout(built["v1.303.2"][1][14], built["HEAD"][0].all_types["NOST::OldStateData"], strict_decl=False)
    print("\nNOST: HEAD NOST::OldStateData == v1.303.2 NOST::StateData (names/offsets/sizes; only the constant names carry an _OLD suffix): %s" % old_new)
    rc |= 0 if old_new else 1
    js = make_json(built)
    with open(args.json, "w") as f:
        json.dump(js, f, indent=1)
    print("\nwrote %s" % args.json)
    if args.emit_oracle:
        n = emit_oracle(args.emit_oracle[0], built, args.emit_oracle[1])
        print("wrote %s (%d checks)" % (args.emit_oracle[1], n))
    for v, p in args.check_oracle or []:
        rc |= 1 if check_oracle(v, built, p) else 0
    if args.render:
        render(args.template, args.render, built)
        print("rendered %s" % args.render)
    if args.dump:
        for v in built:
            ty = built[v][0].all_types.get(args.dump)
            if ty:
                print("\n%s %s" % (v, args.dump))
                print(md_fields_table(ty))
    return rc


if __name__ == "__main__":
    sys.exit(main())
