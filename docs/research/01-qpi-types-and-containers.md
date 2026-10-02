# 01 — QPI data types and containers: binary layout and logical semantics

Reference for the qstate-viewer layout engine and decoders. Everything below was read from the Qubic core source
(v1.306.0 "HEAD" at `/home/yeti/devwork/space/core`, and the v1.303.2 / epoch-229 snapshot) and validated
numerically against the real epoch-229 state files and against the real QPI container code compiled with g++.
File:line citations refer to HEAD unless marked `(v1.303.2)`; **all QPI headers listed in the task are byte-identical in
both versions** (see §8), so the line numbers are valid for both.

Validation scripts and captured outputs: `scratchpad/research/scripts/` (see `README-01-qpi-types.md` there and §6).

---

## 0. Executive summary

* A contract state file `contractNNNN.EEE` is the raw little-endian memory image of `X::StateData`
  (`contract_core/contract_def.h:406-437`), laid out by **MSVC x86-64** (the production node is built with Visual
  Studio for UEFI; the clang port "is not resulting in a working node", `README_CLANG.md:2`). For every type that can
  occur in a state, MSVC and the Itanium (g++/clang) layout coincide, with one latent exception (tail-padding reuse of
  non-POD base classes, §7) that no current contract triggers. So: natural alignment, no packing, max alignment 8.
* `m256i` (= `QPI::id`) is a **union with no `__m256i` member → sizeof 32, alignof 8** (`src/platform/m256.h:9-205`,
  verified by reading the whole file, by g++ (`alignof(m256i)=8`) and by the fact that `sizeof(QX::StateData)` only
  equals the 621 806 120-byte file with align 8).
* All QPI containers are plain structs of `T x[N]` arrays plus `uint64` counters; closed-form sizes (§3) reproduce
  `sizeof` of every g++-probed instantiation and the exact file sizes of 9 real epoch-229 state files
  (QX, QTRY, NOST, QBOND, QIP, ESCROW, GGWP, contract0, IPO-shaped files; §6.1).
* Decode algorithms (§4) were run on the real files (179/179 checks, §6.2) and on a synthetic image produced by the
  *real* container code including LinkedList, tombstones, `cleanup()`, BST rebuild and zero-state lists (56/56, §6.3).
* Identity encoding (§5): 56 base-26 letters from the four little-endian `uint64`s (least significant digit first) +
  4 letters from `K12(pubkey,32 bytes) → 3 bytes & 0x3FFFF`. The core's `KangarooTwelve()` is **standard KT128 with
  empty customization**; a pure-Python K12 reproduces 27 core-generated digests and 40 core-generated identities,
  plus the official K12 vectors. `NULL_ID` = `AAAA…AAAAFXIB`, QX (contract 1) = `BAAA…AAAARMID`.

---

## 1. Primitive and fixed-layout types

### 1.1 Integer typedefs (`src/qpi/qpi_types.h:11-18`, `lib/platform_common/qstdint.h`)

| QPI name | C++ type | size | align | notes |
|---|---|---|---|---|
| `sint8` | `signed char` | 1 | 1 | |
| `uint8` | `unsigned char` | 1 | 1 | |
| `sint16` | `signed short` | 2 | 2 | |
| `uint16` | `unsigned short` | 2 | 2 | |
| `sint32` | `signed int` | 4 | 4 | |
| `uint32` | `unsigned int` | 4 | 4 | |
| `sint64` | `signed long long` | 8 | 8 | |
| `uint64` | `unsigned long long` | 8 | 8 | |
| `bool` | | 1 | 1 | stored 0/1; any non-zero is true |
| `char` | | 1 | 1 | |
| `int8_t … uint64_t` | `qstdint.h` | 1..8 | =size | On MSVC `__int8…__int64`; on Linux `<cstdint>` (`uint64_t` = `unsigned long`, which is why `uint64_t`/`QPI::uint64` are *different* types for template deduction under g++, `qstdint.h:6-14`). Layout identical. |
| `long` | | **4 on MSVC / 8 on Linux** | | not used in any contract state (grep, §7); the layout engine must treat `long` as 4 bytes if it ever appears |
| `wchar_t` | | 2 MSVC / 4 Linux | | not used in state |

All multi-byte values are little-endian.

### 1.2 `m256i` / `QPI::id` (`src/platform/m256.h`)

```
union m256i {                       // m256.h:9
    int8_t  m256i_i8[32]; int16_t m256i_i16[16]; int32_t m256i_i32[8]; int64_t m256i_i64[4];   // :12-15
    uint8_t m256i_u8[32]; uint16_t m256i_u16[16]; uint32_t m256i_u32[8]; uint64_t m256i_u64[4]; // :16-19
    struct { uint64_t _0,_1,_2,_3; } u64;  struct { int64_t _0.._3; } i64;                       // :22-29
    struct { uint32_t _0.._7; } u32;       struct { int32_t _0.._7; } i32;                        // :30-37
    struct { uint16_t _0.._15; } u16;      struct { int16_t _0.._15; } i16;                       // :38-45
    struct { uint8_t _0.._31; } u8;        struct { int8_t _0.._31; } i8;                         // :46-55
    // constructors / assign / getIntrinsicValue use _mm256_storeu/loadu (unaligned) – no __m256i member  :57-204
};
static_assert(sizeof(m256i) == 32);         // :207
typedef m256i id;                           // qpi_types.h:62
```
* **sizeof 32, alignof 8.** No member has alignment > 8, there is no `alignas`/`__declspec(align)`; all intrinsic access
  goes through `_mm256_storeu_si256`/`_mm256_loadu_si256` (unaligned) precisely because the union is only 8-aligned.
  g++ probe: `id size=32 align=8` (`evidence/gpp_probe_qpi.out`).
* `m256i(ull0, ull1, ull2, ull3)` (`m256.h:59-63`) = `_mm256_set_epi64x(ull3, ull2, ull1, ull0)` → `ull0` is bytes 0-7
  (`u64._0`). Hence `id(contractIndex, 0, 0, 0)` is `contractIndex` as LE uint64 in bytes 0-7, bytes 8-31 zero
  (`qpi_context.h:37`, `qpi_macros.h:435 #define SELF id(CONTRACT_INDEX, 0, 0, 0)`).
* `NULL_ID` = `id::zero()` = 32 zero bytes (`qpi_types.h:22`, `m256.h:201-204`).
* An `id` is **not always a public key**: contracts build composite keys in it, e.g. QX `_assetOrders` PoV =
  issuer id with `u64._3` replaced by the asset name (`contracts/Qx.h:290-291`); QUOTTERY `mABOrders` PoV has
  `u64._3 = eid | option/bid bits`. The viewer must offer hex / 4×uint64 display next to the identity rendering.

### 1.3 `uint128_t` / `QPI::uint128` (`src/platform/uint128.h:26-29`, MIT-licensed, calccrypto)

`class uint128_t { public: uint64_t low; uint64_t high; … }` → **16 bytes, align 8, `low` at 0, `high` at 8**
(verified by g++ and synthetic test; note the constructor argument order is `uint128_t(high, low)`, `uint128.h:36`).
Value = `high << 64 | low`.

### 1.4 `QPI::bit` (`qpi_types.h:34-46`)

`struct bit { char charValue; }` → 1 byte, align 1. Constructor maps any `bool` to 0/1; `operator bool` is `!!charValue`.
Decode: `charValue != 0` → true (show raw byte if it is neither 0 nor 1).

### 1.5 Enumerations

* `enum InterContractCallError : uint8` (`qpi_types.h:52-59`) → 1 byte. Values 0..4 (NoCallError, ContractInErrorState,
  InsufficientFees, AllocationFailed, ContractInactive).
* Contracts use `enum class EState : uint8` / `enum EState : uint8` members in state (e.g. `contracts/Pulse.h:74,220`,
  `QDuel.h:22`, `QThirtyFour.h:94`): size = the explicit underlying type. An enum **without** an explicit underlying
  type is `int` (4 bytes, align 4) on both MSVC and GCC (g++ probe `E_default size=4`, `E_cls size=4`,
  `E_cls16 size=2`, `E_u8 size=1`). Scoped vs unscoped makes no layout difference.

### 1.6 `DateAndTime` (`src/qpi/qpi_date_time.h:15-652`)

`struct DateAndTime { … protected: uint64 value; … }` → **8 bytes, align 8**, one `uint64` (`:610`). All other members
are functions / `static`. Bit packing (`set()` `:56-61`, getters `:107-153`, comment `:600-609`):

