# 02 — QPI proposal / voting types as they appear in contract states

Research report for qstate-viewer. Everything below is grounded in the two core checkouts:

* HEAD = `/home/yeti/devwork/space/core`, git `6cdc1bc7` tag `v1.306.0`, `src/public_settings.h:80` `#define EPOCH 233`
* OLD  = `/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/core-v1.303.2`, `src/public_settings.h:80` `#define EPOCH 229`, `:81 #define TICK 77700000`

File paths below are relative to `src/` of either checkout unless stated otherwise. All line numbers were checked against HEAD; the OLD files involved are byte-identical (see §1).

Verification artefacts (all under `/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/research/`):

| file | purpose |
|---|---|
| `scripts/p02_oracle_proposals.cpp`, `scripts/p02_build_oracle.sh` | g++ "ground truth": compiles the REAL qpi headers + REAL contract files (QX, QUTIL, GQMPROP, CCF, TESTEXA, TESTEXB) and prints sizeof/alignof/offsetof of every proposal type and of the five `StateData`s |
| `p02-work/oracle-HEAD/oracle.out`, `p02-work/oracle-v1.303.2/oracle.out` | oracle output for both core versions (identical, 233 lines) |
| `scripts/p02_decode_proposals.py` | stdlib-only decoder: decodes GQMPROP / CCF / QUTIL proposals from the epoch-229 files, generic `blob` mode for any `ProposalVoting<>` instantiation, `--canonical` mode, self-test (K12 vector, identity checksum, 10 sizeof vectors) |
| `p02-work/decode_gqmprop.txt`, `decode_ccf.txt`, `decode_qutil.txt`, `decode_gqmprop_votes_first2.txt` | decoder output on the real files |
| `scripts/p02_make_fixtures.cpp`, `scripts/p02_build_fixtures.sh` | generates 3 synthetic state blobs with the REAL core code (`ProposalWithAllVoteData::set/setVoteValue/getVoteValue`, `QpiContextProposalFunctionCall::getVotingSummary`, `ProposalAndVotingByShareholders::getVoterId/getVoteCount`) plus an "expected" dump written by that real code |
| `p02-work/fixtures/fixture{A,B,C}*.bin / *.expected.txt / *.python.txt` | golden files; `diff` of python decode vs real-code dump is empty for all three |

---

## 0. TL;DR

* Only ONE family of types is persisted: `QPI::ProposalVoting<ProposerAndVoterHandlingT, ProposalDataT>` (`qpi/qpi_proposals.h:472-499`). It appears as a member named `proposals` in exactly five contracts: GQMPROP (index 6), CCF (8), QUTIL (4) and the test-only TESTEXA / TESTEXB (compiled only with `INCLUDE_CONTRACT_TEST_EXAMPLES`, `contract_core/contract_def.h:322`). Identical in both core versions.
* Layout (all alignments are 8, every size below is a multiple of 8 except where noted):
  * `ProposalVoting<H,D>` = `{ H proposersAndVoters; ProposalWithAllVoteData<D, H::maxVotes> proposals[H::maxProposals]; }`
  * `ProposalAndVotingByComputors<N>` / `ProposalByAnyoneVotingByComputors<N>` = `{ id currentProposalProposers[N]; }` → 32·N bytes, `maxProposals = N`, `maxVotes = 676`
  * `ProposalAndVotingByShareholders<N, asset>` = `{ id currentProposalProposers[N]; id currentProposalShareholders[N][676]; }` → 32·N + 32·N·676 bytes
  * `ProposalWithAllVoteData<D, V>` = `D` (base, 304 or 328 bytes, no tail padding) + vote storage, rounded up to 8:
    * `D = ProposalDataYesNo` (304): `uint8 votes[(2*V+7)/8]` (2 bits/vote, 3 = no vote) → 480 bytes for V=676
    * `D = ProposalDataV1<false>` (328): `uint8 votes[V]` (0xff = no vote) → 1008 bytes
    * `D = ProposalDataV1<true>` (328): `sint64 votes[V]` (`NO_VOTE_VALUE` = 0x8000000000000000 = no vote) → 5736 bytes
* Instantiations in state (oracle-confirmed, file-size cross-checked):
  * GQMPROP: `ProposalVoting<ProposalAndVotingByComputors<676>, ProposalDataV1<false>>` = 703 040 bytes at offset 0; StateData = 709 184 = `contract0006.229` size.
  * CCF: `ProposalVoting<ProposalAndVotingByComputors<100>, ProposalDataYesNo>` = 51 200 bytes at offset 0; StateData = 493 584 = `contract0008.229` size.
  * QUTIL: `ProposalVoting<ProposalAndVotingByShareholders<8, 327647778129>, ProposalDataYesNo>` = 177 152 bytes (= 8 × 22 144, matching `doc/contracts_proposals.md:71`) at offset 402 717 952 (last member); StateData = 402 895 104 = `contract0004.229` size.
* The decoder reproduces 41 real GQMPROP proposals and 73 real CCF proposals with plausible URLs/epochs/ticks/vote totals (≤ 676), 42 CCF transfer-log entries match decoded proposals exactly, and on the synthetic fixtures the Python decode is byte-identical to the dump written by the real core code (including the 2-bit packing, the sint64 scalar storage and the two-path scalar mean).
* A generic engine needs: class templates with bool/uint16/uint32/uint64 NTTPs, default template arguments given only on a forward declaration, inheritance from a template *type parameter* and from a dependent template-id, dependent static constexpr members (`H::maxProposals`), lazy evaluation of static constexpr initializers, explicit + partial specialization with canonical type matching through typedef chains, `A<expr>::type` without `typename`, named unions with inline declarators, anonymous unions, 2-D arrays, constant expressions with `* + / |` over mixed NTTP/macro/constexpr-global operands, `using namespace`, friend/static_assert/member-function skipping, and a preprocessor with include paths, function-like multi-line macros, identical redefinition and `#ifndef` inside class bodies. Full list with citations in §4.

---

## 1. Sources, versions, include order

Files that define the types (both versions byte-identical — verified with `diff`, exit 0, for `qpi_proposals.h`, `impl/qpi_proposals_impl.h`, `qpi_macros.h`, `qpi_types.h`, `qpi.h`, `qpi_containers.h`, `doc/contracts_proposals.md`, `contracts/GeneralQuorumProposal.h`, `contracts/ComputorControlledFund.h`, `contracts/QUtil.h`, `contracts/TestExampleA.h`, `contracts/TestExampleB.h`):

| file | content |
|---|---|
| `qpi/qpi_types.h` | `sint8..uint64` typedefs (:11-18), `NUMBER_OF_COMPUTORS 676` and `QUORUM` macros (:28-29), `bit` (:34-46), `typedef m256i id` (:62) |
| `platform/m256.h` | `union m256i` = `id` (:9-205), `operator<` (:295-306) |
| `qpi/qpi_containers.h` | `template <typename T, uint64 L> struct Array { T _values[L]; ... }` (:115-123) |
| `qpi/qpi_proposals.h` | constants, vote I/O structs, `ProposalTypes`, `ProposalDataV1`, `ProposalDataYesNo`, forward decls, `ProposalVoting`, `QpiContextProposal*Call` |
| `qpi/impl/qpi_proposals_impl.h` | **definitions** of `ProposalAndVotingByComputors`, `ProposalByAnyoneVotingByComputors`, `ProposalAndVotingByShareholders`, `__VoteStorageTypeSelector`, `ProposalWithAllVoteData` (+ partial specialization), all member-function bodies |
| `qpi/qpi_macros.h` | `DEFINE_SHAREHOLDER_PROPOSAL_TYPES` and the `IMPLEMENT_*`/`REGISTER_*` shareholder-voting macros (:441-570), `SET_SHAREHOLDER_*` system-procedure typedefs/macros (:46-56, :163-187) |
| `contract_core/contract_def.h` | include protocol: `pre_qpi_def.h`, `qpi/qpi.h`, **`qpi/impl/qpi_proposals_impl.h` (line 16, BEFORE the contracts)**, oracle/oc interface headers, then per contract `#define CONTRACT_INDEX/CONTRACT_STATE_TYPE/CONTRACT_STATE2_TYPE` + `#include "contracts/X.h"` (:24-318); `contractDescriptions[]` with `sizeof(X::StateData)` (:405-443) |

Important for the parser: unlike the other `impl/*.h` files (collection/hash map/linked list, included *after* the contracts, `contract_def.h:380-383`), `qpi_proposals_impl.h` must be in the parse set because it holds the class *definitions* needed for layout. `qpi_proposals_impl.h:3` does `#include "qpi/qpi.h"` (path relative to `src/`, so the preprocessor needs `src/` as include dir); `qpi_proposals.h:3` does `#include "qpi_types.h"` (relative to its own directory).