| bits (LSB=0) | width | field | valid range |
|---|---|---|---|
| 62-63 | 2 | reserved/padding | 0 |
| 46-61 | 16 | year (full, e.g. 2026) | 0..65535 |
| 42-45 | 4 | month | 1..12 |
| 37-41 | 5 | day | 1..28/29/30/31 |
| 32-36 | 5 | hour | 0..23 |
| 26-31 | 6 | minute | 0..59 |
| 20-25 | 6 | second | 0..59 |
| 10-19 | 10 | millisecond | 0..999 |
| 0-9 | 10 | microsecond within the millisecond | 0..999 |

`value == 0` means "no valid date / unset" (`DateAndTime()` ctor `:21-24`, `isValid()` `:156-161`);
`setInvalid(false)` stores `UINT64_MAX` (`:73-76`). `now()` stores `etalonTick.year + 2000`
(`impl/qpi_ticking_impl.h:50-54`), so the year field is the full Gregorian year. Comparison is plain `uint64`
comparison (`:219-252`), which is why the field order is year-first. Leap years: Gregorian (`isLeapYear` `:164-176`),
no leap seconds. See §4.7 for the decode pseudocode.

### 1.7 `Asset`, `Entity`, selectors (`qpi_types.h:66-81`, `qpi_assets.h`)

```
struct Entity { id publicKey; sint64 incomingAmount, outgoingAmount;             // 0, 32, 40
                uint32 numberOfIncomingTransfers, numberOfOutgoingTransfers;     // 48, 52
                uint32 latestIncomingTransferTick, latestOutgoingTransferTick; } // 56, 60   -> 64 bytes, align 8
struct Asset  { id issuer; uint64 assetName; }                                   // 0, 32    -> 40 bytes, align 8
```
`assetName` is a `uint64` holding up to 7 ASCII characters, little-endian, NUL-padded: text = bytes
`[(v >> 8*i) & 0xFF for i in 0..6]` up to the first 0 byte (the 8th byte is always 0; `test/test_util.h:46-60`
`assetNameFromString`/`assetNameFromInt64`; e.g. `"QUTIL"` = 327647778129, `"QX"` = 22609, `"GGWP"` = 1347897159).
Contract shares are issued with `issuer = NULL_ID` and `assetName` = the `contractDescriptions[].assetName`.

Selector structs are I/O helpers (not seen in states) but are legal POD members:
`AssetIssuanceSelect : public Asset { bool anyIssuer; bool anyName; }` → 48 bytes (`qpi_assets.h:7-11`; base has no
tail padding, so MSVC == Itanium); `AssetOwnershipSelect { id owner; uint16 managingContract; bool anyOwner;
bool anyManagingContract; }` → 40 bytes (`:28-33`); `AssetPossessionSelect` identical shape (`:51-56`).
The iterator classes (`:76-215`) contain `unsigned int` indices and selectors; they are not meant for state.

### 1.8 Other small QPI structs

| type | source | layout | size |
|---|---|---|---|
| `NoData` | `qpi_types.h:113` | empty struct | 1 (align 1) |
| `ContractBase::StateData` | `qpi_types.h:121` | empty | 1 |
| `ContractState<T, idx>` | `qpi_types.h:101-108` | single private member `T _data` at offset 0, `static constexpr __contract_index` | `sizeof(T)` — the file holds `T` directly |
| `OracleNotificationInput<OI>` | `qpi_context.h:265-273` | `sint64 queryId; sint32 subscriptionId; uint8 status; uint8 __reserved0; uint16 __reserved1; OI::OracleReply reply;` | 16 + sizeof(reply) (reply at 16 if its align ≤ 8) |
| `PreManagementRightsTransfer_input` | `qpi_macros.h:10-18` | `Asset asset; id owner; id possessor; sint64 numberOfShares; sint64 offeredFee; uint16 otherContractIndex;` | 128 |
| `PreManagementRightsTransfer_output` | `:21-25` | `bool allowTransfer; sint64 requestedFee;` | 16 (fee at 8) |
| `PostManagementRightsTransfer_input` | `:28-36` | like Pre…input with `receivedFee` | 128 |
| `PostIncomingTransfer_input` | `:39-44` | `id sourceId; sint64 amount; uint8 type;` | 48 |
| `ProposalSingleVoteDataV1` | `qpi_proposals.h:13-29` | `uint16 proposalIndex; uint16 proposalType; uint32 proposalTick; sint64 voteValue;` | 16 (static_assert) |
| `ProposalMultiVoteDataV1` | `:37-58` | `uint16, uint16, uint32, Array<sint64,8> voteValues, Array<uint32,8> voteCounts` | 104 (static_assert) |
| `ProposalSummarizedVotingDataV1` | `:62-131` | `uint16 proposalIndex; uint16 optionCount; uint32 proposalTick; uint32 totalVotesAuthorized; uint32 totalVotesCasted; union { Array<uint32,8> optionVoteCount; sint64 scalarVotingResult; };` | 48 (static_assert) |
| `TransferType::*` | `qpi_types.h:85-94` | constants for `PostIncomingTransfer_input.type` (0 standard, 1 procedure tx, 2 qpiTransfer, 3 distributeDividends, 4 revenueDonation, 5 ipoBidRefund, 6 procedureInvocationByOtherContract) | — |

Constants: `NULL_INDEX = -1` (sint64, `qpi_types.h:24`), `INVALID_AMOUNT = 0x8000000000000000` (`:26`),
`NUMBER_OF_COMPUTORS = 676`, `QUORUM = 451` (`:28-29`), `X_MULTIPLIER = 1` (`:232`), `MAX_NUMBER_OF_CONTRACTS = 1024`
(`network_messages/common_def.h:5`), `INVALID_PROPOSAL_INDEX = 0xffff`, `INVALID_VOTE_INDEX = 0xffffffff`,
`NO_VOTE_VALUE = 0x8000000000000000` (`qpi_proposals.h:7-9`).

### 1.9 Non-contract state structs in the same files

* `Contract0State { long long contractFeeReserves[MAX_NUMBER_OF_CONTRACTS]; }` → 8192 bytes
  (`contract_def.h:385-388`; v1.303.2 `:365-368`). `contract0000.229` = 8192 bytes; entries 1..28 are non-zero.
* `IPO { m256i publicKeys[676]; long long prices[676]; }` → 27 040 bytes (`contract_def.h:390-396`), used as the
  "state" of contracts 5 (MLM), 7 (SWATCH), **21 (QRP)**, TESTEXC/D.

---

## 2. Layout rules the engine must implement (x86-64, MSVC-compatible)

1. Scalars: size = align (table §1.1); `bool`/`char`/`uint8` 1; enums = underlying type (default `int`).
2. Struct/class: members in declaration order; each member starts at the next multiple of its alignment; struct
   alignment = max member alignment; `sizeof` = end offset rounded up to the alignment; an empty struct is 1 byte
   (and occupies 1 byte + padding as a member — `HasEmpty { Empty e; uint32 x; }` → x at 4, size 8).
3. `T x[N]`: `N * sizeof(T)`, alignment of `T`. Nested arrays multiply.
4. `union`: size = max member size rounded to max alignment.
5. `static` / `static constexpr` / `enum { … }` / typedefs / member functions / friend declarations: **no storage**.
   In QPI this concerns `BitArray::_bits/_elements`, `HashMap/HashSet/Collection::_nEncodedFlags`,
   `ProposalDataV1::supportScalarVotes`, `VariableScalar::minSupportedValue/maxSupportedValue`,
   `ProposalVoting::maxProposals/maxVotes`, `ContractState::__contract_index`, every `static constexpr` in contracts.
6. Inheritance: derived-class members start at `sizeof(Base)` rounded to their alignment (**MSVC rule, never reuse
   base tail padding**); an *empty* base contributes 0 bytes (EBO, both ABIs; `DerivedFromEmpty` → 4 bytes).
   Multiple/virtual inheritance and virtual functions do not occur in states (contracts are forbidden to use them).
7. No `#pragma pack`, `alignas`, `__declspec(align)`, bit-fields exist in `src/qpi/**` or `src/contracts/**`
   (grep in both versions). Nothing in QPI has alignment > 8, so `max(align) == 8` for every state struct.
8. Zero-initialised: a contract state is created as all-zero bytes (no constructors run; `qpi_linked_list_impl.h:23`
   "Contract state is zero-initialized (no constructor runs)"); every container is valid in the all-zero state.

---

## 3. Containers (`src/qpi/qpi_containers.h`) — members, static data, size formulas

Notation: `S(T)`=sizeof, `A(T)`=alignof, `up(x,a)` = round x up to a multiple of a, `W2(L) = (2*L+63)/64`
(uint64 words for 2-bit flags), `W1(L) = (L+63)/64`. All capacities `L` must be powers of two (static_asserts),
except `SlowAnySizeArray` (any `L ≥ 1`). Containers do not add alignment beyond their members (align =
max(A(T), 8) for everything containing `uint64` counters).

### 3.1 `template <uint64 L> struct BitArray` (`:8-84`)
```
static_assert(L && !(L & (L-1)))                       // :12  power of two
static constexpr uint64 _bits = L;                     // :16  no storage
static constexpr uint64 _elements = ((L + 63) / 64);   // :17  no storage
uint64 _values[_elements];                             // :19  THE ONLY DATA MEMBER
```
size = `8 * W1(L)`, align 8. `bit_2 … bit_4096` typedefs (`:87-98`) → 8,8,8,8,8,8,16,32,64,128,256,512 bytes
(`bit_2`..`bit_64` are all one full uint64). Bit order: `get(i) = (_values[(i>>6) & (_elements-1)] >> (i & 63)) & 1`
(`:29-32`) → bit `i` is bit `(i & 7)` of **byte** `i >> 3` (LSB-first within little-endian words).

### 3.2 `template <typename T, uint64 L> struct Array` (`:115-203`)
`T _values[L];` (`:123`) — size `L*S(T)`, align `A(T)`. `get(i)` masks `i & (L-1)`. User-declared copy ctor/assignment
(`:190-202`) → *non-trivial* type (irrelevant for members, relevant only as a base class, §7).
Typedefs `:206-240`: `sint8_2`…`uint64_8`, `id_2 = Array<id,2>`, **`id_4 = Array<id, 8>` (typo in source, `:239`) and
`id_8 = Array<id, 8>`** — a parser that resolves typedefs gets 256 bytes for `id_4`; do not hard-code "4 ids".

### 3.3 `template <typename T, uint64 L> struct SlowAnySizeArray` (`:252-300`)
`T _values[L];` (`:258`), any `L ≥ 1` (`static_assert(L)`), access `index % L`. size `L*S(T)`. Not used in any state
(HEAD or v1.303.2).

### 3.4 `template <typename KeyT, typename ValueT, uint64 L, typename HashFunc = HashFunction<KeyT>> class HashMap` (`:311-405`)
```
static constexpr sint64 _nEncodedFlags = L > 32 ? 32 : L;   // :318  no storage
struct Element { KeyT key; ValueT value; } _elements[L];     // :321-325
uint64 _occupationFlags[(L * 2 + 63) / 64];                  // :330  2 bits per slot
uint64 _population;                                          // :332
uint64 _markRemovalCounter;                                  // :333
```
* `S(Element) = up(up(S(K), A(V)) + S(V), max(A(K),A(V)))`, `key` at 0, `value` at `up(S(K), A(V))`.
* `off(_occupationFlags) = up(L*S(Element), 8)`; `off(_population) = off(flags) + 8*W2(L)`;
  `off(_markRemovalCounter) = off(_population) + 8`; **`sizeof = up(L*S(Element), 8) + 8*W2(L) + 16`**.
* Examples (g++-verified): `HashMap<id,uint64,1024>` = 41 232; `HashMap<uint64,uint8,16>` = 280 (Element 16);
  `HashMap<uint16,V3{3×uint8},2>` = 40 (Element 6, flags at 16); `HashMap<uint8,uint8,1>` = 32.
* Default `HashFunc = HashFunction<KeyT>` (`:303-307`), implemented in `impl/qpi_hash_map_impl.h:17-30`:
  `hash(key) = first 8 bytes (LE uint64) of KangarooTwelve(&key, sizeof(KeyT))`; specialisation for `m256i`:
  `key.u64._0`. **The K12 input is the raw `sizeof(KeyT)` bytes including padding bytes** (verified with a padded
  struct key, §6.3). No contract passes a custom `HashFunc` (grep).

### 3.5 `template <typename KeyT, uint64 L, typename HashFunc = HashFunction<KeyT>> class HashSet` (`:409-487`)
```
static constexpr sint64 _nEncodedFlags = L > 32 ? 32 : L;   // :416
KeyT _keys[L];                                               // :419
uint64 _occupationFlags[(L * 2 + 63) / 64];                  // :424
uint64 _population;                                          // :426
uint64 _markRemovalCounter;                                  // :427
```
**`sizeof = up(L*S(K), 8) + 8*W2(L) + 16`**; `HashSet<id,1024>` = 33 040, `HashSet<uint32,8>` = 56, `HashSet<uint8,1>` = 32,
`HashSet<V3,64>` = 224 (keys 192 → flags at 192).

### 3.6 `template <typename T, uint64 L> struct Collection` (`:492-657`)
```
static constexpr sint64 _nEncodedFlags = L > 32 ? 32 : L;          // :499
struct PoV { id value; uint64 population; sint64 headIndex, tailIndex; sint64 bstRootIndex; } _povs[L]; // :502-508
uint64 _povOccupationFlags[(L * 2 + 63) / 64];                      // :513
struct Element { T value; sint64 priority; sint64 povIndex; sint64 bstParentIndex;
                 sint64 bstLeftIndex; sint64 bstRightIndex; /* init() */ } _elements[L];              // :518-537
uint64 _population;                                                 // :538
uint64 _markRemovalCounter;                                         // :539
```
* `PoV` = 64 bytes: value 0, population 32, headIndex 40, tailIndex 48, bstRootIndex 56.
* `Element`: value 0, priority `up(S(T),8)`, then +8 each; `S(Element) = up(S(T), 8) + 40`.
* `off(_povOccupationFlags) = 64L`; `off(_elements) = 64L + 8*W2(L)`; `off(_population) = off(_elements) + L*S(Element)`;
  **`sizeof = 64L + 8*W2(L) + L*(up(S(T),8)+40) + 16`**.
* Examples: `Collection<K40,1024>` = 147 728; `Collection<uint8,16>` = 1 816; `Collection<V3,1>` = 136;
  QX `Collection<AssetOrder(40),2097152>` = 302 514 192; `Collection<EntityOrder(48),2097152>` = 319 291 408.

### 3.7 `template <typename T, uint64 L> class LinkedList` (`:664-752`)
```
struct Node { T value; sint64 nextIndex; sint64 prevIndex; } _nodes[L];   // :672-677
uint64 _occupiedFlags[(L + 63) / 64];      // :680  1 bit per node
sint64 _headIndex;                         // :682
sint64 _tailIndex;                         // :683
sint64 _freeHeadIndex;                     // :684
uint64 _nextUnusedIndex;                   // :685
uint64 _population;                        // :686
```
`S(Node) = up(S(T),8) + 16` (next at `up(S(T),8)`, prev +8); **`sizeof = L*S(Node) + 8*W1(L) + 40`**.
`LinkedList<K40,64>` = 3 632, `LinkedList<uint8,128>` = 3 128, `LinkedList<V3,1>` = 72. Not used by any contract yet
(HEAD/v1.303.2); added in commit 76a08756.

### 3.8 Proposal voting (`src/qpi/qpi_proposals.h`, `impl/qpi_proposals_impl.h`) — persisted in CCF, GQMPROP, QUTIL states
* `ProposalDataV1<bool SupportScalarVotes>` (`qpi_proposals.h:239-363`): `Array<uint8,256> url` @0, `uint16 epoch` @256,
  `uint16 type` @258, `uint32 tick` @260, `union Data { Transfer{id destination; Array<sint64,4> amounts}=64;
  TransferInEpoch{id destination; sint64 amount; uint16 targetEpoch}=48; VariableOptions{uint64 variable;
  Array<sint64,4> values}=40; VariableScalar{uint64 variable; sint64 minValue,maxValue,proposedValue}=32 } data` @264
  (64 bytes) → **328** (static_assert `:363`); identical for `<true>` and `<false>`. `supportScalarVotes` is static.
* `ProposalDataYesNo` (`:367-437`): same header, `union Data { Transfer{id destination; sint64 amount}=40;
  VariableOptions{uint64 variable; sint64 value}=16 }` @264 (40 bytes) → **304** (static_assert `:437`).