Contract indices (same in both versions, `contract_def.h:54,74,94`; OLD `:390-394`): QUTIL = 4, GQMPROP = 6, CCF = 8. HEAD adds QPAYHUB = 29 and QTREAT = 30 (not proposal users). No production contract uses `ProposalByAnyoneVotingByComputors` or `ProposalDataV1<true>` (only `test/qpi.cpp` and TESTEXB).

Other contracts contain *custom* proposal structs that are ordinary structs/Arrays and need no special handling (grep "proposal" hits in HEAD: QVAULT, VottunBridge, QRaffle, GGWP, qRWA, Quottery, QTREAT, Qusino, MsVault) — none of them uses the QPI proposal types.

---

## 2. Type catalogue (verbatim members, in declaration order)

Notation: `@off` = byte offset, `sz` = size, both from the g++ oracle (`p02-work/oracle-HEAD/oracle.out`); alignment of every type here is 8 unless noted.

### 2.1 Namespace-scope constants (`qpi/qpi_proposals.h:7-9`, `qpi/qpi_types.h:28-29`)

```cpp
constexpr uint16 INVALID_PROPOSAL_INDEX = 0xffff;                 // 65535
constexpr uint32 INVALID_VOTE_INDEX = 0xffffffff;                 // 4294967295
constexpr sint64 NO_VOTE_VALUE = 0x8000000000000000;              // -9223372036854775808 (literal is unsigned, converted)
#define NUMBER_OF_COMPUTORS 676                                   // also network_messages/common_def.h:6 (identical redefinition)
#define QUORUM (NUMBER_OF_COMPUTORS * 2 / 3 + 1)                  // 451
```

### 2.2 Vote I/O structs (never in `StateData`; used in `_input/_output/_locals` only)

`ProposalSingleVoteDataV1` (`:13-28`), `static_assert(sizeof == 16)` (`:29`):

| member | type | @off | sz |
|---|---|---|---|
| proposalIndex | uint16 | 0 | 2 |
| proposalType | uint16 | 2 | 2 |
| proposalTick | uint32 | 4 | 4 |
| voteValue | sint64 | 8 | 8 |

`ProposalMultiVoteDataV1` (`:37-57`), `static_assert(sizeof == 104)` (`:58`): proposalIndex uint16 @0, proposalType uint16 @2, proposalTick uint32 @4, `Array<sint64, 8> voteValues` @8 (64), `Array<uint32, 8> voteCounts` @72 (32).

`ProposalSummarizedVotingDataV1` (`:62-130`), `static_assert(sizeof == 16 + 8 * 4)` = 48 (`:131`):

| member | type | @off | sz |
|---|---|---|---|
| proposalIndex | uint16 | 0 | 2 |
| optionCount | uint16 | 2 | 2 |
| proposalTick | uint32 | 4 | 4 |
| totalVotesAuthorized | uint32 | 8 | 4 |
| totalVotesCasted | uint32 | 12 | 4 |
| **anonymous union** `{ Array<uint32, 8> optionVoteCount; sint64 scalarVotingResult; }` (`:80-87`) | | 16 | 32 |

Also contains inline member functions `getMostVotedOption()` (`:90-106`), `getAcceptedOption(uint32 totalVotesThresh = QUORUM, uint32 mostVotedThreshold = QUORUM / 2)` (`:109-118`), `= default` ctor, user-provided copy ctor and `operator=` (`:120-129`).

### 2.3 `namespace ProposalTypes` (`:137-234`) — the `type` field encoding

```cpp
namespace ProposalTypes {
  namespace Class {                                    // high byte = class
    static constexpr uint16 GeneralOptions  = 0;       // :143  options 2..8 (V1), 2..3 (YesNo); data unused
    static constexpr uint16 Transfer        = 0x100;   // :146  options 2..5 (V1), ==2 (YesNo)
    static constexpr uint16 Variable        = 0x200;   // :149  options 2..5 (V1), ==2 (YesNo); 0 = scalar (V1<true> only)
    static constexpr uint16 MultiVariables  = 0x300;   // :152  options 2..8 (V1), 2..3 (YesNo); data stored by contract
    static constexpr uint16 TransferInEpoch = 0x400;   // :155  options ==2 (V1 only)
  };                                                   // :156  (stray ';')
  static constexpr uint16 Invalid = 0;                                     // :159
  static constexpr uint16 YesNo = Class::GeneralOptions | 2;               // :162  0x0002
  static constexpr uint16 ThreeOptions = Class::GeneralOptions | 3;        // :165  0x0003
  static constexpr uint16 FourOptions = Class::GeneralOptions | 4;         // :168  0x0004
  static constexpr uint16 TransferYesNo = Class::Transfer | 2;             // :171  0x0102
  static constexpr uint16 TransferTwoAmounts = Class::Transfer | 3;        // :174  0x0103
  static constexpr uint16 TransferThreeAmounts = Class::Transfer | 4;      // :177  0x0104
  static constexpr uint16 TransferFourAmounts = Class::Transfer | 5;       // :180  0x0105
  static constexpr uint16 TransferInEpochYesNo = Class::TransferInEpoch | 2; // :183 0x0402
  static constexpr uint16 VariableYesNo = Class::Variable | 2;             // :186  0x0202
  static constexpr uint16 VariableTwoValues = Class::Variable | 3;         // :189  0x0203
  static constexpr uint16 VariableThreeValues = Class::Variable | 4;       // :192  0x0204
  static constexpr uint16 VariableFourValues = Class::Variable | 5;        // :195  0x0205
  static constexpr uint16 VariableScalarMean = Class::Variable | 0;        // :198  0x0200
  static constexpr uint16 MultiVariablesYesNo = Class::MultiVariables | 2; // :203  0x0302
  static constexpr uint16 MultiVariablesThreeOptions = Class::MultiVariables | 3; // :207 0x0303
  static constexpr uint16 MultiVariablesFourOptions = Class::MultiVariables | 4;  // :211 0x0304
  static constexpr uint16 type(uint16 cls, uint16 options) { return cls | options; } // :214
  static uint16 optionCount(uint16 proposalType) { return proposalType & 0x00ff; }  // :221
  static uint16 cls(uint16 proposalType) { return proposalType & 0xff00; }          // :227
  inline static bool isValid(uint16 proposalType);   // :233, defined out of line qpi_proposals_impl.h:343-367
};                                                   // :234 (stray ';')
```

### 2.4 `template <bool SupportScalarVotes> struct ProposalDataV1` (`:239-362`)