* `ProposalWithAllVoteData<ProposalDataType, numOfVotes> : public ProposalDataType` (`impl:378-477`):
  adds `VoteStorageType votes[numOfVotes]` where `VoteStorageType` = `sint64` if `supportScalarVotes` else `uint8`
  (`__VoteStorageTypeSelector`, `impl:370-373`). `votes` starts at `sizeof(base)` = 328 (no tail padding in either base,
  so MSVC == Itanium here). With 676 votes: `<ProposalDataV1<true>>` = 328+5408 = **5736**;
  `<ProposalDataV1<false>>` = 328+676 = 1004 → **1008**.
  Specialisation `<ProposalDataYesNo, numOfVotes>` (`impl:481-545`): `uint8 votes[(2*numOfVotes+7)/8]` (2 bits per vote)
  @304 → 304+169 = 473 → **480**.
* `ProposalAndVotingByComputors<uint16 proposalSlotCount>` (`impl:8-115`): data = `id currentProposalProposers[maxProposals]`
  (`:114`), `maxProposals = proposalSlotCount`, `maxVotes = 676` (static). `ProposalByAnyoneVotingByComputors`
  (`impl:117-125`) derives from it and adds **no** data.
* `ProposalAndVotingByShareholders<uint16 proposalSlotCount, uint64 contractAssetName>` (`impl:130-340`):
  `id currentProposalProposers[maxProposals]; id currentProposalShareholders[maxProposals][676];` (`:338-339`) →
  `slots*32 + slots*676*32`; 8 slots = 173 312.
* `ProposalVoting<ProposerAndVoterHandlingT, ProposalDataT>` (`qpi_proposals.h:472-499`):
  `ProposerAndVoterHandlingType proposersAndVoters;` (`:490`) then `ProposalAndVotesDataType proposals[maxProposals];`
  (`:494`, `protected`). g++-verified: `ProposalVoting<ByComputors<200>, V1<false>>` = 6400 + 200×1008 = **208 000**;
  `<ByAnyone<100>, V1<true>>` = 3200 + 100×5736 = **576 800**; `<ByShareholders<8,…>, YesNo>` = 173 312 + 8×480 =
  **177 152**. `DEFINE_SHAREHOLDER_PROPOSAL_TYPES(n, asset)` (`qpi_macros.h:441-445`) typedefs
  `ProposalDataT = ProposalDataYesNo`, `ProposersAndVotersT = ProposalAndVotingByShareholders<n, asset>`,
  `ProposalVotingT = ProposalVoting<ProposersAndVotersT, ProposalDataT>`.

### 3.9 Size formulas cross-checked against g++ (`evidence/layout_vs_gpp_probe.out`)
37/37 instantiations match (all rows of §3 plus `Entity` 64, `Asset` 40, `uint128` 16, `DateAndTime` 8, `bit` 1,
`NoData` 1, `Array<V3,4>` 12, `SlowAnySizeArray<V12,5>` 60). The Python model (`scripts/qpi_layout.py`) builds each
container as a struct of its declared members with the generic rules of §2 and asserts equality with the closed forms.

---

## 4. Decode algorithms (read-only viewer)

Common: `NULL_INDEX == -1`; all indices are `sint64`; `L` is the capacity; `flags[w]` denotes little-endian uint64
word `w` of the flag array.

### 4.1 2-bit occupation flags (HashMap, HashSet, Collection PoV table)

Slot `i` is described by bits `2*(i & 31)` and `2*(i & 31)+1` of word `i >> 5`
(`qpi_hash_map_impl.h:38-39`, `:130`, `:238`):
```
state(i) = (flags[i >> 5] >> ((i & 31) * 2)) & 3
  0b00  empty / never used  -> probing stops here
  0b01  occupied (live)
  0b10  occupied but marked for removal ("tombstone"): the slot stays non-empty for probing; its Element/key bytes
        are ZEROED by removeByIndex() (CLEAR_UNUSED_ELEMENT, impl:240-244 / :574-578)
  0b11  unused -> treat as corruption
```
Bulk counting: `x = int(flags, LE); lo = x & 0x5555…; hi = (x >> 1) & 0x5555…; live = popcount(lo & ~hi);
tombstones = popcount(hi & ~lo); invalid = popcount(lo & hi)` (mask off bits ≥ 2L when L < 32).
`_getEncodedOccupationFlags(flags, i)` (`impl:35-45`) just returns a 64-bit window of consecutive slot states
starting at slot `i` (wrapping at `L`); `_nEncodedFlags = min(L, 32)` is how many 2-bit states that window holds.
A viewer does not need the window — per-slot `state(i)` plus wrap-around probing is equivalent.

### 4.2 HashMap / HashSet

* `_population` = number of slots in state 0b01 **exactly** (incremented in `set()/add()` `:133,:164,:469,:499`,
  decremented in `removeByIndex` `:236,:570`). Tombstones are NOT counted. Verified on every real map (§6.2).
* `_markRemovalCounter` = removals since the last `cleanup()`/`reset()`; it is **not** decremented when a tombstone slot
  is reused by `set()`/`add()` (`:153-166` comment), so `tombstones ≤ _markRemovalCounter` (real data: QX
  `_entityOrders` 359 tombstoned PoVs vs counter 149 034). `cleanup()` (`:278-367`) rebuilds the table in a scratch
  buffer (positions change!) and sets the counter to 0; `needsCleanup(p)` ⇔ `counter > p*L/100` (`:263-267`).
* Enumerate live entries in slot order (= `nextElementIndex()` order, `impl:189-226`):
```
for w in 0 .. W2(L)-1:
    word = flags[w]; if word == 0: continue
    for j in 0 .. 31:                                   # slot i = w*32 + j  (only j < L when L < 32)
        if ((word >> (2*j)) & 3) == 1:
            i = w*32 + j
            key   = read KeyT at   base + off(_elements) + i*S(Element) + 0        # HashSet: base + i*S(KeyT)
            value = read ValueT at base + off(_elements) + i*S(Element) + off(value)
            yield (i, key, value)
```
`isEmptySlot(i)` ⇔ `state(i) != 1` (`:181-187`). `key(i)/value(i)` are only meaningful for state 1.
* Lookup of a key (`getElementIndex`, `impl:65-89`): linear probing from the home slot, stopping at the first 0b00:
```
h = (KeyT is id) ? key.u64._0 : LE64(K12(raw key bytes, sizeof(KeyT)))      # impl:17-30
i = h & (L-1)
repeat L times:
    s = state(i)
    if s == 0: return NULL_INDEX
    if s == 1 and memcmp(slotKey(i), key) == 0: return i                        # operator== (bytewise for id/ints)
    i = (i + 1) & (L-1)
return NULL_INDEX
```
(For struct keys the contract's `operator==` compares fields, e.g. `QIP::IcoBuyerKey`, `ESCROW::EscrowAsset`; a
bytewise compare of the stored key is equivalent for the viewer because stored keys were written by value.)
Verified: re-lookup of every live key in 20 real maps/sets found the same slot, including `uint64`, `sint64`,
`uint16` and 104-byte struct keys hashed with K12 (§6.2).
* `set()` behaviour relevant for interpretation: an existing key gets its value overwritten in place; a new key takes
  the first empty slot *or*, if a tombstone was passed on the way, the first tombstone (`:124-134,:143-147,:153-166`).
  So live entries can sit in slots whose flag went 0b01→0b10→0b01.
* Zero state: all flags 0 → population 0, no entries. `reset()` = `setMem(this, sizeof(*this), 0)` (`:381-385`).

### 4.3 Collection — PoV hash table, dense element array, per-PoV BST

**PoV table** (`_povs` + `_povOccupationFlags`): exactly the HashSet scheme of §4.1/§4.2 with key = `PoV.value` and
hash = `pov.u64._0 & (L-1)` (`qpi_collection_impl.h:25`, `:538`). Enumerate occupied PoVs by scanning the 2-bit flags for
0b01; each gives `{value (id), population, headIndex, tailIndex, bstRootIndex}`.
* `PoV.population` = number of elements in that PoV's queue (incremented `:194,:212`, decremented `:870`).
* When the last element of a PoV is removed, the PoV is **tombstoned**: `pov.population = 0; _markRemovalCounter++;
  flag 0b01→0b10` (`:872-877`) but **`value/headIndex/tailIndex/bstRootIndex` are left stale** (not cleared — real data:
  all 46 tombstoned PoVs of QUOTTERY `mABOrders` still hold a non-zero id). Only `cleanup()`/`reset()` clears them.
  A viewer must only show PoVs with flag 0b01 (optionally list tombstones as "removed").
* `cleanup()` (`:603-716`) rebuilds the PoV table in a scratch buffer, **rewrites `povIndex` of every element whose PoV
  moved** (`:678-695`) and leaves `_elements` otherwise untouched; `_softReset()` (`:13-20`) zeroes the PoV arrays and
  counters only.

**Elements are a dense prefix `[0, _population)`** — proof from the code:
* `_addPovElement` (`:185-230`): `newElementIdx = _population++` — always appended at the end.
* `remove()` (`:782-895`): after unlinking, `--_population`; if the freed array slot `deleteElementIdx` is not the last
  one, `_moveElement(_population, deleteElementIdx)` (`:476-517`) copies the last element into the hole and patches
  every reference to it (PoV head/tail/root, children's parent, parent's child); finally `_elements[_population]` is
  zeroed (`:887-891`, `CLEAR_UNUSED_ELEMENT = true`). (When the removed node has two BST children the *successor's*
  value+priority are copied into the removed node and the successor's slot is the one freed, `:799-831`.)
* `cleanup()` does not touch element positions; `reset()` zeroes everything (`:906-910`).
* Therefore `_elements[i]` is live ⇔ `i < _population`; `element(i)/priority(i)/pov(i)` on larger `i` read zeroed
  memory. Verified: in all 9 real Collections the tail `_elements[_population..L)` is entirely zero (§6.2).
* `Σ PoV.population over flag-0b01 PoVs == _population`, and grouping live elements by `povIndex` reproduces each
  PoV's population (verified).

**Element fields**: `value` (T), `priority` (sint64, user-defined ordering key), `povIndex` (slot of the owning PoV),
`bstParentIndex/bstLeftIndex/bstRightIndex` (element indices or `NULL_INDEX`) forming one binary search tree per PoV
rooted at `PoV.bstRootIndex` (root's parent is `NULL_INDEX`).

**BST invariant and order** (`_searchElement`, `:147-183`; `_addPovElement` `:201-210`): descending from a node with
priority `p`, a new element with priority `q` goes **right if `p >= q`** else left. Hence: left subtree = strictly
higher priorities, right subtree = lower-or-equal priorities, and the **in-order traversal (left, node, right) lists a
PoV's elements from the highest priority to the lowest; equal priorities keep insertion order (FIFO)**.
* `headIndex` = highest priority = leftmost node; updated when a new element has priority `>` head's (`:215-217`).
* `tailIndex` = lowest priority = rightmost node; updated when a new element has priority `<=` tail's (`:219-221`).
* `nextElementIndex(i)` (`:418-447`) = in-order successor (towards lower priority); `prevElementIndex(i)` (`:387-416`)
  = in-order predecessor; both return `NULL_INDEX` for `i >= _population`.
* `headIndex(pov, maxPriority)` = first element with `priority <= maxPriority`; `tailIndex(pov, minPriority)` = last
  element with `priority >= minPriority` (`:48-145`).
* `_rebuild()` (`:280-365`) rebalances a PoV's tree (triggered when `population > 32` and the insertion path was
  longer than `population/4`, `:223-227`); it preserves the in-order sequence and never moves elements in the array.

Pseudocode to list one PoV's queue in priority order (two equivalent ways, both verified against the API):
```
# A) successor walk, exactly what contracts do
i = pov.headIndex; n = 0
while i != NULL_INDEX and n < pov.population:      # guard against corruption
    emit(i, priority(i), value(i)); n += 1
    i = next(i)
next(i): if right(i) != NULL_INDEX: j = right(i); while left(j) != NULL_INDEX: j = left(j); return j
         p = parent(i); while p != NULL_INDEX and right(p) == i: i = p; p = parent(p); return p   # (left(p)==i case returns p)
# B) iterative in-order traversal from pov.bstRootIndex (stack), identical output
```
Alternative overview without trees: iterate `i in [0,_population)`, group by `povIndex`, sort each group by
`(-priority, insertion order unknown)` — but only the tree walk gives the exact FIFO order among equal priorities.

Semantics hints seen in contracts (not generic): QX stores asks with `priority = -price` and bids with `+price` in
the same PoV queue, so the queue reads: bids by descending price, then asks by ascending price
(`contracts/Qx.h:304,:347,:709,:714`).

### 4.4 LinkedList (`impl/qpi_linked_list_impl.h`)

* Occupied flag: bit `i & 63` of `_occupiedFlags[i >> 6]` (`:18`); `popcount(all flags) == _population`.
* Traversal (`headIndex()` `:82-86`, `nextElementIndex` `:94-100`):
```
if _population == 0: list is empty  (do NOT look at _headIndex/_tailIndex)
i = _headIndex; n = 0
while i != NULL_INDEX and n < _population:
    require occupied(i)                              # else corrupt
    emit(i, value(i)); n += 1
    i = _nodes[i].nextIndex                          # prevIndex walks backwards from _tailIndex
```
* **Zero-state caveat**: a never-used list has `_headIndex == _tailIndex == _freeHeadIndex == 0` (NOT -1), because no
  constructor runs. `_initIfNeeded()` (`:21-34`) sets the sentinels to -1 lazily on the first allocation, detectable as
  `_population == 0 && _nextUnusedIndex == 0`. `headIndex()/tailIndex()` return `NULL_INDEX` whenever `_population == 0`
  (`:82-92`), which is the rule the viewer must copy. Verified (§6.3: `llNeverUsed` raw fields 0/0/0, `llEmptied`
  raw head -1, `_nextUnusedIndex` 3, free list head 2).
* Free nodes: `_freeNode` (`:61-74`) clears the occupied bit, zeroes `value`, sets `prevIndex = -1` and chains the node
  into the LIFO free list via `nextIndex` (head `_freeHeadIndex`). `_nextUnusedIndex` = count of nodes ever handed
  out (allocation prefers the free list, then `_nextUnusedIndex++`, `:36-59`). `reset()` zeroes all and sets the three
  sentinels to -1 (`:282-289`).
* Node layout: `value` @0, `nextIndex` @`up(S(T),8)`, `prevIndex` +8.

### 4.5 BitArray, Array, SlowAnySizeArray
* `BitArray<L>`: bit `i` = `(byte[i >> 3] >> (i & 7)) & 1` (equivalently `(_values[i>>6] >> (i&63)) & 1`); the padding
  bits above `L` (when `L < 64`) are normally 0 but should be ignored.
* `Array<T,L>` / `SlowAnySizeArray<T,L>`: element `i` at `i*S(T)`. No population field — the meaning of "used" entries
  is contract-specific (e.g. a separate counter member).

### 4.6 HashMap-like helpers in proposals (vote storage)
* `ProposalWithAllVoteData<ProposalDataV1<false>,N>::votes[i]` (`uint8`): `0xFF` = no vote, else option index
  (`impl:400-401, :456-476`). `<ProposalDataV1<true>,N>::votes[i]` (`sint64`): `NO_VOTE_VALUE` (0x8000000000000000) =
  no vote, otherwise the option index or the scalar value. `<ProposalDataYesNo,N>::votes`: vote `i` =
  `(votes[i >> 2] >> ((i & 3) * 2)) & 3`, value 3 = no vote (`impl:531-544`). After `set()` all votes are 0xFF bytes.
* A proposal slot is in use iff `epoch != 0` (`impl:768`, `:1021`); `clearProposal()` zeroes the whole slot
  (`impl:625`). Vote index `i` is the computor index (ByComputors) or the position in
  `currentProposalShareholders[proposalIndex][i]` (ByShareholders, sorted by possessor id, one entry per share).
* `ProposalTypes` (`qpi_proposals.h:137-234`): `type & 0xff00` = class (0 GeneralOptions, 0x100 Transfer,
  0x200 Variable, 0x300 MultiVariables, 0x400 TransferInEpoch), `type & 0x00ff` = option count (0 = scalar voting).
  Which `union Data` member is meaningful follows from the class.

### 4.7 DateAndTime
```
year  = (v >> 46) & 0xFFFF;  month = (v >> 42) & 0xF;   day = (v >> 37) & 0x1F;  hour = (v >> 32) & 0x1F
minute= (v >> 26) & 0x3F;    second= (v >> 20) & 0x3F;  ms  = (v >> 10) & 0x3FF; us  = v & 0x3FF
valid = v != 0 && 1<=month<=12 && 1<=day<=daysInMonth(year,month) && hour<24 && minute<60 && second<60 && ms<1000 && us<1000
```
Render `YYYY-MM-DD hh:mm:ss.mmm'uuu` (`test/test_util.h:30-44` uses exactly this), "unset" for 0, flag invalid values.
No epoch offset, no time zone (UTC by construction of `etalonTick`). Real data: QUOTTERY event 406 `openDate`
raw → `2026-08-19 18:15:31.000'000`, `endDate` → `2026-10-17 14:00:00.000'000` (§6.2).
`qpi.dayOfWeek(y,m,d)` = `dayIndex(y,m,d) % 7` with 0 = Wednesday (`platform/time.h:45-48`, `qpi_types.h:224-230`) — a
derived value, not stored.

### 4.8 Asset / asset name
`Asset{issuer, assetName}` (§1.7). `assetName` → text as described in §1.7; show the integer too (names are
case-sensitive and contracts compare the integer). `issuer == NULL_ID` denotes contract shares / assets issued by
the system (e.g. GGWP `wpToken` issuer is a user, QX `_tradeMessage.issuer` NULL_ID + `"VOTTUN"` = contract shares).

---

## 5. Identity text encoding, K12, contract ids

### 5.1 `getIdentity` (`src/four_q.h:1777-1797`) — pseudocode
```
input: publicKey[32]            # bytes of the m256i / id
for i in 0..3:
    frag = LE_uint64(publicKey[8*i .. 8*i+8])
    for j in 0..13:
        identity[14*i + j] = 'A' + (frag % 26)      # least significant base-26 digit first
        frag /= 26
checksum3 = KangarooTwelve(publicKey, 32 bytes, output 3 bytes)
c = LE_uint32(checksum3 || 0x00) & 0x3FFFF          # 18 bits  (four_q.h:1788-1790; the 4th byte is uninitialised
for i in 0..3:                                       #  in the core but masked away)
    identity[56 + i] = 'A' + (c % 26); c /= 26
identity[60] = 0                                      # 60 upper-case letters (lower-case variant for digests only)
```
Inverse (`getPublicKeyFromIdentity`, `:1723-1741`): for each of the 4 fragments
`frag = Σ_{j=13..0} (frag*26 + (identity[14*i+j]-'A'))`, computed modulo 2^64 (26^14 > 2^64, no range check), stored
little-endian; letters outside `A..Z` → failure; the checksum letters are **not** verified by the core function — the
viewer should verify them when parsing user input. The `ID(_A,_B,…)` helper (`qpi_types.h:198-206`) builds an id from
56 letter constants with the same digit order (verified: `ID(_Q,_T,_R,…)` == pubkey of `QTREAT…VXHHDHUD`).

### 5.2 KangarooTwelve as used by the core
* Location: `src/kangaroo_twelve.h` (2518 lines: scalar/AVX-512 Keccak-p[1600,12] + `KangarooTwelve()` `:1394-1530`,
  wrapper `:1532-1535`, `KangarooTwelve64To32`, `random()`), and `src/K12/kangaroo_twelve_xkcp.h` (namespace `XKCP`,
  an XKCP-derived implementation used by tests to cross-check). **Neither file carries a licence header**; the
  repository licence is the "Anti-Military License" (`LICENSE.md`; MIT only for `uint128_t`). → Do not copy; implement
  KT128 from the public specification (RFC 9861) and test with the vectors below.
* Parameters (`:652-657`): security 128 → capacity 256 bits, **rate 168 bytes**, chunk size **8192**, leaf suffix 0x0B,
  12-round Keccak-p (round constants `KeccakF1600RoundConstant0..10` `:15-25` + `0x8000000080008008`, i.e. RC[12..23]).
  Short messages (`len ≤ 8191`): `S = M || 0x00` (length_encode of the empty customization string, realised by
  `++byteIOIndex`, `:1480-1489`), domain byte 0x07, pad10*1, one permutation, output = first `outputByteLen` bytes of
  the state (≤ 168). Long messages: standard tree hashing (CV = 32 bytes per 8192-byte chunk, suffix 0x0B, final node
  `S0 || 03 00×7 || CVs || length_encode(n-1) || FF FF`, domain 0x06). Verified identical to the official KT128 for
  lengths 0..83521 (`evidence/identity_selftest.out`, 27 lengths generated by the core's own code).
* Pure-Python reference: `scripts/qubic_identity.py` (`k12()`, `turboshake128()`, `keccak_p1600_12()`; 8 ms per
  identity, fine for a viewer; a C++ port needs Keccak-p[1600,12] only).
* Official KT128 test vectors (M = `ptn(n)` = bytes `i % 251`, C empty, 32-byte output):
  n=0 `1ac2d450fc3b4205d19da7bfca1b37513c0803577ac7167f06fe2ce1f0ef39e5`,
  n=1 `2bda92450e8b147f8a7cb629e784a058efca7cf7d8218e02d345dfaa65244a1f`,
  n=17 `6bf75fa2239198db4772e36478f8e19b0f371205f6a9a93a273f51df37122888`,
  n=289 `0c315ebcdedbf61426de7dcf8fb725d1e74675d7f5327a5067f367b108ecb67c`,
  n=4913 `cb552e2ec77d9910701d578b457ddf772c12e322e4ee7fe417f92c758f0d59d0`,
  n=83521 `8701045e22205345ff4dda05555cbb5c3af1a771c2b89baef37db43d9998b9fe`.
  More (32, 40, 168, 169, 8191, 8192, 8193, 16385, …) in `scripts/ref_vectors_core_generated.txt`.

### 5.3 Identity test vectors (public key hex, little-endian bytes → 60-letter identity)
| public key (hex) | identity | source |
|---|---|---|
| `00…00` (NULL_ID) | `AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAFXIB` | core `getIdentity` (K12 3 bytes `235c3c`) |
| `0100…00` (contract 1, QX) | `BAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARMID` | core, checksum `61e40c` |
| `0200…00` (QTRY) | `CAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACNKL` | core |
| `0400…00` (QUTIL) | `EAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVWRF` | core |
| `0900…00` (QEARN) | `JAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVKHO` | core |
| `1100…00` (QBOND, 17) | `RAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADKAH` | core |
| `1600…00` (QTF, 22) | `WAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVDQA` | core; found live in QRP state |
| `1c00…00` (GGWP, 28) | `CBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAARUCN` | core |
| `ff…ff` | `PQMUYSXMZCXHLHPQMUYSXMZCXHLHPQMUYSXMZCXHLHPQMUYSXMZCXHLHTGWM` | core |
| `9e1a100cfb556def7bcc6252e47ddf0985428637c3d1b3caa16f33fd98438d94` | `AFZPUAIYVPNUYGJRQVLUKOPPVLHAZQTGLYAAUUNBXFTVTAMSBKQBLEIEPCVJ` | ARBITRATOR `public_settings.h:84` |
| `9968ba6a6ebc80e5b018a62c578307203fcef47f0ef98a6f605113553de425d4` | `XPXYKFLGSWRHRGAUKWFWVXCDVEYAPCPCNUTMUDWFGDYQCWZNJMWFZEEGCFFO` | DISPATCHER `public_settings.h:85` |
| `222f107643616c16ca323647e848c61015b6fc1129ec190d3205730071c383fb` | `QTREATZZIVFYQAIBKCZPSHGLIRMALZKHEWAPFLFXJAMDAXMGTBKQVXHHDHUD` | gtest `test/contract_qtreat.cpp:532` |
| `24eb67b7ebad48caa869ce671cb30301c9be1f861721a4b72511e7d861507454` | `EEWCBEZNLEITWFWVEOFBLKHVXTAARMIGJNXICDIRIFDBUDGFXEYABULCFXAN` | `test/contract_qx.cpp:182` |
| `b080e1181018abf269d91fd12d73065ae79aae3e2606c531cb630498bdca19dd` | `QMINEQQXYBEGBHNSUPOUYDIQKZPCBPQIIHUUZMCPLBPCCAIARVZBTYKGFCWM` | `contracts/qRWA.h:1237`; live in ESCROW state |
| `25986d38a63dd6450c0734d8aa479527d72c0f9b3a860aa89e9fb1f3fd3d1f95` | `XPILPIJYHRBTACMMIRSJLIZWCXDBHWVEOTZBQFBXWEUXDZGGDEKDQPIEQKQK` | `src/qubic.cpp:2048` |
40 core-generated rows (incl. 9 pseudo-random keys) in `scripts/ref_vectors_core_generated.txt`; all reproduced by the
Python implementation (`evidence/identity_selftest.out`).

### 5.4 Contract ids
`id(contractIndex, 0, 0, 0)` (`qpi_context.h:37`, `qpi_macros.h:435`): bytes 0-7 = index LE, rest 0. Identity =
letter `'A'+index%26` then `'A'+index/26 …` followed by `A`s and the checksum; index 0 is `NULL_ID`. The viewer can
recognise contract ids as `u64._1 == u64._2 == u64._3 == 0 && u64._0 < MAX_NUMBER_OF_CONTRACTS (1024)` (mind that a
few user-created composite keys look alike). Contract 0 has no state struct of its own (`Contract0State`).

---

## 6. Numerical validation

### 6.1 sizeof(StateData) vs real file sizes (`scripts/contract_states_229.py`, `evidence/sizes_vs_files.out`)
StateData of 8 contracts were transcribed by hand from the v1.303.2 sources into the layout model:

| idx | contract | source (v1.303.2) | model `sizeof` | file size | |
|---|---|---|---|---|---|
| 0 | Contract0State | `contract_def.h:365` | 8 192 | 8 192 | PASS |
| 1 | QX | `contracts/Qx.h:213-242` | 621 806 120 | 621 806 120 | PASS |
| 2 | QUOTTERY | `contracts/Quottery.h:187-224` | 923 559 560 | 923 559 560 | PASS |
| 5/7 | MLM/SWATCH (`sizeof(IPO)`) | `contract_def.h:370` | 27 040 | 27 040 | PASS |
| 14 | NOST | `contracts/Nostromo.h:199-216` | 1 030 098 088 | 1 030 098 088 | PASS |
| 17 | QBOND | `contracts/QBond.h:65-78` | 357 237 432 | 357 237 432 | PASS |
| 18 | QIP | `contracts/QIP.h:100-107` | 26 906 696 | 26 906 696 | PASS |
| 21 | QRP | `contracts/QReservePool.h:13-28` | **4 208** | **27 040** | PASS* |
| 27 | ESCROW | `contracts/Escrow.h:153-171` | 403 505 400 | 403 505 400 | PASS |
| 28 | GGWP (WOLFPACK) | `contracts/GGWP.h:125-182` | 3 669 088 | 3 669 088 | PASS |

QX breakdown: 3×uint64 + 3×uint32 = 36 → Collection at 40; `Collection<AssetOrder,2^21>` = 302 514 192;
`Collection<EntityOrder,2^21>` = 319 291 408 (at 302 514 232); trailing scalars 480 bytes (at 621 805 640) → 621 806 120.
\*QRP: `contractDescriptions[21]` says `sizeof(IPO)` (`contract_def.h:400` v1.303.2 / `:427` HEAD) although
`QRP::StateData` is 4 208 bytes; the file is 27 040 bytes, the state occupies the prefix and bytes `[4208, 27040)` are
zero (checked). **The viewer must take the file size from `contractDescriptions[].stateSize`, not from
`sizeof(StateData)`, and must tolerate `file size > sizeof(StateData)`.**

### 6.2 Real data decoded (`scripts/decode_real.py`, full output `evidence/decode_real.out`, 179 checks, 0 failed, 0.9 s)
Per container the script checks: `_population == #0b01`, no 0b11, `#0b10 ≤ _markRemovalCounter`, tombstone slots
all-zero, re-lookup of every live key via the documented hash+probing, and for Collections: zero tail, Σ PoV
populations, povIndex validity, per-PoV counts, tombstoned PoVs have population 0, head/tail/root properties,
non-increasing priorities, successor-walk == in-order walk. Excerpts:

```
QX _assetOrders: Collection<AssetOrder,2097152> @40  _population=3382 _markRemovalCounter=42
   PoV flags: empty=2097054 occupied=92 marked=6      (all 6 tombstoned PoVs: population 0, stale id kept)
   [0] priority=10 povIndex=1 pov=[issuer=0100..(QX id) | assetName='QWALLET'] value={entity: NPGCDSLR…NZSJ, numberOfShares: 1}
   PoV slot 0: pov=[issuer=00..00 | assetName='QUTIL'] population=38 head=3380 tail=3237 bstRoot=77; queue:
      elem 3380 priority=25000001 …, elem 3095 priority=25000000 …, elem 365 priority=21100000 …   (bids, descending price)
QX _entityOrders: _population=3382 _markRemovalCounter=149034  PoV flags: occupied=790 marked=359
   PoV RZNXSSXS…MGRJ population=9: priorities -2,-3,-4,-5,-6 (asks of 'QXMR', ascending price)
QBOND _epochMbondInfoMap: HashMap<uint16,MBondInfo,1024> _population=47  slot 5: key=207 value={name:'MBND16', stakersAmount:5, totalStaked:11261}
   re-lookup of all 47 keys via K12(key)[0..8) & 1023 OK (1 displaced by collision)
QBOND _userTotalStakedMap: HashMap<id,sint64,524288> _population=175  slot 3020: KVYYKHMD…BWIK -> 7532
QBOND _commissionFreeAddresses: HashSet<id,1024> _population=1 _markRemovalCounter=4 (4 tombstones)  BONDAAFB…ATSO
QBOND _askOrders: Collection<Order,1048576> _population=6; PoV ids are composites (RAAAA…AAJEJPUUMNVDUDAAWEXO)
QUOTTERY mEventInfo: HashMap<uint64,QtryEventInfo,4096> _population=114
   slot 2: key=406 value={eid:406, openDate:"2026-08-19 18:15:31.000'000", endDate:"2026-10-17 14:00:00.000'000", desc:[…]}
QUOTTERY mEventResult: HashMap<uint64,sint8,4096>  values -1 (NOT_SET)
QUOTTERY mPositionInfo: HashMap<id,QtryOrder,8388608> _population=39 (21 of 39 keys displaced from home slot)
QUOTTERY mABOrders: Collection<QtryOrder,2097152> _population=121 _markRemovalCounter=492 occupied PoVs=28 tombstoned=46
NOST users: HashMap<id,uint8,262144> _population=4 _markRemovalCounter=9 tombstones=6 (all zero bytes)
QRP allowedSmartContracts: HashSet<id,128> _population=1  slot 22: WAAAA…VDQA (= contract 22 QTF; 22 & 127 == 22)
ESCROW _earnedTokens: HashSet<EscrowAsset{id,uint64},1048576> _population=6 tombstones=5; keys e.g. {QMINEQQX…FCWM,'QMINE'}
ESCROW _deals: HashMap<sint64,Deal,262144> sizeof(Element)=472 _population=5 tombstones=35 == _markRemovalCounter
ESCROW _ownerDealIndexes: Collection<sint64,262144>: PoV BDSEQVHD…XEOF queue (all priority 0): 33, 34, 36 = insertion order (FIFO)
GGWP holderBalances: HashMap<id,uint64,16384> _population=264 == scalar holderCount; stakedBalances 61 == stakerCount
contract0: contractFeeReserves[1]=10082305477318 … [28]=71654409484, [29..1023]=0
```

### 6.3 Synthetic image from the real container code (`scripts/synthetic/gen_synthetic.cpp`, `validate_synthetic.py`, 56/56)
The core's `qpi_hash_map_impl.h`, `qpi_collection_impl.h`, `qpi_linked_list_impl.h` were compiled with g++ (shims only
for MSVC intrinsics / scratchpad allocation) into a state struct of 24 members; the raw image was decoded with the
documented algorithms and compared with the containers' own API output. Covered: tombstones + slot reuse +
`_markRemovalCounter` semantics, `cleanup()`, `L < 32` tables, K12-hashed `uint32`, `uint64` and padded-struct keys,
Collection with 8 colliding PoVs, duplicate/negative priorities, BST `_rebuild`, removals of head/tail/inner/two-child
nodes, PoV tombstone reuse, LinkedList addHead/addTail/insertAfter/insertBefore/remove/recycling, never-used,
emptied, reset and full lists, BitArray bit order, DateAndTime packing, `uint128` order, `ID()` letters, `id(5,0,0,0)`.

---

## 7. Compiler-specific layout concerns (checklist for the layout engine)

| feature | occurs in states? | rule to implement | evidence |
|---|---|---|---|
| `m256i` alignment | everywhere | 8, not 32 | §1.2; file sizes |
| `bool` | legal; seen in locals/I-O and proposal structs (`Qdraw.h:71` locals, `TestExampleA.h:45`), `AssetIssuanceSelect` | 1 byte, align 1 | g++ |
| `QPI::bit` | yes | 1 byte struct | g++, synthetic |
| enum without underlying type | not in states (log enums only) | `int` (4) | g++ `E_default=4` |
| `enum class X : uint8/uint16` | yes (Pulse, QDuel, RL, QTF, QRP) | underlying type size | g++ |
| `long`, `unsigned long` | **no** (grep both versions) | if ever: 4 bytes (MSVC) | — |
| `long long`, `unsigned int`, `int`, `char` raw types | yes (`Qx.h:25-26`, `Contract0State`, `IPO`, `TestExampleC.h:27`) | 8/4/4/1 | — |
| `long double`, `wchar_t`, pointers, `float/double` | no (prohibited: `qpi.h:11-32`) | reject | — |
| bit-fields | **none** in `src/qpi`, `src/contracts` | implement MSVC rule only if needed (MSVC does not merge bit-fields of different base types) | grep |
| `union` | only `ProposalDataV1::Data`, `ProposalDataYesNo::Data` (in states via `ProposalVoting`), anonymous union in `ProposalSummarizedVotingDataV1` (I/O), `m256i` | max-size/max-align | g++ sizes 64/40 |
| `alignas`, `#pragma pack`, `__declspec(align)` | none in `src/qpi`, `src/contracts` | not needed (network_messages use pack but are not state) | grep |
| empty struct member | `NoData` (I/O only) | 1 byte | g++ `HasEmpty` |
| empty base class | `ContractBase` (contract struct, not StateData); `ProposalByAnyoneVotingByComputors` adds no members | EBO: 0 bytes | g++ `DerivedFromEmpty=4` |
| **non-empty base class** | only `ProposalWithAllVoteData : ProposalDataT` (`impl:379,:482`) and `AssetIssuanceSelect : Asset` | place derived members at `up(sizeof(Base), align)` — **MSVC never reuses base tail padding; g++ does for non-POD bases** (`DerivedFromTailPad`: member at 33, size 40 under g++; MSVC would give 40/48). Both current bases have zero tail padding (328, 304, 40), so results agree today; keep the MSVC rule to stay correct if a future base has tail padding. | g++ probe |
| `static constexpr` data members, `enum {}` constants, typedefs inside structs | many (`_nEncodedFlags`, `supportScalarVotes`, `minSupportedValue`, contract constants) | no storage | §2 |
| template non-type params with expressions (`2097152 * X_MULTIPLIER`, `QRAFFLE_MAX_PROPOSAL_EPOCH`, `CONTRACT_INDEX + 1`) | yes | constant folding over `constexpr`/`#define`/enum values | — |
| nested templates as values (`HashMap<id, Array<uint32,128>, L>`, `HashMap<id, BitArray<N>, L>`, `Array<Array<uint8,N>,M>`) | yes (NOST, QRAFFLE, QTF) | recursive layout | §6.1 NOST |
| nested struct containing a container (`QUOTTERY::StateData::OperationParams { HashMap…; sint64 }`) | yes | ordinary struct | §6.1 QTRY |
| `typedef Order _Order;` / in-class typedefs (`ProposalVotingT`, `LatestTransfersT`) | yes (CCF, GQMPROP, QUTIL, QBOND) | typedef resolution incl. macro-generated ones | — |
| `id_4` typo | typedef is `Array<id, 8>` | resolve, do not assume | `qpi_containers.h:239` |

Trailing padding: `sizeof(StateData)` is rounded up to 8 (e.g. QBOND ends with `uint8 _cyclicMbondCounter` → 7 pad
bytes, included in the 357 237 432-byte file).

---

## 8. HEAD (v1.306.0 / epoch 233) vs v1.303.2 (epoch 229)

* **Byte-identical**: `qpi_types.h`, `qpi_containers.h`, `qpi_assets.h`, `qpi_date_time.h`, `qpi.h`, `qpi_macros.h`,
  `qpi_proposals.h`, `impl/qpi_hash_map_impl.h`, `impl/qpi_collection_impl.h`, `impl/qpi_linked_list_impl.h`,
  `impl/qpi_trivial_impl.h`, `impl/qpi_proposals_impl.h`, `platform/m256.h`, `platform/uint128.h`,
  `lib/platform_common/qstdint.h`.
* `qpi_context.h`: HEAD adds `inline uint32 initialTick() const;` (`:188-190`) — a function, no layout effect.
  `impl/qpi_system_impl.h` and `lib/platform_common/qintrin.h` differ (implementation only).
* Contract set: v1.303.2 has indices 0..28 (`contract_def.h:379-408`); HEAD adds 29 QPAYHUB (construction epoch 231)
  and 30 QTREAT (233) (`contract_def.h:435-436`). Several contracts changed their types between the versions
  (e.g. Nostromo `StateData` grew from 18 to 112 lines, Quottery shifted by 3 lines) → **state layouts must always be
  derived from the source revision matching the file's epoch**; the QPI layer itself can be shared.

---

## 9. Implementation recommendations for the viewer

1. Treat the file as `contractDescriptions[i].stateSize` bytes; decode `StateData` from offset 0; show trailing
   bytes (QRP) as "unused by StateData". Error if `sizeof(StateData) > file size`.
2. Render `id`: identity (60 letters) + hex + "contract N" tag; keep a toggle to show `u64._0.._3` for composite keys.
   Cache K12 per id (3-byte checksum is the only cost).
3. HashMap/HashSet views: header (`L`, population, tombstones, `_markRemovalCounter`, load factor), live entries in slot
   order, optional "show tombstones"; key lookup via the hash function (id → `u64._0`, otherwise K12 of the raw stored
   key bytes) — fall back to a linear scan when the user-supplied key has unknown padding.
4. Collection views: PoV table (live PoVs with population, head/tail/root), per-PoV queue in head→tail order (BST walk
   with the `population` guard), and a flat `_elements[0.._population)` table with `povIndex` resolved to the PoV id;
   mark tombstoned PoVs as removed (stale ids). Validate the invariants of §4.3 and show a warning badge on mismatch.
5. LinkedList: use `_population`-gated traversal; show free-list/`_nextUnusedIndex` only in a "raw" panel.
6. Integrity checks worth surfacing per container: `population == #0b01`, `Σ PoV.population == _population`,
   zero tail of `_elements`, no 0b11 flags, BST parent/child consistency, LinkedList popcount == population.
7. Big files (≥ 1 GB): memory-map; the flag arrays are small (`L/4` bytes) and let you find live entries without
   touching the element arrays; element reads are random-access by index.

---

## Appendix A — scripts and outputs (`scratchpad/research/scripts/`)
* `qubic_identity.py` — K12 + identity codec + `hash_u64_of_key`; self test incl. core-generated vectors.
* `qpi_layout.py` — layout engine, container models, closed forms, decoders (HashMap/HashSet/Collection/LinkedList/BitArray/DateAndTime).
* `contract_states_229.py` — StateData models of QX, QTRY, NOST, QBOND, QIP, QRP, ESCROW, GGWP (+Contract0State, IPO); size comparison.
* `decode_real.py` — real-file decode + 179 checks → `evidence/decode_real.out`.
* `synthetic/gen_synthetic.cpp`, `synthetic/build_synthetic.sh`, `synthetic/synthetic_state.bin`, `synthetic/synthetic_expected.json`, `validate_synthetic.py` → `evidence/validate_synthetic.out`.
* `synthetic/probe_qpi.cpp`, `synthetic/probe_prop.cpp` (g++ sizeof/offsetof probes) → `evidence/gpp_probe_qpi.out`, `evidence/gpp_probe_prop.out`;
  `synthetic/ref_identity.cpp` → `ref_vectors_core_generated.txt`; `synthetic/msvc_shims.h`, `synthetic/stubs/common_buffers.h`.
* Working copies used to build the probes (patched one-liners for g++): `scratchpad/research/p01-work/`.

## Appendix B — g++ ground truth for container instances (`evidence/gpp_probe_qpi.out`, excerpt)
```
HashMap<id,uint64,1024>        size 41232   _elements@0 (40960)  _occupationFlags@40960 (256)  _population@41216  _markRemovalCounter@41224
HashMap<uint64,uint8,16>       size 280     Element 16 (key@0,value@8)  flags@256 (8)  population@264  mrc@272
HashMap<uint16,V3,2>           size 40      Element 6 align 2  _elements 12 bytes  flags@16  population@24  mrc@32
HashSet<id,1024>               size 33040   _keys@0 (32768) flags@32768 (256) population@33024 mrc@33032
Collection<K40,1024>           size 147728  PoV 64  Element 80  _povs@0 (65536) povFlags@65536 (256) _elements@65792 (81920) population@147712 mrc@147720
Collection<uint8,16>           size 1816    Element 48 (value@0, priority@8)  _povs@0 (1024) flags@1024 (8) _elements@1032 (768) population@1800
LinkedList<K40,64>             size 3632    Node 56 (value@0,next@40,prev@48)  _nodes@0 (3584) flags@3584 (8) head@3592 tail@3600 freeHead@3608 nextUnused@3616 population@3624
ProposalWithAllVoteData<V1<true>,676> 5736 (votes@328, 5408)   <V1<false>,676> 1008 (votes@328, 676)   <YesNo,676> 480 (votes@304, 169)
```