`static_assert(sizeof(ProposalDataV1<true>) == 256 + 8 + 64)` (`:363`) = 328; oracle: `ProposalDataV1<false>` is also 328 (the parameter does not influence this struct's layout, only `supportScalarVotes`).

| member | type (verbatim) | @off | sz |
|---|---|---|---|
| url | `Array<uint8, 256>` | 0 | 256 |
| epoch | uint16 | 256 | 2 |
| type | uint16 | 258 | 2 |
| tick | uint32 | 260 | 4 |
| data | `union Data { ... }` (named union defined inline, declarator `} data;` `:255-290`) | 264 | 64 |

Union alternatives (all at offset 0 of the union, union size 64 = max, align 8):

| alternative | members | sz |
|---|---|---|
| `struct Transfer { id destination; Array<sint64, 4> amounts; } transfer;` (`:258-262`) | destination @0 (32), amounts @32 (32) | 64 |
| `struct TransferInEpoch { id destination; sint64 amount; uint16 targetEpoch; } transferInEpoch;` (`:265-270`) | destination @0, amount @32, targetEpoch @40 (2) | 48 (6 tail pad) |
| `struct VariableOptions { uint64 variable; Array<sint64, 4> values; } variableOptions;` (`:273-277`) | variable @0, values @8 (32) | 40 |
| `struct VariableScalar { uint64 variable; sint64 minValue; sint64 maxValue; sint64 proposedValue; static constexpr sint64 minSupportedValue = 0x8000000000000001; static constexpr sint64 maxSupportedValue = 0x7fffffffffffffff; } variableScalar;` (`:280-289`) | variable @0, minValue @8, maxValue @16, proposedValue @24 | 32 |

Non-layout members: `bool checkValidity() const` (`:294-347`, body with `switch`), `static constexpr bool supportScalarVotes = SupportScalarVotes;` (`:350`, declared AFTER the data members and after a function that uses it), `ProposalDataV1() = default;` (`:352`), copy ctor and `operator=` written with the template-id `ProposalDataV1<SupportScalarVotes>` (`:353-361`). Because of the user-provided copy ctor the type is not trivially copyable (oracle: `is_trivially_copyable = 0`) but it is standard-layout (oracle: 1).

### 2.5 `struct ProposalDataYesNo` (`:367-436`), `static_assert(sizeof == 256 + 8 + 40)` = 304 (`:437`)

| member | type | @off | sz |
|---|---|---|---|
| url | `Array<uint8, 256>` | 0 | 256 |
| epoch | uint16 | 256 | 2 |
| type | uint16 | 258 | 2 |
| tick | uint32 | 260 | 4 |
| data | `union Data { struct Transfer { id destination; sint64 amount; } transfer; struct VariableOptions { uint64 variable; sint64 value; } variableOptions; } data;` (`:382-397`) | 264 | 40 |

`Transfer`: destination @0 (32), amount @32 (8) → 40. `VariableOptions`: variable @0, value @8 → 16. `checkValidity()` (`:401-421`), `static constexpr bool supportScalarVotes = false;` (`:424`), ctors/operator= as above (`:426-435`).

### 2.6 Forward declarations (`:440-461`)

```cpp
template <typename ProposalDataType, uint32 numOfVoters> struct ProposalWithAllVoteData;        // :441-442 (definition names the param numOfVotes!)
template <uint16 proposalSlotCount = NUMBER_OF_COMPUTORS> struct ProposalAndVotingByComputors; // :446-447 DEFAULT ONLY HERE
template <uint16 proposalSlotCount> struct ProposalByAnyoneVotingByComputors;                  // :450-451
template <uint16 proposalSlotCount, uint64 contractAssetName> struct ProposalAndVotingByShareholders; // :454-455
template <typename ProposerAndVoterHandlingType, typename ProposalDataType> struct QpiContextProposalFunctionCall;  // :457-458
template <typename ProposerAndVoterHandlingType, typename ProposalDataType> struct QpiContextProposalProcedureCall; // :460-461
```

The default argument `= NUMBER_OF_COMPUTORS` exists only on the forward declaration; the definition (`qpi_proposals_impl.h:8`) has none. It is used in `test/qpi.cpp:464` (`QPI::ProposalAndVotingByComputors pv;`) but by no contract.

### 2.7 `template <typename ProposerAndVoterHandlingT, typename ProposalDataT> class ProposalVoting` (`:472-499`)

Note the class-key `class` (default access private) with explicit `public:` (`:475`) and `protected:` (`:492`).

```cpp
public:
  static constexpr uint16 maxProposals = ProposerAndVoterHandlingT::maxProposals;   // :476  dependent static member
  static constexpr uint32 maxVotes = ProposerAndVoterHandlingT::maxVotes;           // :477
  typedef ProposerAndVoterHandlingT ProposerAndVoterHandlingType;                   // :479
  typedef ProposalDataT ProposalDataType;                                           // :480
  typedef ProposalWithAllVoteData<ProposalDataT, maxVotes> ProposalAndVotesDataType; // :481-484 (spans 4 lines)
  static_assert(maxProposals <= INVALID_PROPOSAL_INDEX);                            // :486 (no message)
  static_assert(maxVotes <= INVALID_VOTE_INDEX);                                    // :487
  ProposerAndVoterHandlingType proposersAndVoters;                                  // :490  DATA MEMBER 1  @0
protected:
  ProposalAndVotesDataType proposals[maxProposals];                                 // :494  DATA MEMBER 2  @sizeof(H)
  friend struct QpiContextProposalProcedureCall<ProposerAndVoterHandlingT, ProposalDataT>; // :497
  friend struct QpiContextProposalFunctionCall<ProposerAndVoterHandlingT, ProposalDataT>;  // :498
```

`sizeof(ProposalVoting<H,D>) = sizeof(H) + maxProposals * sizeof(ProposalWithAllVoteData<D, maxVotes>)` (no padding between: both have alignment 8 and sizeof(H) is a multiple of 32). Mixed access (public + protected data) makes it non-standard-layout, which changes nothing in practice.

### 2.8 Proposer/voter handling classes (`qpi/impl/qpi_proposals_impl.h`)

`template <uint16 proposalSlotCount> struct ProposalAndVotingByComputors` (`:8-115`):

```cpp
static constexpr uint16 maxProposals = proposalSlotCount;   // :12
static constexpr uint32 maxVotes = NUMBER_OF_COMPUTORS;     // :15  (676)
// member functions :18-110 (isValidProposer, getNewProposalIndex, freeProposalByIndex, getProposerId,
//   getExistingProposalIndex, getVoteIndex(..., uint16 proposalIndex = 0), getVoteCount, getVoterId) -- skipped
protected:
id currentProposalProposers[maxProposals];                  // :114  ONLY data member, @0, 32*N bytes
```

`template <uint16 proposalSlotCount> struct ProposalByAnyoneVotingByComputors : public ProposalAndVotingByComputors<proposalSlotCount>` (`:117-125`): no data members, one member function (`isValidProposer`). `maxProposals`/`maxVotes` are inherited → `ProposalVoting` finds them through the base class. Layout identical to the base (oracle: `<200>` = 6400 bytes, `currentProposalProposers` @0).

`template <uint16 proposalSlotCount, uint64 contractAssetName> struct ProposalAndVotingByShareholders` (`:130-340`):

```cpp
static constexpr uint16 maxProposals = proposalSlotCount;   // :134
static constexpr uint32 maxVotes = NUMBER_OF_COMPUTORS;     // :137  (676 shares per contract)
// member functions :140-333; note ':143' body followed by a stray ';', a local 'struct Shareholder' inside
//   setupNewProposal (:154-158), '#ifndef NDEBUG ... #endif' inside the body (:200-214)
protected:
id currentProposalProposers[maxProposals];                           // :338  @0,          32*N
id currentProposalShareholders[maxProposals][NUMBER_OF_COMPUTORS];   // :339  @32*N,       32*N*676  (2-D array)
```

`contractAssetName` is layout-irrelevant but part of the type identity (values: QUTIL 327647778129 `contracts/QUtil.h:19`, TESTEXA 18392928276923732 `TestExampleA.h:3`, TESTEXB 18674403253634388 `TestExampleB.h:3` — 64-bit constant evaluation required).

Oracle sizes: `<676>` 21632, `<100>` 3200, `<200>` 6400; shareholders `<8,·>` 173312 (= 256 + 173056), `<16,·>` 346624 (= 512 + 346112), `<3,·>` 64992.

### 2.9 `__VoteStorageTypeSelector` (`:369-373`)

```cpp
template <bool scalarVotesSupported> struct __VoteStorageTypeSelector { typedef uint8 type;  };   // primary
template <> struct __VoteStorageTypeSelector<true> { typedef sint64 type; };                       // explicit (full) specialization
```

(empty structs, sizeof 1; only the nested typedef matters.)

### 2.10 `ProposalWithAllVoteData` — primary template (`:376-477`) and partial specialization (`:479-545`)

Primary: `template <typename ProposalDataType, uint32 numOfVotes> struct ProposalWithAllVoteData : public ProposalDataType`

```cpp
static constexpr bool supportScalarVotes = ProposalDataType::supportScalarVotes;      // :382  dependent static bool
typedef __VoteStorageTypeSelector<supportScalarVotes>::type VoteStorageType;          // :383  NOTE: no 'typename'
VoteStorageType votes[numOfVotes];                                                    // :386  ONLY own data member
// bool set(const ProposalDataType&) :389-411, bool setVoteValue(uint32, sint64) :414-453, sint64 getVoteValue(uint32) const :456-476
```

Partial specialization: `template <uint32 numOfVotes> struct ProposalWithAllVoteData<ProposalDataYesNo, numOfVotes> : public ProposalDataYesNo`

```cpp
uint8 votes[(2 * numOfVotes + 7) / 8];     // :485  2 bits per vote; ONLY own data member
// set :488-500, setVoteValue :503-528 (bit ops), getVoteValue :531-544
```

Resulting element layouts (oracle):

| instantiation | base sz | votes @ | votes sz | elem sz | tail pad |
|---|---|---|---|---|---|
| `<ProposalDataYesNo, 676>` | 304 | 304 | 169 | 480 | 7 |
| `<ProposalDataV1<false>, 676>` | 328 | 328 | 676 | 1008 | 4 |
| `<ProposalDataV1<true>, 676>` | 328 | 328 | 5408 | 5736 | 0 |
| `<ProposalDataYesNo, 42>` | 304 | 304 | 11 | 320 | 5 |
| `<ProposalDataV1<false>, 42>` | 328 | 328 | 42 | 376 | 6 |
| `<ProposalDataV1<true>, 42>` | 328 | 328 | 336 | 664 | 0 |
| `<ProposalDataYesNo, 1>`, `<…,4>`, `<…,5>` | 304 | 304 | 1, 1, 2 | 312 | 7, 7, 6 |

The base-class members (url @0, epoch @256, type @258, tick @260, data @264) keep their offsets in the derived class (oracle lines for `PWAV_*`).

### 2.11 `QpiContextProposalFunctionCall` / `QpiContextProposalProcedureCall` (`qpi_proposals.h:502-620`) — never in state

Structs with two reference members `const QpiContextFunctionCall& qpi; const ProposalVotingType& pv;` (`:557-558`), a constructor with mem-initializer list (`:551-555`), member function declarations with default arguments (`:526,530,535,540`), and the derived struct inheriting from the template-id `QpiContextProposalFunctionCall<…>` (`:563`) with `BaseClass(qpi, pv)` initializer (`:615-619`). Their member functions are defined out of line in `qpi_proposals_impl.h:547-1104`; `qpi_context.h:243-246,379-382` declare the `operator()` overloads that create them (`qpi(state.get().proposals).func()` idiom). The parser only has to skip all of this.

### 2.12 Supporting types

* `id` = `typedef m256i id;` (`qpi_types.h:62`); `union m256i` (`platform/m256.h:9-205`): members `int8_t m256i_i8[32]; int16_t m256i_i16[16]; int32_t m256i_i32[8]; int64_t m256i_i64[4]; uint8_t m256i_u8[32]; uint16_t m256i_u16[16]; uint32_t m256i_u32[8]; uint64_t m256i_u64[4];` plus eight unnamed-struct members with declarators `struct { uint64_t _0, _1, _2, _3; } u64;` … `struct { int8_t _0..._31; } i8;` (`:22-55`), many constructors/operators (`:57-204`); **no `__m256i` member, no `alignas`** → sizeof 32, alignof 8 (`static_assert(sizeof(m256i) == 32)` `:207`; oracle confirms align 8). Ordering used for shareholder sorting: `operator<` compares `m256i_u64[0..3]` in that order (`:295-306`).
* `Array<T, L>` (`qpi_containers.h:115-123`): `private: static_assert(L && !(L & (L - 1)), ...); T _values[L];` → sizeof = L·sizeof(T), align = alignof(T).
* `bit` (`qpi_types.h:34-46`): `struct bit { char charValue; }` → 1 byte (used in CCF entries, not in proposal types).
* No `#pragma pack`, `alignas`, `__declspec(align)`, bit-fields or enums anywhere in `qpi/` or `contracts/` (grep, HEAD). The keyword `union` is forbidden in contract code (`qpi/qpi.h:27`), so the only unions a state can contain are `m256i`, the proposal `Data` unions and (never in state) the anonymous union of §2.2.

### 2.13 Macros (`qpi/qpi_macros.h`)

```cpp
#define DEFINE_SHAREHOLDER_PROPOSAL_TYPES(numProposalSlots, assetNameInt64) \      // :441-445
        public: \
            typedef ProposalDataYesNo ProposalDataT; \
            typedef ProposalAndVotingByShareholders<numProposalSlots, assetNameInt64> ProposersAndVotersT; \
            typedef ProposalVoting<ProposersAndVotersT, ProposalDataT> ProposalVotingT
```

Use site `contracts/QUtil.h:137` `DEFINE_SHAREHOLDER_PROPOSAL_TYPES(8, QUTIL_CONTRACT_ASSET_NAME);` (the `;` comes from the use site). `gcc -E` expansion (`p02-work/oracle_head.ii:45031`):

```cpp
public: typedef ProposalDataYesNo ProposalDataT; typedef ProposalAndVotingByShareholders<8, QUTIL_CONTRACT_ASSET_NAME> ProposersAndVotersT; typedef ProposalVoting<ProposersAndVotersT, ProposalDataT> ProposalVotingT;
```

The macro defines TYPES ONLY; the state member is written by hand: `ProposalVotingT proposals;` (`QUtil.h:176`). Other macros in the same block only generate functions/procedures and their `_input/_output/_locals` structs (never state): `IMPLEMENT_SetShareholderProposal` (`:447-463`), `IMPLEMENT_GetShareholderProposal` (`:465-470`, output `{ ProposalDataT proposal; id proposerPubicKey; }`), `IMPLEMENT_GetShareholderProposalIndices` (`:472-485`), `IMPLEMENT_GetShareholderProposalFees` (`:487-492`), `IMPLEMENT_SetShareholderVotes` (`:494-498`), `IMPLEMENT_GetShareholderVotes` (`:500-504`), `IMPLEMENT_GetShareholderVotingResults` (`:506-510`), `IMPLEMENT_SET_SHAREHOLDER_PROPOSAL` (`:512-516`), `IMPLEMENT_SET_SHAREHOLDER_VOTES` (`:518-520`), `IMPLEMENT_FinalizeShareholderStateVarProposals` (`:523-546`), `IMPLEMENT_DEFAULT_SHAREHOLDER_PROPOSAL_VOTING` (`:548-557`), `REGISTER_*` (`:559-570`, function ids 65531-65535, procedure ids 65534-65535). System-procedure I/O typedefs: `typedef Array<uint8, 1024> SET_SHAREHOLDER_PROPOSAL_input;` (`:47`), `typedef ProposalMultiVoteDataV1 SET_SHAREHOLDER_VOTES_input;` (`:53`).

**History (relevant only if older cores are ever supported):** `DEFINE_SHAREHOLDER_PROPOSAL_STORAGE(numProposalSlots, assetNameInt64)` existed from commit `9540a9de` (first tag v1.266.0) to `ff681a10` "Automatic state change detection (#774)" (first tag v1.282.0). It expanded to the same three typedefs **plus** `protected: ProposalVotingT proposals` — i.e. the state member itself was macro-generated inside the contract struct (pre-`StateData` era). Neither v1.303.2 nor v1.306.0 contains it (grep: no occurrence).

### 2.14 Free test vectors

| assertion / source | value |
|---|---|
| `static_assert(sizeof(ProposalSingleVoteDataV1) == 16)` `qpi_proposals.h:29` | 16 |
| `static_assert(sizeof(ProposalMultiVoteDataV1) == 104)` `:58` | 104 |
| `static_assert(sizeof(ProposalSummarizedVotingDataV1) == 16 + 8 * 4)` `:131` | 48 |
| `static_assert(sizeof(ProposalDataV1<true>) == 256 + 8 + 64)` `:363` | 328 |
| `static_assert(sizeof(ProposalDataYesNo) == 256 + 8 + 40)` `:437` | 304 |
| class-scope `static_assert(maxProposals <= INVALID_PROPOSAL_INDEX)`, `static_assert(maxVotes <= INVALID_VOTE_INDEX)` `:486-487` | dependent, true for all instantiations |
| `static_assert(sizeof(m256i) == 32)` `platform/m256.h:207` | 32 |
| `static_assert(sizeof(IPO) == 32 * NUMBER_OF_COMPUTORS + 8 * NUMBER_OF_COMPUTORS)` `contract_def.h:396` | 27040 (= contract0005/0007/0021.229 sizes) |
| `doc/contracts_proposals.md:71` "each proposal slot occupies 22144 Bytes" (YesNo shareholder slot) | 32 + 32·676 + 480 = 22144 |
| oracle: `sizeof(GQMPROP::ProposalVotingT)`, `CCF::`, `QUTIL::`, `TESTEXA::`, `TESTEXB::` | 703040, 51200, 177152, 354304, 438400 |
| oracle: `ProposalVoting<ProposalAndVotingByComputors<200>, ProposalDataV1<true>>`, `ProposalVoting<ProposalByAnyoneVotingByComputors<200>, ProposalDataV1<false>>` (test/qpi.cpp:1332-1337 types) | 1153600, 208000 |
| oracle: `sizeof(GQMPROP::StateData)`, `CCF::StateData`, `QUTIL::StateData`, `TESTEXA::StateData`, `TESTEXB::StateData` | 709184, 493584, 402895104, 363104, 439024 |

Recommendation: the engine should evaluate every parsed `static_assert(sizeof(...) == ...)` as a start-up self-test and surface failures in a diagnostics panel — the headers ship dozens of them for free.

---

## 3. Layout rules and formulas

### 3.1 Formulas (all alignments 8; `align8(x) = (x + 7) & ~7`)

```
sizeof(id) = 32, alignof(id) = 8
sizeof(ProposalDataYesNo)   = 304    sizeof(ProposalDataV1<b>) = 328          (no tail padding in either)
voteBytes(YesNo, V)      = (2*V + 7) / 8          (integer division; V = 676 -> 169)
voteBytes(V1<false>, V)  = V                       (676)
voteBytes(V1<true>, V)   = 8*V                     (5408)
sizeof(ProposalWithAllVoteData<D,V>) = align8(sizeof(D) + voteBytes(D,V))       -> 480 / 1008 / 5736
offsetof(votes) = sizeof(D)                                                     -> 304 / 328 / 328

sizeof(ProposalAndVotingByComputors<N>)         = 32*N              (currentProposalProposers @0)
sizeof(ProposalByAnyoneVotingByComputors<N>)    = 32*N              (same, inherited)
sizeof(ProposalAndVotingByShareholders<N,A>)    = 32*N + 32*N*676   (proposers @0, shareholders @32*N; row i at 32*N + i*32*676)

sizeof(ProposalVoting<H,D>) = sizeof(H) + H::maxProposals * sizeof(ProposalWithAllVoteData<D, H::maxVotes>)
offsetof(proposersAndVoters) = 0;  offsetof(proposals) = sizeof(H);  proposals[i] at sizeof(H) + i*elemSize
```

Pseudocode of the generic instantiation (what the engine effectively has to do for `GQMPROP::StateData::proposals`):

```
resolve typedef ProposalVotingT -> ProposalVoting<ProposersAndVotersT, ProposalDataT>
  resolve args through class-scope typedefs: H = ProposalAndVotingByComputors<676>, D = ProposalDataV1<false>
instantiate ProposalVoting<H,D>:
  maxProposals = eval(H::maxProposals)  -> instantiate H, find static constexpr maxProposals = proposalSlotCount = 676
  maxVotes     = eval(H::maxVotes)      -> 676 (NUMBER_OF_COMPUTORS macro)
  member proposersAndVoters : H          -> layout(H): id[676] -> size 21632 align 8
  member proposals : E[maxProposals], E = ProposalWithAllVoteData<D, maxVotes>
    choose specialization: D canonical == ProposalDataYesNo ? partial : primary   -> primary
    primary: base D (328), supportScalarVotes = eval(D::supportScalarVotes) = false
             VoteStorageType = __VoteStorageTypeSelector<false>::type -> primary -> uint8
             votes : uint8[676] at align_up(328, 1) = 328 -> size align8(1004) = 1008
  size = 21632 + 676*1008 = 703040
```

### 3.2 ABI remarks (MSVC x64 vs Itanium)

Production nodes are MSVC/UEFI builds; the oracle is g++. For these types both ABIs agree because (a) no base class has tail padding (`ProposalDataYesNo`/`ProposalDataV1` end exactly on their union, and the unions end on their largest alternative), so the Itanium rule "non-POD base tail padding may be reused" never fires; (b) `m256i` has alignment 8 in both; (c) only `long long`/fixed-width types are used (no `long`, which differs between the ABIs). Evidence: the g++-computed `sizeof(StateData)` equals the real file sizes for all three production contracts. A generic engine should nevertheless implement the MSVC rule (derived members start at `align_up(sizeof(Base), align(member))`, never inside base tail padding) because it is the producer's ABI; for the proposal types the rule is observationally equivalent.

---

## 4. C++ features a generic layout engine must implement (exhaustive for the proposal types)

Preprocessor:
1. `#pragma once` (every header), `#include "..."` relative to the including file (`qpi_proposals.h:3 "qpi_types.h"`, `qpi_types.h:4 "../platform/m256.h"`) AND relative to an include root (`qpi_proposals_impl.h:3 "qpi/qpi.h"`, `contract_def.h:14-22`).
2. Object-like macros used as template arguments and array extents: `NUMBER_OF_COMPUTORS` (`GeneralQuorumProposal.h:17`, `qpi_proposals_impl.h:15,137,339`, `qpi_proposals.h:446`), `QUORUM` in default function arguments (`qpi_proposals.h:109`). Benign identical redefinition of `NUMBER_OF_COMPUTORS`/`QUORUM` (`qpi_types.h:28-29` vs `network_messages/common_def.h:6-7`).
3. Function-like multi-line macros with `\` continuation and parameters substituted into template-ids: `DEFINE_SHAREHOLDER_PROPOSAL_TYPES` (`qpi_macros.h:441-445`), use `QUtil.h:137`. The other `IMPLEMENT_*` macros expand to nested structs/typedefs/functions inside the contract struct (`qpi_macros.h:447-557`) and must at least be expanded and skipped correctly (`#` stringizing and `##` pasting appear in `NO_IO_SYSTEM_PROC*` `:63-74`).
4. Conditional compilation inside a class/function body: `#ifndef NDEBUG ... #endif` (`qpi_proposals_impl.h:200-214`), `#ifdef INCLUDE_CONTRACT_TEST_EXAMPLES` around the test contracts (`contract_def.h:322-356,438-443`).
5. Per-contract `#define CONTRACT_INDEX ... #undef` protocol (`contract_def.h:24-32` etc.); `CONTRACT_INDEX` feeds `ContractState<..., CONTRACT_INDEX>` in macro-generated function signatures (skip-able).

Declarations / scoping:
6. `namespace QPI { ... }`, nested namespaces with constants (`qpi_proposals.h:137-234`), stray `;` after a namespace's closing brace (`:156`, `:234`) and after a member function body (`qpi_proposals_impl.h:143`).
7. `using namespace QPI;` at file scope of every contract (`GeneralQuorumProposal.h:1`, `ComputorControlledFund.h:1`, `QUtil.h:1`); contracts live in the global namespace.
8. Namespace-scope `constexpr` variables of typedef'd integer types with hex/decimal literals, including out-of-range-for-signed literals (`qpi_proposals.h:7-9`) and 64-bit values (`QUtil.h:19` `327647778129`, `TestExampleA.h:3`); `static constexpr` namespace-scope constants initialised by expressions over other constants (`qpi_proposals.h:162-211`, `Class::X | n`).
9. Typedef chains across scopes: file-scope `using`, contract-class typedefs (`GeneralQuorumProposal.h:13-20`), nested `StateData` referring to enclosing-class typedefs (`:36-39`), template-parameter aliases inside templates (`qpi_proposals.h:479-484`), nested typedef of a specialization (`qpi_proposals_impl.h:371,373,383`). Template arguments must be **canonicalized through typedefs** before matching specializations (CCF passes `ProposalDataT` = `ProposalDataYesNo` → must select the partial specialization).
10. Data member named `type` (`qpi_proposals.h:249,376`) coexisting with a function `ProposalTypes::type()` (`:214`) and a nested typedef `::type` (`qpi_proposals_impl.h:371`) — plain identifiers, no keyword handling.
11. Access specifiers `public:`/`protected:`/`private:` in `struct` and `class` (`qpi_proposals.h:475,492`; `qpi_containers.h:118,125`; macro-injected `public:` `qpi_macros.h:442`), `friend struct X<...>;` (`qpi_proposals.h:497-498`), class-scope `static_assert` with and without message (`:486-487`, `qpi_containers.h:119`), namespace-scope `static_assert(sizeof(TemplateId<arg>) == expr, "msg")` (`qpi_proposals.h:363`).
12. Member functions with inline bodies to skip by brace matching (containing `switch/case`, `for`, `goto`/labels `qpi_proposals_impl.h:727,752`, `reinterpret_cast`/`static_cast`/`const_cast`, braced init-lists `{ NULL_ID, contractAssetName }` `:142,165`, local struct definitions `:154-158`, local template variables `QPI::HashMap<sint64, uint32, 16> valueIdx;` `:832`, `auto&` `:643`), default arguments in parameter lists (`qpi_proposals.h:109,526,530,535,540`; `qpi_proposals_impl.h:81,96,105`), `const` member functions, `inline static`/`static constexpr` functions (`qpi_proposals.h:214,221,227,233`), `= default` constructors (`:120,352,426`), user-provided copy ctor/`operator=` using the injected-class-name with template arguments `ProposalDataV1<SupportScalarVotes>&` (`:353-361`), constructors with mem-initializer lists (`:551-555`, `:615-619`), member `operator()` templates (`qpi_context.h:243-246,379-382`).
13. Out-of-line definitions to skip: member functions of class templates `template <typename A, typename B> R C<A,B>::f(...) {...}` (`qpi_proposals_impl.h:547-1104`), namespace-member function `inline bool ProposalTypes::isValid(uint16) {...}` (`:343`), free function templates incl. overloads (`:884-956`), member templates of non-template classes (`:1107-1122`).
14. Reference data members (`qpi_proposals.h:557-558`) — only in non-state types; if laid out, pointer-sized.

Templates:
15. Class templates with non-type parameters of type `bool` (`qpi_proposals.h:239`, `qpi_proposals_impl.h:370`), `uint16` (`impl:8,117,130`), `uint32` (`impl:378,481`), `uint64` (`impl:130`; `Array` `qpi_containers.h:115`) and with type parameters (`qpi_proposals.h:472`); parameter names that differ between declaration and definition (`numOfVoters` `:441` vs `numOfVotes` `impl:378`).
16. Default template argument present only on a forward declaration, definition later without it (`qpi_proposals.h:446-447` vs `impl:8`) → merge across redeclarations; redeclaration/forward declaration of templates in general (`:441-461`), with the definition appearing in a different header included later. Instantiation must be lazy (at point of use in `StateData`, after all headers are parsed): `ProposalVoting` (`qpi_proposals.h:472`) refers to `ProposalWithAllVoteData`, which is still incomplete at that line.
17. Template arguments of all kinds: bool literal (`ProposalDataV1<false>` `GeneralQuorumProposal.h:13`), integer literal (`ProposalAndVotingByComputors<100>` `ComputorControlledFund.h:19`; `Array<uint8, 256>`), macro (`<NUMBER_OF_COMPUTORS>`), namespace-scope constexpr variable (`<8, QUTIL_CONTRACT_ASSET_NAME>`), static constexpr member of the current instantiation (`<ProposalDataT, maxVotes>` `qpi_proposals.h:481-484`, spanning lines), typedef names (`<ProposersAndVotersT, ProposalDataT>`), template-ids nested as arguments (through typedefs, e.g. `ProposalVoting<ProposalAndVotingByShareholders<8, …>, ProposalDataYesNo>`), implicit conversion of the argument to the parameter type (`int`/`uint32` 676 → `uint16`/`uint32`; `uint64 L`).
18. Dependent qualified names for values and types: `ProposerAndVoterHandlingT::maxProposals` (`qpi_proposals.h:476-477`), `ProposalDataType::supportScalarVotes` (`impl:382`), `__VoteStorageTypeSelector<supportScalarVotes>::type` **without `typename`** (`impl:383`, C++20 P0634). Lookup of such static members must search base classes of the (instantiated) argument type (`ProposalByAnyoneVotingByComputors<N>` inherits `maxProposals`/`maxVotes`).
19. Inheritance: from a template **type parameter** (`struct ProposalWithAllVoteData : public ProposalDataType` `impl:379`), from a dependent template-id (`ProposalByAnyoneVotingByComputors<N> : public ProposalAndVotingByComputors<proposalSlotCount>` `impl:118`), from a concrete class in a partial specialization (`: public ProposalDataYesNo` `impl:482`), from a template-id in non-state helpers (`qpi_proposals.h:563`), and from the empty `ContractBase` by every contract (`GeneralQuorumProposal.h:7`; irrelevant for `StateData`, which is a nested struct). Derived members are laid out after the complete base (§3.2).
20. Explicit (full) specialization `template <> struct __VoteStorageTypeSelector<true>` (`impl:372-373`) and partial specialization on a type argument with the NTTP left open `template <uint32 numOfVotes> struct ProposalWithAllVoteData<ProposalDataYesNo, numOfVotes>` (`impl:481-482`); selection by canonical type identity of the first argument.
21. Static constexpr data members of class templates whose initializers depend on NTTPs, macros or other static members, declared **after** their first use in the class body (`qpi_proposals.h:350` is used by `checkValidity` `:339` and by `impl:382`) → collect the whole class body first / evaluate lazily. Static constexpr members inside nested structs inside unions (`qpi_proposals.h:287-288`) must be ignored for layout.
22. Nested types of class templates (`ProposalDataV1<b>::Data::Transfer` etc.) and nested member typedefs (`ProposalVoting<…>::ProposalAndVotesDataType`).
23. `>>` closing two template argument lists (`test/qpi.cpp`, and in general) — not on the state path of these contracts (they go through typedefs) but required generally.

Aggregates and expressions:
24. Named union type defined inline with a declarator: `union Data { ... } data;` (`qpi_proposals.h:255-290, 382-397`), nested struct types defined inline with declarators inside that union (`:258-289`); anonymous union member (`:80-87`); unnamed-struct members with declarators inside `union m256i` (`platform/m256.h:22-55`); union size = max alternative, alignment = max alignment.
25. Arrays: 1-D with extent = NTTP (`impl:386`), = static constexpr member (`impl:114,338`, `qpi_proposals.h:494`), = macro (`impl:339`), = arithmetic expression `(2 * numOfVotes + 7) / 8` (`impl:485`: `*`, `+`, `/` with truncating integer division, parentheses, uint32 arithmetic), 2-D arrays `id a[N][676]` (`impl:339`); `Array<T, L>` wrapper with `T _values[L]` (`qpi_containers.h:123`).
26. Constant-expression operators needed overall: `|` (`qpi_proposals.h:162-211`), `& ~ <<` only in function bodies (skip), `* / +` (above; `QUORUM` macro `qpi_types.h:29` `676 * 2 / 3 + 1`), comparisons `<=` in static_asserts; `sizeof(T)` and `sizeof(TemplateId<arg>)` inside namespace-scope static_asserts (`qpi_proposals.h:29,58,131,363,437`) — optional for layout, recommended as self-test. No `sizeof` is used in any array extent or template argument of the proposal types (the `sizeof(Shareholder) * maxVotes` / `sizeof(id) * maxVotes` uses are inside function bodies `impl:159,262`). No `constexpr` function call is on the layout path of the proposal types (the QPI `div()` constexpr template `qpi.h:53-57` feeds `QUTIL_MAX_NEW_POLL` `QUtil.h:39`, which is not a state extent).
27. Bit-fields, `alignas`, `#pragma pack`, enums as extents, `std::conditional`/`decltype`: **not used** by the proposal types (grep over `qpi/` and `contracts/`: none).

---

## 5. Per-contract instantiations and cross-checks

All numbers from the g++ oracle (`p02-work/oracle-HEAD/oracle.out`; identical for v1.303.2) and `stat` of the sample files.

### 5.1 GQMPROP (contract 6, `contracts/GeneralQuorumProposal.h`)

```cpp
typedef ProposalDataV1<false> ProposalDataT;                                   // :13
typedef ProposalAndVotingByComputors<NUMBER_OF_COMPUTORS> ProposersAndVotersT;  // :17  -> <676>
typedef ProposalVoting<ProposersAndVotersT, ProposalDataT> ProposalVotingT;      // :20
struct RevenueDonationEntry { id destinationPublicKey; sint64 millionthAmount; uint16 firstEpoch; }; // :27-32 -> 48 bytes
typedef Array<RevenueDonationEntry, 128> RevenueDonationT;                      // :34
struct StateData { ProposalVotingT proposals; RevenueDonationT revenueDonation; }; // :36-51
```

| item | value |
|---|---|
| `ProposalVoting<ProposalAndVotingByComputors<676>, ProposalDataV1<false>>` | 703 040 = 21 632 + 676 × 1008 |
| `proposersAndVoters` (id[676]) | @0, 21 632 |
| `proposals[676]` | @21 632, element 1008 (votes @328, 676 bytes, 4 pad) |
| `revenueDonation` | @703 040, 6 144 = 128 × 48 (destinationPublicKey @0, millionthAmount @32, firstEpoch @40) |
| `sizeof(GQMPROP::StateData)` | **709 184** = `contract0006.229` (709 184 bytes) ✓ |

### 5.2 CCF (contract 8, `contracts/ComputorControlledFund.h`)

```cpp
typedef ProposalDataYesNo ProposalDataT;                                   // :16
typedef ProposalAndVotingByComputors<100> ProposersAndVotersT;              // :19
typedef ProposalVoting<ProposersAndVotersT, ProposalDataT> ProposalVotingT;  // :22
struct StateData {                                                          // :89-106
    ProposalVotingT proposals;                       // @0        51 200 = 3 200 + 100 × 480
    LatestTransfersT latestTransfers;                // @51 200   38 912 = 128 × 304  (LatestTransfersEntry: id, Array<uint8,256> url, sint64 amount, uint32 tick, bit success)
    uint8 lastTransfersNextOverwriteIdx;             // @90 112   1 (+3 pad)
    uint32 setProposalFee;                           // @90 116   4
    RegularPaymentsT regularPayments;                // @90 120   39 936 = 128 × 312
    SubscriptionProposalsT subscriptionProposals;    // @130 056  44 032 = 128 × 344
    ActiveSubscriptionsT activeSubscriptions;        // @174 088  319 488 = 1024 × 312   (CCF_MAX_SUBSCRIPTIONS = 1024, :3)
    uint8 lastRegularPaymentsNextOverwriteIdx;       // @493 576  1 (+7 pad)
};                                                   // sizeof = 493 584 = contract0008.229 (493 584 bytes) ✓
```

Element: `ProposalWithAllVoteData<ProposalDataYesNo, 676>` = 480 (votes @304, 169 bytes, 7 pad).

### 5.3 QUTIL (contract 4, `contracts/QUtil.h`)

`DEFINE_SHAREHOLDER_PROPOSAL_TYPES(8, QUTIL_CONTRACT_ASSET_NAME);` (`:137`, expansion in §2.13; `QUTIL_CONTRACT_ASSET_NAME = 327647778129` `:19`) → `ProposalVoting<ProposalAndVotingByShareholders<8, 327647778129>, ProposalDataYesNo>`; `ProposalVotingT proposals;` is the **last** `StateData` member (`:176`).

| item | value |
|---|---|
| `proposersAndVoters` (`ProposalAndVotingByShareholders<8,…>`) | 173 312 = 8×32 (proposers @0) + 8×676×32 (shareholders @256; row i @256 + i×21 632) |
| `proposals[8]` | @173 312, element 480 → 3 840 |
| `sizeof(ProposalVotingT)` | 177 152 = 8 × 22 144 |
| `offsetof(QUTIL::StateData, proposals)` | 402 717 952 (= file size − 177 152); preceding fee block: `smt1InvocationFee` @402 717 864, `shareholderProposalFee` @402 717 896, `_futureFeePlaceholder0` @402 717 904 (6 × 8) |
| `sizeof(QUTIL::StateData)` | **402 895 104** = `contract0004.229` ✓ (arithmetic: 88 + 47 104 + 402 653 184 + 512 + 512 + 16 384 + 16 + 64 + 40 + 48 + 177 152) |

### 5.4 TESTEXA / TESTEXB (test-only, indices after the last production contract, `contract_def.h:324,332`)

* TESTEXA (`TestExampleA.h:56-62,101`): `ProposalVoting<ProposalAndVotingByShareholders<16, 18392928276923732>, ProposalDataYesNo>` = 354 304 (= 346 624 + 16 × 480); `offsetof(StateData, proposals)` = 8 416; `Array<MultiVariablesProposalExtraData, 16> multiVariablesProposalData` @362 720 (384); `sizeof(StateData)` = 363 104.
* TESTEXB (`TestExampleB.h:25-31,63`): `ProposalVoting<ProposalAndVotingByShareholders<16, 18674403253634388>, ProposalDataV1<true>>` = 438 400 (= 346 624 + 16 × 5 736); `offsetof(StateData, proposals)` = 624; `sizeof(StateData)` = 439 024.

### 5.5 Real-data decode evidence (`scripts/p02_decode_proposals.py samples`)

* GQMPROP: 41 of 676 slots used (epochs 128…227, ticks 16 141 998…75 869 485, all < epoch-229 start tick 77 700 000), types 39 × `0x0002` and 2 × `0x0003`, URLs such as `https://github.com/qubic/proposal/blob/main/SmartContracts/2026-07-02-upgrade_qraffle.md`; vote bytes in used slots take only values {0, 1, 2, 0xff}; unused slots are all-zero. `revenueDonation` decodes to `id(7,0,0,0)` 775 000 ppm from epoch 227, `id(8,0,0,0)` 80 000 ppm from 129, `id(9,0,0,0)` 122 500 ppm from 137, then `NULL_ID` (contract indices SWATCH, CCF, QEARN). Example: slot 1: proposer `ZFPYXZJQ…HDRG`, epoch 220, tick 63905772, casted 648/676, opt0=165, opt1=483 → accepted option 1.
* CCF: 73 of 100 slots used, all `0x0102 TransferYesNo`, e.g. slot 0: epoch 135, tick 17181987, destination `XQCLNHCE…EONG`, amount 180 000 000 000, 535 votes casted (76 no / 459 yes); 2-bit vote values in used slots ∈ {0,1,3}; `setProposalFee = 1 000 000` (= `INITIALIZE`, `ComputorControlledFund.h:457`), `lastTransfersNextOverwriteIdx = 74`; **42 of 74 `latestTransfers` log entries match a stored proposal exactly on (url, destination, amount)** (the rest belong to overwritten slots).
* QUTIL: the 177 152-byte `proposals` region is entirely zero (no shareholder proposal ever made); the fee block right before it decodes to `[10, 10000000, 100, 5, 100]` = the initial values (`QUtil.h:26,37,36,63`, `:1806`), confirming the offset.

### 5.6 Synthetic fixtures (real-code generated, `p02-work/fixtures/`)

| fixture | type | size | content |
|---|---|---|---|
| `fixtureA_shareholders8_yesno.bin` | QUTIL layout | 177 152 | slot 1 VariableYesNo with 5 shareholders (300/200/100/75/1 shares, split votes) → hist 125/451 accepted 1; slot 5 MultiVariablesThreeOptions (2-bit value 2 exercised); slot 7 TransferYesNo, no votes |
| `fixtureB_shareholders16_v1scalar.bin` | TESTEXB layout | 438 400 | slot 0 scalar (regular mean path, result 263510), slot 2 scalar with full range (overflow-safe path; result 873793140343346800 ≠ naive truncated mean 873793140343346799), slot 3 VariableFourValues, slot 4 eight options, slot 9 TransferInEpoch (accepted 1), slot 15 TransferThreeAmounts |
| `fixtureC_anyone200_v1.bin` | `ProposalVoting<ProposalByAnyoneVotingByComputors<200>, ProposalDataV1<false>>` | 208 000 | slots 0, 100, 199 (yes/no, TransferFourAmounts, 8 options) |

`p02_decode_proposals.py blob --canonical` output is byte-identical to the `*.expected.txt` written by the real core functions for all three.

---

## 6. Semantics for the viewer

Legend: **[E]** essential for a readable decoded view, **[N]** nice-to-have.

### 6.1 Slot occupancy and status [E]

* Slot `i` holds a proposal iff `proposals[i].epoch != 0` (`getProposal` `impl:768`, `nextProposalIndex` `:1075`). A free slot is all-zero (cleared by `setMemory(pv.proposals[i], 0)` `impl:625` and never-used memory is zero-initialised); its proposer entry is `NULL_ID` (`freeProposalByIndex` `impl:51-55, 257-264`, which also zeroes the shareholder row). **Never interpret the vote bytes of a free slot**: zeros would read as "676 votes for option 0" (V1) or "676 votes for option 0" (YesNo 2-bit 00). Real files: 0 free slots with residue.
* Proposer of slot `i` = `proposersAndVoters.currentProposalProposers[i]` (`impl:58-63, 267-272`; `proposerId()` returns `NULL_ID` when epoch == 0 `impl:1021`).
* Status relative to the state file's epoch `EEE` (file name `contractNNNN.EEE`): `epoch == EEE` → active / open for voting (`vote()` requires `qpi.epoch() == proposal.epoch` `impl:646`); `0 < epoch < EEE` → finished (`nextFinishedProposalIndex` `impl:1097-1098`). In the epoch-229 samples the newest proposals are from epoch 227, so everything shows as finished.
* `tick` = tick at which the proposal was (last) set (`impl:601`); `epoch` is overwritten with the current epoch at set time (`impl:602`). A proposer re-setting a proposal reuses his slot and discards all votes (`getExistingProposalIndex` `impl:30-35`; `set()` resets votes `impl:396-409`).

### 6.2 `type` field and active union alternative [E]

`cls = type & 0xff00`, `options = type & 0x00ff` (`qpi_proposals.h:221-230`). Display the named constant when one matches (§2.3). Union alternative per class:

| class | V1 alternative | YesNo alternative | notes |
|---|---|---|---|
| GeneralOptions 0x000 | none (data unused, normally zero; not validated) | none | options 2..8 (V1) / 2..3 (YesNo) |
| Transfer 0x100 | `data.transfer` {destination, amounts[4]}; options−1 amounts used, ascending, non-negative, rest 0 (`checkValidity` `:306-324`) | `data.transfer` {destination, amount} | option k ≥ 1 = amounts[k−1] / the amount |
| Variable 0x200, options ≥ 2 | `data.variableOptions` {variable, values[4]}; options−1 values used | `data.variableOptions` {variable, value} | `variable` is a contract-defined index |
| Variable 0x200, options == 0 | `data.variableScalar` {variable, minValue, maxValue, proposedValue} — **only storable with `ProposalDataV1<true>`** (`set()` `impl:391`, `checkValidity` `:336-343`) | impossible | votes are sint64 values in [min, max] |
| MultiVariables 0x300 | none (option data stored by the contract elsewhere, e.g. TESTEXA `multiVariablesProposalData[slot]` `TestExampleA.h:104`) | none | |
| TransferInEpoch 0x400 | `data.transferInEpoch` {destination, amount, targetEpoch} — V1 only (YesNo `checkValidity` has no case `:407-419`) | impossible | options == 2 |

If `data` is non-zero for a class that does not use it, show the raw hex (it is copied verbatim from the proposer's input).

### 6.3 Vote storage [E]

`proposals[i].votes` has `maxVotes` = 676 entries; entry `v` is the vote of vote index `v`:

| storage | per-vote decode (`getVoteValue` `impl:456-476`, `:531-544`) | "no vote" | reset value on `set()` |
|---|---|---|---|
| `ProposalDataYesNo` | `(votes[v >> 2] >> ((v & 3) * 2)) & 3`; 3 = no vote, else the option (0..2) | 3 | all bytes 0xff |
| `ProposalDataV1<false>` | `votes[v]`; 0xff = no vote, else option 0..7 | 0xff | 0xff |
| `ProposalDataV1<true>` | `sint64 votes[v]`; `NO_VOTE_VALUE` = no vote; else option index (option proposals) or scalar value (`VariableScalarMean`) | 0x8000000000000000 | NO_VOTE_VALUE |

Option semantics: 0 = "no" / "no change" (status quo), k ≥ 1 = k-th proposed option (`qpi_proposals.h:25-26`; doc `contracts_proposals.md:15`). `setVoteValue` only stores `0 <= value < optionCount` (`impl:443-448`), so values ≥ optionCount cannot appear (but a tolerant viewer should still guard).

### 6.4 Voter ↔ identity mapping

* Computor voting (`ProposalAndVotingByComputors`, `ProposalByAnyoneVotingByComputors`): vote index == computor index 0..675 in the computor list of the epoch in which the vote was cast (`getVoteIndex` loops `qpi.computor(compIdx)` `impl:87-91`; `qpi.computor(i)` = `broadcastedComputors.computors.publicKeys[i % 676]` `qpi/impl/qpi_trivial_impl.h:95-98`); one vote per computor (`getVoteCount` = 1 `impl:96-102`). **The state file does not contain the computor list**, so [E] show "computor #v", [N] resolve to an identity with an externally supplied computor list for `proposal.epoch` (the list changes every epoch; votes in finished proposals refer to that proposal's epoch).
* Shareholder voting (`ProposalAndVotingByShareholders`): `currentProposalShareholders[i][v]` is the id entitled to vote index `v` of slot `i`, snapshotted when the proposal was created/overwritten (`setupNewProposal` `impl:146-228`): one entry per share, 676 entries total, holders sorted ascending by `m256i operator<` (compare `u64[0]`, then `[1]`, `[2]`, `[3]` `platform/m256.h:295-306`), equal ids consecutive. A holder's votes = the run of equal ids (`getVoteIndex` = first index of the run `impl:290-306`, `getVoteCount` = run length `impl:309-325`, `getVoterId(v)` = entry `impl:328-333`). [E] show per holder: identity, number of shares/votes, and the distribution of its vote values (this is what `getVotes()` returns `impl:800-881`: histogram of the run's values; for scalar proposals up to 8 distinct values). A free slot's row is all zero.
* Identity rendering (`four_q.h:1777-1803`): for each of the 4 little-endian uint64 limbs emit 14 base-26 letters (least significant first, 'A' + digit), then a 4-letter checksum = `KangarooTwelve(publicKey, 32) → first 3 bytes LE & 0x3FFFF` in base 26 (implemented and verified in the script against `ARBITRATOR`, `public_settings.h:84`). Ids of the form `id(n,0,0,0)` denote contract `n` (e.g. `SELF`, `qpi_macros.h:435`; GQMPROP revenue donation destinations).

### 6.5 Summaries and acceptance [E for histogram/casted; N for exact mean & per-contract rules]

`getVotingSummary` (`impl:959-1004`): `optionCount = type & 0xff`; option proposals: `totalVotesCasted` = number of votes with `0 <= value < optionCount`, `optionVoteCount[k]` = histogram (8 entries); `totalVotesAuthorized = maxVotes` (676). Scalar (`type == 0x0200`, `__getVotingSummaryScalarVotes` `impl:884-946`): mean of all non-NO_VOTE values with C++ truncating division; two code paths: if `maxValue > 0x7fffffffffffffff / 676` or `minValue < -0x7fffffffffffffff / 676` the overflow-safe form `Σ(v / n) + (Σ(v % n)) / n` is used, otherwise `(Σ v) / n`; the two can differ by 1 (fixture B slot 2) — replicate both to match on-chain results.

Library acceptance (`ProposalSummarizedVotingDataV1::getAcceptedOption(QUORUM, QUORUM/2)` `qpi_proposals.h:109-118`): requires `totalVotesCasted >= 451`; the most-voted option (first maximum, `:90-106`) is "accepted" if it has `> 225` votes; result 0 means the status quo won, −1 means no decision / no quorum. Contract rules on top: GQMPROP accepts Transfer/TransferInEpoch proposals of the previous epoch in `BEGIN_EPOCH` if casted ≥ QUORUM and most-voted option > 0 with > QUORUM/2 votes, writing `revenueDonation` (`GeneralQuorumProposal.h:387-434`); CCF in `END_EPOCH` requires casted ≥ QUORUM, `opt1 >= opt0`, `opt1 > QUORUM/2`, then transfers `amount` QU and logs to `latestTransfers` (`ComputorControlledFund.h:487-591`); QUTIL default macro applies `getAcceptedOption() > 0` to Variable/MultiVariables proposals and sets the fee selected by `data.variableOptions.variable` (`qpi_macros.h:532-546`, `QUtil.h:2082-2102`: 0 smt1InvocationFee, 1 pollCreationFee, 2 pollVoteFee, 3 distributeQuToShareholderFeePerShareholder, 4 shareholderProposalFee).

### 6.6 Contract-specific units [N]

* GQMPROP Transfer/TransferInEpoch amounts are **millionths of computor revenue** (0…1 000 000 = 0…100 %, `GeneralQuorumProposal.h:201-220`), destination usually a contract id; GeneralOptions proposals are plain yes/no (or 3-option) polls with the URL as the only payload.
* CCF Transfer amounts are **QU** (`qpi.transfer(destination, amount)` `ComputorControlledFund.h:586`); only class Transfer is allowed (`:164-173`); subscription proposals keep extra data in `subscriptionProposals[proposalIndex]` (`:102`).
* QUTIL: only `VariableYesNo` with `variable < 5`, `value >= 0` (`qpi_macros.h:451-453`, instantiated with `numFeeStateVariables = 5` `QUtil.h:2104`); `value` is a fee in QU.

### 6.7 Suggested decoded view (summary)

Per `ProposalVoting` member: table of used slots with columns index · status (active/finished + epoch) · proposer identity · type (name + class + options) · URL (link) · payload summary (destination/amount(s), variable/value(s), min/max/proposed) · casted/authorized · histogram or mean · accepted option; detail pane with the raw `data` hex, the vote list (computor index → value, or shareholder → shares and value distribution), and tail-padding bytes hidden. Free slots collapsed into "N free slots". Everything in §6.1-6.3 plus the histogram is essential; §6.4 identity resolution of computors, §6.5 exact scalar mean and contract rules, §6.6 units are nice-to-have.

---

## 7. How to reproduce

```bash
S=/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/research/scripts
W=/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/research/p02-work
# g++ oracle (real headers + real contracts); the only patch is one expression inside a function body
$S/p02_build_oracle.sh /home/yeti/devwork/space/core $W/oracle-HEAD          # -> oracle.out
$S/p02_build_oracle.sh /tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/core-v1.303.2 $W/oracle-v1.303.2
diff $W/oracle-HEAD/oracle.out $W/oracle-v1.303.2/oracle.out                 # empty
# decode the real files
python3 $S/p02_decode_proposals.py samples --contract gqmprop --limit 3 --votes
python3 $S/p02_decode_proposals.py samples --contract ccf
python3 $S/p02_decode_proposals.py samples --contract qutil
# fixtures from real code + decoder equivalence
$S/p02_build_fixtures.sh /home/yeti/devwork/space/core $W/fixtures
python3 $S/p02_decode_proposals.py blob --file $W/fixtures/fixtureB_shareholders16_v1scalar.bin \
   --handling shareholders --slots 16 --data V1true --canonical | diff - $W/fixtures/fixtureB_shareholders16_v1scalar.expected.txt
```

Oracle build notes (useful for any future compile-based test harness): `g++ -std=c++20 -mavx2 -mbmi2 -fno-access-control -Wno-invalid-offsetof -w -I<overlay> -I core/src -I core`, `#define NO_UEFI`, include `<cstdio>` and `platform/memory.h` first, then `contract_core/pre_qpi_def.h`, `qpi/qpi.h`, the impl header, `oracle_core/oracle_interfaces_def.h`, `oc_core/oc_interfaces_def.h`, then the contracts with the `CONTRACT_INDEX/CONTRACT_STATE_TYPE/CONTRACT_STATE2_TYPE` defines. g++ 13 rejects only `qpi_proposals_impl.h:971` (`pv.maxVotes` as a template argument through a reference member — P2280, fixed in GCC 14); the overlay replaces it by `ProposalVotingType::maxVotes`. The full `contract_def.h` does NOT compile with g++ (≈300 errors in other contracts/platform code), so a compile-based oracle must pick its contracts.

---

## 8. Open questions / risks

1. Which epoch does `contractNNNN.EEE` represent exactly (start-of-epoch snapshot vs. mid-epoch save)? It determines whether `epoch == EEE` proposals can appear as "active". The samples contain none; semantics in §6.1 are stated relative to EEE and are correct either way.
2. Computor lists per epoch are not in any file of `/home/yeti/devwork/space/229` (zip contents checked) — computor-vote identities need an external source (network / archive) or stay as indices.
3. Pre-v1.282 cores used `DEFINE_SHAREHOLDER_PROPOSAL_STORAGE`, which injects the `proposals` data member via macro into the contract struct (no `StateData`). Out of scope for v1.303.2/v1.306.0 but relevant if older epochs are ever loaded.
4. The MSVC-vs-Itanium tail-padding difference cannot show up with the current `ProposalDataT` types, but a future custom `ProposalDataT` with tail padding (allowed by the design) would — the engine must implement MSVC's rule, not the Itanium one.
