# 06 — Qubic contract state files and sibling epoch files: how they are produced, consumed and how to live-watch them

Research report for qstate-viewer (phase: research). Everything below is grounded in the sources listed in §1; `file:line` references are to those checkouts. A throw-away spike (standalone KangarooTwelve, computer-digest tool) lives in `scratchpad/research/06-spike/` (§13).

---

## 0. TL;DR for the spec

1. **Names.** `contract????.???` → `contractNNNN.EEE` (NNNN = 4-digit zero-padded contract index, EEE = 3 decimal digits of the epoch, i.e. `epoch % 1000`), plus `spectrum.EEE`, `universe.EEE`, `contract_exec_fees_rec.EEE`, `contract_exec_fees_acc.EEE` (snapshots only), `system` / `system.eoe` / `system.snp`. Regex for contract files: `^contract(\d{4})\.(\d{3})$`. The special extension `000` means "saved on demand" (F6 key or node snapshot), not epoch 0 (core `src/qubic.cpp:8640-8668`, `5335-5373`).
2. **Content = raw memory dump** of `contractStates[i]`, exactly `contractDescriptions[i].stateSize` bytes (`save(CONTRACT_FILE_NAME, contractDescriptions[contractIndex].stateSize, contractStates[contractIndex], directory)`, core `src/qubic.cpp:7198`). No header, no checksum, little-endian x86-64 layout.
3. **`contractNNNN.229` is the state at the START of epoch 229, written at the END of epoch 228 by the node software that ran epoch 228 (v1.302.x).** Proven by (a) the transition code: `endEpoch()` increments `system.epoch` (core `src/qubic.cpp:5190`), `beginEpoch()` then stamps the new epoch into the file names (`4976-4994`) and only then the main loop saves spectrum/universe/contracts/fee reports (`6944-6947`, `9298-9313`); (b) the sample files' mtimes: `2026-09-02 12:00:14..17 UTC`, i.e. 14-17 s after the epoch-228→229 boundary (Wednesday 12:00 UTC; `6799-6806`); (c) the public RPC status: epoch 228 ended at tick 77560787, epoch 229 started at 77700000 (= `#define TICK` of v1.303.x). The file is **read** by the version that declares `EPOCH 229` (v1.303.0-2) at start-up (`7504-7614`), before `INITIALIZE`/`BEGIN_EPOCH` of epoch 229 run (`4094-4120`).
4. **Schema choice.** Writer = latest tag with `#define EPOCH E-1`, reader = tag with `#define EPOCH E`. Both describe the same bytes except for contracts listed in the reader's `contractStateChangeInfos[]` with `changeEpoch == E` (`src/contract_core/contract_def.h:583-604`). For the 229 set, writer v1.302.1 and reader v1.303.2 have identical state layouts (git diff shows only code changes), so "use the checkout whose `EPOCH == E`" is correct. HEAD (v1.306.0, EPOCH 233) decodes all 229 files except `contract0014` (NOST), whose 229 layout is HEAD's `NOST::OldStateData` (migrated at epoch 230).
5. **Size != sizeof(StateData)** is handled by the node only at load time and only when a `contractStateChangeInfos` entry exists for that contract and epoch: `PADDING` (file smaller, zero-extend), `RESET` (ignore file, zero), `MIGRATE` (file size must equal `sizeof(X::OldStateData)`; `MIGRATE` procedure converts). Other cases: contract in IPO (`epoch < constructionEpoch`, file missing → zeros), construction epoch (file ignored, zeros + `INITIALIZE`). Everything else is a fatal load error (`src/qubic.cpp:7032-7164`).
6. **Three contracts (MLM 5, SWATCH 7, QRP 21) have `stateSize = sizeof(IPO) = 27040`** although their `StateData` is empty (MLM, SWATCH) or small (QRP, real data in the first ~4.2 KB): decode `StateData` as a prefix of the file (`contract_def.h:411,413,427`).
7. **Digest.** Per-contract digest = KangarooTwelve (KT128, RFC 9861, **empty customization string**, 32-byte output) over the whole `stateSize` bytes; computer digest = Merkle root over 1024 leaves (`K12(64 bytes)` inner nodes, zero leaves for unused indices) — `src/qubic.cpp:819-887`. Verified in the spike against the core's own implementation (bit-identical, incl. the 8192-byte edge case of `contract0000`). Digest of the 229 sample set: `9271f1ec…155a` (table in §7.5).
8. **Write pattern.** core (UEFI): open-with-create (no truncate), 32 KiB chunks, close (`src/platform/file_io.h:315-403`) — a shrinking struct leaves stale tail bytes on FAT32. core-lite (Linux/Windows): `fopen("wb")` → truncate to 0 → one `fwrite` → `fclose` (`core-lite src/platform/file_io.h:458-487`): a watcher **will** observe size 0 and partial sizes; there is no temp+rename for epoch files. Snapshots in core-lite are staged in `ep<E>.tmp/` and promoted by directory rename (`core-lite src/qubic.cpp:6240-6247`, `6470-6481`), so the watched `ep<E>` directory is **replaced** (new inode) on each snapshot.
9. **core-lite** writes exactly the same files, in the process' current working directory; snapshots (`ep<E>/` with `.000` extension) are produced on F8, `GET /request-save-snapshot`, or the `SPECIAL_COMMAND_SAVE_SNAPSHOT`; with the default `TICK_STORAGE_AUTOSAVE_MODE 2` there is no periodic autosave. Epoch files only change at an epoch transition (local long-run testnet: every 5,184,244 ticks or F7). Nothing is saved on shutdown except `system`, score cache and ant replay cache.
10. **Auto-detect**: group directory entries by the regex above, require contiguous indices from 0, read the epoch from the extension; for `000` sets read it from the enclosing `ep<E>` directory name or from `system.snp`/`system` (`uint16 epoch` at byte offset 2), and fingerprint the writer's `contractCount` as `size(contract_exec_fees_rec.EEE) / 5408` (= 29 for the sample).

---

## 1. Sources

| Alias | Path | Version |
|---|---|---|
| core HEAD | `/home/yeti/devwork/space/core` | tag v1.306.0, `EPOCH 233`, `TICK 82400000`, `contractCount = 31` (indices 0..30) |
| core 229 | `/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/core-v1.303.2` | tag v1.303.2, `EPOCH 229`, `TICK 77700000`, `contractCount = 29` (0..28) |
| core-lite | `/home/yeti/devwork/testnet/core-lite` | `E233.0-3-gd56abeaf` (core-lite tag E233.0 + 3 commits; merges core v1.306.0); local uncommitted change: `#define TESTNET` and `#define LONG_RUN_LOCAL_TESTNET` enabled in `src/qubic.cpp:54-59`; `build/` only has a CMake cache (configured for clang++-20, not built here) |
| samples | `/home/yeti/devwork/space/229` and the identical copy `/home/yeti/devwork/testnet/229` | 29 contract files, `contract_exec_fees_rec.229`, `spectrum.229`, `universe.229`, `ep229-full.zip` |

`/home/yeti/devwork/testnet` otherwise contains `nostromo/` (a web app, unrelated to state files), `qli` (a binary), and no `ep*` snapshot directories or `.000` files anywhere (checked with `find`).

`unzip -l ep229-full.zip` (32 entries, 8,136,696,064 bytes, all dated 2026-09-02 12:00 UTC, zip origin "Unix"): `contract0000.229` … `contract0028.229`, `contract_exec_fees_rec.229`, `spectrum.229`, `universe.229` — i.e. exactly the files that sit next to it. No `system`, no snapshot files, no `contract_exec_fees_acc`.

---

## 2. File names and directories

### 2.1 Name templates (core HEAD `src/public_settings.h:87-105`, identical in v1.303.2; core-lite uses `wchar_t` instead of `unsigned short`, `core-lite src/public_settings.h:129-147`)

```
SYSTEM_FILE_NAME                      "system"
SYSTEM_END_OF_EPOCH_FILE_NAME         "system.eoe"
SPECTRUM_FILE_NAME                    "spectrum.???"
UNIVERSE_FILE_NAME                    "universe.???"
SCORE_CACHE_FILE_NAME                 "score.???"
CONTRACT_FILE_NAME                    "contract????.???"
CONTRACT_EXEC_FEES_ACC_FILE_NAME      "contract_exec_fees_acc.???"
CONTRACT_EXEC_FEES_REC_FILE_NAME      "contract_exec_fees_rec.???"
REVENUE_DATA_END_OF_EPOCH_FILE_NAME   "revenue_data.eoe"
REVENUE_DATA_SNAPSHOT_FILE_NAME       "revenue_data.???"
MULTIDIM_REVENUE_SNAPSHOT_FILE_NAME   "revenue_data_multi.???"
MULTIDIM_REVENUE_END_OF_EPOCH_FILE_NAME "revenue_data_multi.eoe"
ANT_SNAPSHOT_HEADER_FILENAME          "snapshotAntColonyHeader.???"   (+Records, +Pool, antColonyReplayCache.???, antColonySolutions.eoe, snapshotAntSolutionFlag)
SCORE_BPP9000_TASK_FILE_NAME          "bpp9000.task"
```

The templates are static buffers patched in place:

* epoch: `name[len-4] = epoch/100+'0'; name[len-3] = (epoch%100)/10+'0'; name[len-2] = epoch%10+'0'` (`beginEpoch()`, `src/qubic.cpp:4976-4994`; generic helper `addEpochToFileName`, `src/platform/file_io.h:1291-1296`). Only 3 digits: for epoch ≥ 1000 the first character would leave `'0'..'9'` — irrelevant today (epoch 233), but the viewer should parse the extension as `epoch % 1000` and prefer other epoch evidence (§12) when available.
* contract index: `name[len-9..len-6] = index/1000, (index%1000)/100, (index%100)/10, index%10` (`src/qubic.cpp:7039-7042` load, `7193-7196` save). Hence `contract0000.229`, `contract0028.229`.
* `000`: F6 and snapshot code overwrite the three epoch characters with `'0'` (`src/qubic.cpp:8643-8661`, `5335-5373`). **The buffer is not restored afterwards**; it is only re-stamped with the epoch by the next `beginEpoch()`. Consequence: after an F6/F8 save, a subsequent transition still produces correct `.E+1` names because the transition calls `beginEpoch()` before saving.

Regexes for the viewer:

```
contract state   ^contract(?P<idx>\d{4})\.(?P<ext>\d{3})$
spectrum/universe ^(spectrum|universe)\.(?P<ext>\d{3})$
fee files        ^contract_exec_fees_(rec|acc)\.(?P<ext>\d{3})$
snapshot dir     ^ep(?P<epoch>\d+)(\.tmp|\.old)?$          (no zero padding: "ep229")
system           ^system(\.eoe|\.snp)?$
other snapshot   ^snapshot[A-Za-z]+(\.\d{3})?$, ^score\.\d{3}$, ^revenue_data(_multi)?\.(\d{3}|eoe)$, ^antColony\w+\.(\d{3}|eoe)$, ^logEventState\.db$
```

### 2.2 Where the files live

* **core (UEFI, bare metal):** root directory of the FAT32 volume labelled `QUBIC` (`initFilesystem()`, `src/platform/file_io.h:1146-1259`, `VOLUME_LABEL L"Qubic"`), i.e. `/contract0000.229`, `/spectrum.229`, `/system`, `/efi/boot/Qubic.efi` (README.md:40-61). A `directory` argument other than `NULL` is only used for snapshot directories `ep<epoch>` (`src/qubic.cpp:5320-5322`, `5544-5546`). While the node runs, nothing else can read that disk; files reach the desktop as copies (weekly distribution zips such as `ep229-full.zip`, https://storage.qubic.li/network, Discord).
* **core-lite (Linux/Windows/macOS):** paths are relative to the process' current working directory (`getHostFilePath(fileName, directory)` → `directory / fileName` or just `fileName`, `core-lite src/platform/file_io.h:67-76`); README "Place all of these files in the same directory where you plan to launch the `Qubic` binary". There is **no command-line option for a data directory** (`processArgs`, `core-lite src/qubic.cpp:11866-11937`). Snapshot directories are `./ep<E>` (promoted from `./ep<E>.tmp`).
* core-lite also creates swap/virtual-memory page directories named `<4 letters>.<EEE>` (e.g. `logs.233`, `pmap.233`, `imap.233`, …; `core-lite src/platform/virtual_memory.h:407-441`) plus `s/` shadow subdirectories inside them (`core-lite src/extensions/disk_shadow.h`), `.qubic-tmp/` for zipped RPC downloads (`rpc_routes.h:52`), `bpp9000.task`, `debug.log`, `profiling.csv`, `crash.dump`. The viewer must ignore these.

---

## 3. When contract state files are written (and read)

### 3.1 Start-up load (core `src/qubic.cpp:7504-7644`, core-lite `9190-9367`)

```
load("system")                       -> system struct (ignored fields overwritten:)
system.version = VERSION_B; system.epoch = EPOCH; if (system.epoch == EPOCH) system.initialTick = TICK; system.tick = system.initialTick;
beginEpoch()                         -> file names get extension EPOCH           (4976-4994)
initializeContracts()                -> registers procedures incl. MIGRATE        (7530)
#if TICK_STORAGE_AUTOSAVE_MODE  canLoadFromFile = loadAllNodeStates()  -> tries directory "ep<EPOCH>" with ".000" names (5538-5819)
if (!canLoadFromFile):
    loadSpectrum()  ("spectrum.EPOCH")  -> spectrum digest tree computed
    loadUniverse()  ("universe.EPOCH")
    loadContractStateFiles()            (7613)   -> "contractNNNN.EPOCH" for NNNN in 0..contractCount-1
#if !START_NETWORK_FROM_SCRATCH loadContractExecFeeFiles()   (7615-7618)  -> "contract_exec_fees_rec.EPOCH" only when joining a running epoch
    getComputerDigest()  -> logged as "Computer digest = <identity>"            (7628-7637)
```

Per contract, `loadContractStateFiles()` (HEAD `7032-7164`; v1.303.2 `6957-7082`) does:

```
if constructionEpoch == system.epoch && !forceLoadFromFile:   zero state ("initialized with zeros for construction")   # file ignored
else:
    sz = getFileSize(name)                                    # -1 if missing
    if sz == stateSize:          load exactly stateSize bytes; failure -> fatal
    elif system.epoch < constructionEpoch && stateSize >= sizeof(IPO):  zero state ("initialized with zeros for IPO")   # IPO phase, file normally absent
    else:
        look up {contractIndex, changeType, changeEpoch == system.epoch} in contractStateChangeInfos[]
        RESET   -> zero state                                           (7091-7098)
        PADDING -> if 0 < sz < stateSize: zero, then load sz bytes at offset 0  (7099-7118)
        MIGRATE -> if sz == contractMigrateOldStateSizes[i] (= sizeof(X::OldStateData)): load into scratch, zero state, run X::__migrate(state, oldState)  (7119-7149)
        anything else -> "cannot be read successfully" -> node start-up fails (core) / tolerated in core-lite TESTNET (see §11)
```

Difference between the two core versions: v1.303.2 calls `load(name, stateSize, …)` *first* and only inspects the real size when the read returns short (`6978-6981`); because UEFI `Read()` happily returns the first `stateSize` bytes of a **larger** file, v1.303.2 (and all earlier versions) silently accept oversized files. HEAD compares `getFileSize()` to `stateSize` first (`7055-7056`), so oversized files are rejected unless a state-change rule applies.

### 3.2 The epoch transition (seamless; the only automatic writer of `.EEE` files)

Tick processor, core `src/qubic.cpp:6797-6957` (core-lite `8395-8590`, identical structure):

```
1. after processing tick T: if date >= Wednesday 12:00 UTC of epoch end  -> epochTransitionState = 1      (6799-6806)
   (core-lite TESTNET: if system.tick - system.initialTick >= TESTNET_EPOCH_DURATION, LONG_RUN: also on F7 / SPECIAL_COMMAND_FORCE_SWITCH_EPOCH, 8396-8410)
2. system.tick++                                                                                             (6867)
3. endEpoch()                                                                                                (6885 -> 5047-5194)
      contractProcessor phase END_EPOCH (all contracts' END_EPOCH procedures)                                 (5050-5052)
      finishIPOs()  (IPO struct in contract state is read, shares issued, fee reserve set in contract 0)      (5064 -> contract_core/ipo.h:128-192)
      revenue payout, reorganizeSpectrum(), assetsEndEpoch()                                                  (5075-5169)
      system.epoch++ ; system.initialTick = system.tick                                                       (5190-5191)
4. asyncSave("revenue_data.eoe"), asyncSave("revenue_data_multi.eoe"), ant colony "antColonySolutions.eoe"    (6889-6901)
5. systemMustBeSaved = true; WAIT  -> main loop saveSystem() writes "system.eoe" (epochTransitionState == 1)   (6912-6913, 7242)
6. epochTransitionState = 2; beginEpoch()  -> names now carry the NEW epoch E+1                               (6914-6916, 4976-4994)
7. spectrumMustBeSaved = universeMustBeSaved = computerMustBeSaved = true; WAIT                               (6944-6947)
      main loop (9298-9313): saveSpectrum() -> "spectrum.E+1"; saveUniverse() -> "universe.E+1";
                             saveContractStateFiles() -> "contract0000.E+1" .. "contract{N-1}.E+1" (index order);
                             saveContractExecFeeFiles() -> "contract_exec_fees_rec.E+1" (acc file NOT written here)
8. etalonTick updated with fresh digests, epochTransitionState = 0                                            (6949-6956)
9. next processTick() is the first tick of E+1: INITIALIZE (contracts with constructionEpoch == E+1, state zeroed first) and BEGIN_EPOCH run (4094-4120)
```

Therefore `contractNNNN.E` = state after END_EPOCH of epoch E-1 (and after `finishIPOs`) and before INITIALIZE/BEGIN_EPOCH of epoch E. Sample evidence: all 32 files have mtimes `2026-09-02 12:00:14-17 UTC` in write order spectrum (12:00:14) → universe (12:00:15) → contract0000..0028 (12:00:15-17) → contract_exec_fees_rec (12:00:17). Epoch 229 begins on `2022-04-13 12:00 UTC + 229·7 days = 2026-09-02 12:00 UTC` (`dayIndex` formula `src/platform/time.h:45`, trigger `src/qubic.cpp:6800-6801`). 1-second mtime granularity and "Unix" zip origin indicate a Linux (core-lite) mainnet node wrote the sample set (FAT32 would give 2-second granularity).

Mainnet timeline from https://rpc.qubic.org/v1/status (fetched 2026-10-02): epoch 228 = ticks 76550000–77560787, epoch 229 = 77700000–79146843, 230 = 79300000–80371512, 231 = 80371514–81259537 (seamless), 232 = 81400000–82269080, 233 = 82400000–…. A round initial tick means "network restarted from scratch with a new version at Wednesday 12:00 UTC from the `.EEE` files"; a non-round one (231: 80371514, 225-227, 221) means a seamless transition without restart (those versions ship `START_NETWORK_FROM_SCRATCH 0`, e.g. v1.304.1).

### 3.3 Operator-triggered saves

| Trigger | What is written | Where | Code |
|---|---|---|---|
| **F6** key (core & core-lite) | `spectrum.000`, `universe.000`, `contractNNNN.000` (all), `contract_exec_fees_rec.000` | root / cwd, in place | core `8640-8668`, core-lite `10488-10522` |
| **F8** key, `SPECIAL_COMMAND_SAVE_SNAPSHOT` (network message), core-lite `GET /request-save-snapshot`, auto-save every `TICK_STORAGE_AUTOSAVE_TICK_PERIOD` ticks in AUX mode when `TICK_STORAGE_AUTOSAVE_MODE == 1` | full node snapshot: `spectrum.000`, `universe.000`, `snapshotUniverseIndex`, `contractNNNN.000`, `contract_exec_fees_rec.000`, `contract_exec_fees_acc.000`, `system.snp`, `score.EEE`, `antColonyReplayCache.EEE`, `snapshotNodeMiningState`, `revenue_data.EEE`, `revenue_data_multi.EEE`, `snapshotSpectrumDigest`, `snapshotUniverseDigest`, `snapshotComputerDigest`, `snapshotMinerSolutionFlag`, `snapshotAntSolutionFlag`, tick storage `snapshotTickdata.EEE`/`snapshotTicks.EEE`/`snapshotTickTransactionOffsets.EEE`/`snapshotTickTransaction.EEE` (chunked `.NNN` suffixes above 200 MB), `snapshotMetadata.EEE`, ant colony, oracle and OC engine snapshot files, `logEventState.db` | core: `ep<E>/` written in place; core-lite: `ep<E>.tmp/` then renamed to `ep<E>/` | core `5312-5536` (`requestPersistingNodeState` handling `9353-9366`; F8 `8688-8696`; special command `2346-2353`); core-lite `6228-6483`, `11591-11612`, `rpc_routes.h:164-171`, `private_settings.h:1453-1466` (`TICK_STORAGE_AUTOSAVE_MODE 2`, period 1337) |
| periodic (`SYSTEM_DATA_SAVING_PERIOD` = 300 s, AUX mode, no autosave) | `system`, `score.EEE`, `antColonyReplayCache.EEE` | root / cwd | core `9128-9140` |
| shutdown (ESC) | `system`, `score.EEE`, `antColonyReplayCache.EEE` — **contract states are NOT saved** | root / cwd | core `9465-9467`, core-lite `11710-11712` |

The snapshot is taken while the tick processor is parked right before `processTick(system.tick)` (`6531-6536`): the files contain the state **after tick `system.tick - 1`**; `system.snp` carries that `system.tick`.

Snapshot protocol (both cores): first `snapshotMetadata.EEE` is overwritten with an all-zero `MetaData` (epoch=0) (`invalidateNodeStates`, `5329`, `tick_storage.h:419-431`), then all files are written, and the valid metadata `{epoch, tickBegin, tickEnd = system.tick, outTotalTransactionSize, outNextTickTransactionOffset}` (32 bytes: 3×u32 + pad + 2×u64, `tick_storage.h:103-110`) is the last tick-storage file written (`5496`, `tick_storage.h:349-354`). Contract files are written **before** the metadata, so "metadata.epoch != 0" ⇒ the contract files of an in-place snapshot are complete. core-lite additionally stages everything in `ep<E>.tmp` and promotes with two renames (`6470-6481`): `ep<E>` → `ep<E>.old`, `ep<E>.tmp` → `ep<E>`, then deletes `ep<E>.old`.

### 3.4 IPO-phase and construction-epoch files

During the IPO epoch (`system.epoch == constructionEpoch - 1`) the contract's state buffer holds an `IPO` struct (`m256i publicKeys[676]; long long prices[676];` = 27040 bytes, `contract_def.h:390-396`; bids written in `contract_core/ipo.h:46-108`, `IPO* ipo = (IPO*)contractStates[contractIndex]`). `finishIPOs()` reads it at END_EPOCH and does not clear it. Hence:

* `contractNNNN.<constructionEpoch>` (and snapshot files taken during the IPO epoch) start with the final IPO order book (prices sorted descending, `prices[675]` = final price; a zero final price = failed IPO), followed by zero bytes up to `stateSize`. The node ignores this file at the construction epoch and zeroes the state before `INITIALIZE` (`7047-7052`, `2810-2834`).
* `contractNNNN.<IPO epoch>` normally does not exist (the contract index was introduced by the new version), and the loader tolerates the absence (`7068-7072`).
* Viewer rule: `fileEpoch == constructionEpoch` → offer an "IPO result" decoding (struct `IPO`) instead of/in addition to `StateData`; `fileEpoch < constructionEpoch` → not applicable.

---

## 4. Which version wrote / reads which file; choosing the schema version

### 4.1 Rule

* The bytes of `contractNNNN.E` were produced by the binary that ran epoch E-1 (seamless transition code above); for a network that was restarted at the E-1→E boundary this is the last patch release with `#define EPOCH E-1`; for seamless chains it is the version that was running (same `X.Y`).
* The bytes are consumed by the binary with `#define EPOCH E` (`system.epoch = EPOCH`, `src/qubic.cpp:7508`). Core guarantees compatibility only via `contractStateChangeInfos` (reader side). So **decoding with the reader checkout (`EPOCH == E`) is correct, provided the viewer applies the reader's state-change table for epoch E**:
  * no entry / entry with another epoch → reader `X::StateData` describes the file (sizes must match);
  * `PADDING` → reader `StateData` describes a *superset*: decode the prefix that exists, treat the rest as zeros;
  * `MIGRATE` → reader `X::OldStateData` describes the file (`__migrateOldStateSize = sizeof(CONTRACT_STATE_TYPE::OldStateData)`, `src/qpi/qpi_macros.h:195-205`);
  * `RESET` → the file is pre-reset data in the *previous* layout (needs the writer checkout) and is ignored by the node.
* `OldStateData`/`MIGRATE()` definitions linger in contract sources after the migration epoch (HEAD still has `NOST::OldStateData` at `src/contracts/Nostromo.h:414` and `QRAFFLE::OldStateData` at `src/contracts/QRaffle.h:321`, migrated in 230 and 222/223). Presence of `OldStateData` alone must not trigger anything; only "file size == sizeof(OldStateData)" should.

### 4.2 Tag table (from `git show <tag>:src/public_settings.h` / `contract_def.h` in core HEAD's repository)

| tag (date) | EPOCH | TICK | START_FROM_SCRATCH | contractCount | contractStateChangeInfos |
|---|---|---|---|---|---|
| v1.298.0 (2026-07-01) | 220 | 63760000 | 1 | 29 | – |
| v1.298.1 / v1.298.2 (07-08) | 220 / 221 | 63760000 / 64911281 | 0 | 29 | – |
| v1.299.0 (07-14) | 222 | 66350000 | 1 | 29 | {QRAFFLE, MIGRATE, 222} |
| v1.299.1 (07-15) | 222 | 66360000 | 1 | 29 | – |
| v1.300.0/1 (07-22/28) | 223 | 68200000 | 1 | 29 | {QRAFFLE, MIGRATE, 223} |
| v1.301.0-2 (07-29) | 224 | 70550000 | 1 | 29 | {QIP, RESET, 224}, {RANDOM, PADDING, 224} |
| v1.301.3 (08-05) | 225 | 72065097 | 0 | 29 | (same, inert) |
| v1.301.4-6 (08-12/14) | 226 | 73857162 | 0 | 29 | (same, inert) |
| v1.301.7 (08-19) | 227 | 75241781 | 0 | 29 | (same, inert) |
| v1.302.0/1 (08-26/31) | 228 | 76550000 | 1 | 29 | (same, inert) ← **writer of the 229 set** |
| v1.303.0 (09-02) | 229 | 77700000 | 1 | 29 | {QIP,RESET,224},{RANDOM,PADDING,224},**{NOST, MIGRATE, 229}** |
| v1.303.1 (09-02), **v1.303.2** (09-06) | 229 | 77700000 | 1 | 29 | {QIP,RESET,224},{RANDOM,PADDING,224} (NOST entry removed) ← **reader of the 229 set** |
| v1.304.0 (09-09) | 230 | 79300000 | 1 | 30 (+QPAYHUB) | {NOST, MIGRATE, 230} |
| v1.304.1 (09-16) | 231 | 80371514 | 0 | 30 | (same, inert) |
| v1.305.0 (09-23) | 232 | 81400000 | 1 | 31 (+QTREAT) | (same, inert) |
| v1.306.0 (09-30) = HEAD | 233 | 82400000 | 1 | 31 | (same, inert) |

`git diff v1.302.1 v1.303.2 -- src/contracts src/qpi src/contract_core` touches only `QIP.h` procedure bodies and deletes `QRaffle_old.h`/`Qswap_old.h`: no state layout changed between writer and reader of the 229 set. `git diff v1.303.2 v1.306.0 -- src/contracts src/qpi` changes `Nostromo.h` (+7300 lines, new `StateData`, `OldStateData` = v1.303.2 `StateData` with `*_OLD` constants: `NOSTROMO_MAX_USER_OLD = 262144`, `…_PROJECT_USER_INVEST_OLD = 128`, `…_TOKEN_OLD = 262144`, `…_PROJECT_OLD = 262144`, `Nostromo.h:140-143,414-474`; the three nested record structs are re-declared *inside* `OldStateData`, so a parser must resolve names in the nested scope first), adds `QPayhub.h` (index 29) and `QTREAT.h` (index 30), and makes code-only changes in `Quottery.h`. So HEAD decodes the 229 set with `StateData` for all contracts except 14 (NOST → `OldStateData`).

### 4.3 Fingerprinting the writer without any header

* `size(contract_exec_fees_rec.EEE) / (676·8) = contractCount` of the **writer** (`sizeof(ExecutionFeeReportCollector) = unsigned long long executionFeeReports[contractCount][NUMBER_OF_COMPUTORS]`, `src/ticking/execution_fee_report_collector.h:12,158-166`). Sample: 156832 / 5408 = 29 → v1.302.x/v1.303.x. Any remainder ≠ 0 means "not a fee-report file".
* Number of `contractNNNN.EEE` files = writer's `contractCount` (saved in a loop over `0..contractCount-1`, `7191`).
* Per-file size must equal `contractDescriptions[i].stateSize` of the chosen checkout; the first mismatch tells which contract's layout differs between writer and chosen reader.

### 4.4 Contract index ↔ source mapping (HEAD `src/contract_core/contract_def.h:24-318, 405-444`)

| idx | asset name | `CONTRACT_STATE_TYPE` (header) | constructionEpoch | stateSize expression |
|---|---|---|---|---|
| 0 | "" | `Contract0State` (contract_def.h:385) | 0 | `sizeof(Contract0State)` = 8192 |
| 1 | QX | `QX` (Qx.h) | 66 | `sizeof(QX::StateData)` |
| 2 | QTRY | `QUOTTERY` (Quottery.h) | 72 | StateData |
| 3 | RANDOM | `RANDOM` (Random.h) | 88 | StateData |
| 4 | QUTIL | `QUTIL` (QUtil.h) | 99 | StateData |
| 5 | MLM | `MLM` (MyLastMatch.h) | 112 | **`sizeof(IPO)` = 27040** (StateData is `ContractBase::StateData {}`) |
| 6 | GQMPROP | `GQMPROP` (GeneralQuorumProposal.h) | 123 | StateData |
| 7 | SWATCH | `SWATCH` (SupplyWatcher.h) | 123 | **`sizeof(IPO)`** (empty StateData) |
| 8 | CCF | `CCF` (ComputorControlledFund.h) | 127 | StateData |
| 9 | QEARN | `QEARN` (Qearn.h) | 137 | StateData |
| 10 | QVAULT | `QVAULT` (QVAULT.h) | 138 | StateData |
| 11 | MSVAULT | `MSVAULT` (MsVault.h) | 149 | StateData |
| 12 | QBAY | `QBAY` (Qbay.h) | 154 | StateData |
| 13 | QSWAP | `QSWAP` (Qswap.h) | 171 | StateData |
| 14 | NOST | `NOST` (Nostromo.h) | 172 | StateData (OldStateData for files ≤ epoch 230) |
| 15 | QDRAW | `QDRAW` (Qdraw.h) | 179 | StateData |
| 16 | RL | `RL` (RandomLottery.h) | 182 | StateData |
| 17 | QBOND | `QBOND` (QBond.h) | 182 | StateData |
| 18 | QIP | `QIP` (QIP.h) | 189 | StateData |
| 19 | QRAFFLE | `QRAFFLE` (QRaffle.h) | 192 | StateData |
| 20 | QRWA | `QRWA` (qRWA.h) | 197 | StateData |
| 21 | QRP | `QRP` (QReservePool.h) | 199 | **`sizeof(IPO)`** (real `StateData { id teamAddress; id ownerAddress; HashSet<id, QRP_ALLOWED_SC_NUM> allowedSmartContracts; }`, QReservePool.h:13-28, a few KB; in the sample the last non-zero byte is at offset 4192) |
| 22 | QTF | `QTF` (QThirtyFour.h) | 199 | StateData |
| 23 | QDUEL | `QDUEL` (QDuel.h) | 199 | StateData |
| 24 | PULSE | `PULSE` (Pulse.h) | 204 | StateData |
| 25 | VOTTUN | `VOTTUNBRIDGE` (VottunBridge.h) | 206 | StateData |
| 26 | QUSINO | `QUSINO` (Qusino.h) | 208 | StateData |
| 27 | ESCROW | `ESCROW` (Escrow.h) | 210 | StateData |
| 28 | GGWP | `WOLFPACK` (GGWP.h) | 218 | StateData |
| 29 | QPAYHUB | `QPAYHUB` (QPayhub.h) | 231 | StateData (absent in 229 set) |
| 30 | QTREAT | `QTREAT` (QTREAT.h) | 233 | StateData (absent in 229 set) |

(`INCLUDE_CONTRACT_TEST_EXAMPLES` adds TESTEXA..D after index 30; never defined in node builds. core-lite with `LITE_WASM_SC` appends 48 `LITEDYNn` slots, see §11.)

Sample sizes (bytes) of the 229 set, which must equal `sizeof(StateData)` of v1.303.2 (except 5/7/21 = 27040): 0: 8192, 1: 621806120, 2: 923559560, 3: 2459184, 4: 402895104, 5: 27040, 6: 709184, 7: 27040, 8: 493584, 9: 214171656, 10: 581841120, 11: 376448096, 12: 506986632, 13: 286326928, 14: 1030098088, 15: 32864, 16: 82040, 17: 357237432, 18: 26906696, 19: 48368712, 20: 30677232, 21: 27040, 22: 82608, 23: 177496, 24: 131368, 25: 176264, 26: 438559272, 27: 403505400, 28: 3669088 (total 6,257,491,040).

---

## 5. Size rules and the state-change mechanism — decision table for the viewer

Given a candidate schema (checkout with `EPOCH == E`), file size `F`, `S = stateSize(i)` from `contractDescriptions`, `D = sizeof(X::StateData)` (D ≤ S; D < S only for 5/7/21 and `D == 1` for empty structs), `O = sizeof(X::OldStateData)` if declared:

| condition | meaning | viewer action |
|---|---|---|
| `F == S` | normal | decode `StateData` over `[0, D)`; bytes `[D, S)` are padding (IPO remnant / zeros) — show as "unused tail" |
| `F == S` and `E == constructionEpoch(i)` | IPO result + ignored by node | decode as `IPO` (publicKeys[676], prices[676]) and label; `StateData` view is meaningless |
| `F < S` and `{i, PADDING, E}` in table | struct grew at the end | decode `StateData`, fields beyond `F` read as 0; flag "padded" |
| `F == O` (any epoch) | pre-migration layout | decode `OldStateData`; if `{i, MIGRATE, E}` present say "migrated at load" |
| `F != S`, `{i, RESET, E}` | old layout, discarded by node | decode with the writer checkout (EPOCH E-1) or show hex only |
| `F > S` | shrunk struct / stale tail (UEFI non-truncating writes) or wrong schema | decode prefix (`[0, D)`) only when the user confirms; old cores accepted this silently, HEAD rejects |
| `0 < F < S` otherwise | partial write in progress / wrong schema | do not decode; keep watching (see §8) |
| `F == 0` | truncated by `fopen("wb")`, write in progress | wait |
| file missing, `E < constructionEpoch(i)` | IPO phase, node zeroes | show "not yet constructed" |

Reference: HEAD `src/contract_core/contract_def.h:580-604` (enum `ContractStateChangeType { PADDING, RESET, MIGRATE }`, `struct ContractStateChangeInfo { unsigned int contractIndex; ContractStateChangeType changeType; unsigned short changeEpoch; }`, the active list) and `doc/contracts.md:642-660` ("state changes are currently only triggered in core if the size of the new state struct is different from the old contract state file"). A same-size layout change is impossible to detect from the files; the viewer should surface a warning when the file epoch is not the checkout's `EPOCH` (±seamless chain of the same `X.Y`).

`MAX_CONTRACT_STATE_SIZE = 1073741824` (1 GiB, `src/contract_core/pre_qpi_def.h:19`, enforced by `static_assert` in `REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES`, `contract_def.h:535`); the K12 size argument is `unsigned int`, consistent with this cap.

---

## 6. State buffers inside the node (for the mental model and for digest/dirty logic)

* `GLOBAL_VAR_DECL unsigned char* contractStates[contractCount]` and `ReadWriteLock contractStateLock[contractCount]` (`src/contract_core/contract_exec.h:57-58`); each buffer is allocated with exactly `stateSize` bytes at start-up (`src/qubic.cpp:7430-7437`), freed at shutdown (`7844-7846`). core-lite allocates page-aligned buffers for contracts that use the incremental K12 cache (`core-lite src/extensions/k12_state_digest_cache.h`).
* Contract code never sees the raw buffer: procedures get `QPI::ContractState<StateData, CONTRACT_INDEX>& state` (`src/qpi/qpi_types.h:98-107`: `sizeof(ContractState<T>) == sizeof(T)`, standard layout, `get()` const read, `mut()` marks dirty via `__markContractStateDirty(contractIndex)` → `contractStateChangeFlags[idx >> 6] |= 1 << (idx & 63)`, `contract_exec.h:69-76`). IPO bidding and contract-0 fee-reserve updates set the same flag directly (`ipo.h:102`, `qpi/impl/qpi_spectrum_impl.h:51,61,74`). The bitmap is allocated with `MAX_NUMBER_OF_CONTRACTS / 8 = 128` bytes and initialised to all ones so that every digest is computed once (`contract_exec.h:197-201`).
* Saving a contract file holds the contract's read lock (`7197-7199`), so each file is internally consistent, but different files of one set are not one atomic snapshot (except in `saveAllNodeStates()`, where the tick processor is parked).

---

## 7. Digests

### 7.1 Algorithm (core `src/qubic.cpp:819-887`, identical in core-lite `1277-1352`)

```
leaf[i]   = KangarooTwelve(contractStates[i], stateSize(i), 32)   for i < contractCount (stateSize > 0)
leaf[i]   = 0^32                                                   for contractCount <= i < 1024   (NOT K12 of empty input)
level k+1 = KangarooTwelve64To32(level_k[2j] || level_k[2j+1])     (K12 of the 64 concatenated bytes -> 32 bytes)
computerDigest = contractStateDigests[2*1024 - 2]  (array layout: 1024 leaves, then 512, 256, ..., 1; total 2047 entries)
```

Only dirty leaves/inner nodes are recomputed (`contractStateChangeFlags`), results persist in `static m256i contractStateDigests[MAX_NUMBER_OF_CONTRACTS * 2 - 1]` (`src/qubic.cpp:185-186`). `getComputerDigest()` runs in the tick processor at the end of every tick (`4522`, salted digest) and at start-up after loading the files (`7631`). With `START_NETWORK_FROM_SCRATCH`, the first tick of the epoch publishes the digest of the loaded files as `prevComputerDigest` in every computor's vote (`4076-4087`); the same value is logged at start-up as `Computer digest = <60-char identity>`.

### 7.2 KangarooTwelve variant

Core's `KangarooTwelve()` (`src/kangaroo_twelve.h:1394-1530`, `K12_chunkSize 8192`, `K12_suffixLeaf 0x0B`, security 128) is standard **KT128 with an empty customization string** (XKCP wrapper comment: "Wrapper around `KangarooTwelve` to use the 128-bit security level and no customization String", `src/K12/kangaroo_twelve_xkcp.h:425-430`; the unit test `test/kangaroo_twelve.cpp` asserts the native and XKCP outputs are equal). `KangarooTwelve64To32(in, out)` is the same function specialised for 64-byte input (`0x0700` constants at `kangaroo_twelve.h:1541,1828`: byte 64 = `length_encode(0)`, byte 65 = domain byte 0x07).

Reference structure (RFC 9861): `S = M || 0x00`; if `|S| ≤ 8192` the digest is `TurboSHAKE128(S, D=0x07)`; otherwise `TurboSHAKE128(S_0 || 0x03 || 0x00^7 || CV_1 || … || CV_n || length_encode(n) || 0xFF 0xFF, D=0x06)` with `CV_j = TurboSHAKE128(S_j, D=0x0B, 32)` over consecutive 8192-byte chunks. Note that `contract0000` (8192 bytes) hits the tree edge case (`|S| = 8193` → one 1-byte leaf containing the 0x00).

Spike verification (`06-spike/k12.hpp`, ~150 lines of C++17, no dependencies): passes the eight KT128 test vectors (ptn(17^k), k = 0..6) and matches core's `KangarooTwelve()`/`KangarooTwelve64To32()` compiled from `src/kangaroo_twelve.h` for 21 sizes including 0, 8191, 8192, 8193, 27040, 5,000,000 (`06-spike/k12_vs_qubic.cpp`, build line in the file header). Throughput of the scalar implementation: ≈1.3 GB/s per core; leaf chunks are independent, so 8 threads hash the 6.26 GB set in ≈5 s from page cache (`06-spike/computer_digest.cpp`).

### 7.3 Identity string of a digest

`getIdentity(digest, out, isLowerCase=true)` (`src/four_q.h:1777-1795`): for each of the 4 little-endian `uint64` words emit 14 base-26 characters (least significant first, `'a' + v % 26`), then 4 checksum characters from `K12(digest, 32 bytes → 3 bytes) & 0x3FFFF` in base 26. Digests are printed lower-case, public keys upper-case.

### 7.4 Where the node persists digests

* `ep<E>/snapshotComputerDigest`: the whole `contractStateDigests` array, 2047 × 32 = 65,504 bytes (`5468-5475`); entry `i` (offset `32*i`) is the K12 of `contractNNNN.000` in the same snapshot (digests are current because the snapshot is taken between ticks after `getComputerDigest()` of the previous tick). Entry 2046 is the computer digest. `snapshotSpectrumDigest` and `snapshotUniverseDigest` are the analogous trees (2^25-1 entries × 32 = 1,073,741,792 bytes each).
* The viewer can therefore self-check a snapshot directory: `K12(contractNNNN.000) == snapshotComputerDigest[NNNN]`.

### 7.5 Spike results for the 229 sample set (`06-spike/digest_229.txt`)

```
contract0000.229  9de1e4d7b943b2dcdd84d154402cd2a17bfeac3fb325e2d720e3f621a4e30d2e
contract0001.229  a33de990749ed9225cd3e1984cfe68edb8d073c9be2baeb2fa862b6b96f47cce
contract0002.229  c92a726a6b29c7f598c243baec5ae0edc8babe56e2a28e0ad7167c3e056c9224
contract0003.229  15a04512b35c5b7dcc6eb1b446cc289fd99a9cfaed28c1458371d40cc51cf4ea
contract0004.229  8b222122f4baa10e3ba05c078d118e62404e5129281fbcd2ceb29c0bcee94499
contract0005.229  faf32534e874e0de06f0ac8127f7d578b778314a6dd663dae5e480ea5ae673a7   (27040 zero bytes; same as 0007)
contract0006.229  956430965519b1d294c01c299927eb078e689a71fac5f03ef4011d0d9b0841d7
contract0007.229  faf32534e874e0de06f0ac8127f7d578b778314a6dd663dae5e480ea5ae673a7
contract0008.229  ee3159556284c056c29153e765242ed244e6d9fe08b59faf702c7a120a11b5ea
contract0009.229  4953fa6544ef47c9e3b921ab4ef0e6c1f2f381aa020e77a491bf55fed661fa34
contract0010.229  bd18faa625f9383f65f657856ea708b3f9ad6153526e9ffbd9af7697403d68f1
contract0011.229  f381b1d55c3fde5cd2d37366014963a79d24554de43e761af25d41130ec23488
contract0012.229  9aa9ad3a6ce9f7fc06be1896edb6e75973e030d686f88ba2d71cd8e95a3a2888
contract0013.229  b7095be20b73375587799dd8787f8402694cd0e006c253a77cb01789e4991378
contract0014.229  3768695a0da5272f08d351ff19e378bac0eff683cf02c54bfb227766ed0f66b7
contract0015.229  ce683d9a74c6d9451c3e766e007e00b5340df3eb41485981e94f8501c0aa2678
contract0016.229  0fddad704e0be49d3c3bf8b789a6d08df0ce7e72c0fa35318e5666e84615a6e6
contract0017.229  14a441b1bc84762bc362b709848c4d8a1e6069bdbbe8fee1afb5f4546353229b
contract0018.229  bb6d62518cff0ab10d5d43abd4374123258e593df3468bb871570ca494ddacd5
contract0019.229  ce83315801c97c57798c016854c6b1d43192addf416d629b8187af1c64424763
contract0020.229  af04508d3bc4dcb47ecd2624e94e4cd6457cb849334990d70f2a429aba508d68
contract0021.229  9d1b8ffd0918339b347944fc66acc91329b567e50389d6e344f3d913506d88b4
contract0022.229  48b1ff887cc9289411f179e496b13838e1a4d854e4baac63a763d752957ac1c0
contract0023.229  5317f17953213f4aa728541612f7d3e66b3657e671eb4af9fe47504d4b57a211
contract0024.229  c7963b0127e0cdf1e5545a0651225ff2a2f798df9462615bbdc55eaf389f2bcb
contract0025.229  c3de858e00f105dd7913e94510d6787a836497a52f8f1e019ad034f672af423c
contract0026.229  60a32be921dbf40144270a0ef3ff49561be13fc3929d3371acb79c7845037eeb
contract0027.229  3bbbef284d808902ba8524b4270ae48b1dd25b77fedfdd60b843c30f3a725450
contract0028.229  9f9ccbb1008713a55009b20b48bfbc85910943d64e85d699472b2a6cc278a9a4
computer digest (hex)      = 9271f1eca4512e52bc9f0b40d343e4d8cf42f5ffbaf5ec5c6a4b3391cfd8155a
computer digest (identity) = kyefkvylhpkbkcikgnzscucpcuhgzjdqwsfpidjescouzahdeonipaqctgui
```

These are deterministic functions of the files; they could be cross-checked against `prevComputerDigest` of tick 77700000 (not exposed by the public RPC endpoints I tried, see §14).

### 7.6 Chunk digests as a change-detection device

core-lite's `K12StateDigestCache` (`core-lite src/extensions/k12_state_digest_cache.h`) exploits the K12 tree: it caches the 32-byte CV of every 8192-byte chunk and re-hashes only dirty chunks (bit-identical result). The viewer can do the same between two versions of a file: keep `ceil(size/8192)` CVs (0.4 % of the state size, 4 MB for a 1 GB state), recompute per chunk in parallel on reload, and compare → the set of changed 8 KiB chunks, which maps to changed fields / container slots for reactive highlighting without keeping the previous 1 GB in memory. `06-spike/k12.hpp::leafCV()` / `hashFromCVs()` implement exactly this split.

---

## 8. Write pattern and a robust watcher

### 8.1 How bytes hit the disk

| | core (UEFI) | core-lite (Linux / macOS / Windows) |
|---|---|---|
| open | `EFI_FILE_PROTOCOL.Open(READ\|WRITE\|CREATE)` on root or `ep<E>` dir (`file_io.h:348-376`); existing file is **not truncated** | `fopen(path, "wb")` → **truncates to 0** immediately (`core-lite src/platform/file_io.h:461-475`, `host_file.h`); on MSVC builds `_wfopen_s` opens the file **non-shareable** (other processes get `ERROR_SHARING_VIOLATION` until `fclose`) |
| write | sequential `Write()` in 32768-byte chunks (`WRITING_CHUNK_SIZE`), file position starts at 0 | one `fwrite(buffer, 1, totalSize)` (glibc issues direct `write()` syscalls for the bulk, buffers < 4 KiB tail), then `fclose()` |
| size during write | old size until the write passes it, then grows; shorter new content leaves a stale tail (`F > S`) | 0, then grows monotonically to `totalSize`; final size exact |
| completion signal | none; `getFileSize == expected` and stable | `fclose` → Linux inotify `IN_CLOSE_WRITE`; size == expected |
| order within a set | spectrum → universe → contract 0..N-1 → fee report (epoch transition / F6); snapshot: invalid metadata → spectrum → universe → index → contracts → fee rec/acc → system.snp → … → tick storage → valid metadata → ant/oracle/OC/log files | same, but the whole snapshot is staged in `ep<E>.tmp` and appears atomically via rename |
| duration | 8 GB on NVMe/FAT32: tens of seconds; the main thread is blocked meanwhile | sample set: 8.1 GB in ~3 s (page cache); disk-bound otherwise |

Deleted files: none during normal operation; core-lite removes `ep<E>.old` after promotion and `ep<E>.tmp` before a new snapshot (`6252`, `6472-6480`). The long-run local testnet never deletes old epoch files (`core-lite doc/long_run_local_testnet.md`, "Old-epoch state files").

### 8.2 Watcher design (recommendation)

```
watch(parentDir):                      # parentDir = directory the user opened (node cwd, extracted zip dir, or ep<E>)
  Linux: inotify on parentDir (IN_CREATE|IN_MODIFY|IN_CLOSE_WRITE|IN_MOVED_FROM|IN_MOVED_TO|IN_DELETE|IN_DELETE_SELF|IN_MOVE_SELF)
         plus one watch per recognised ep<E> subdirectory (re-added whenever IN_MOVED_TO/IN_CREATE names ep<E>, because the inode changes)
  Windows: ReadDirectoryChangesW(FILE_NOTIFY_CHANGE_FILE_NAME|DIR_NAME|SIZE|LAST_WRITE, bWatchSubtree=TRUE); open files with
         FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE and retry ERROR_SHARING_VIOLATION with backoff (MSVC fopen_s writer)
  macOS: FSEvents or kqueue per directory; same settle logic

on event for a recognised name N (regexes in §2.1):
  mark N dirty; (re)arm settle timer for N: 300 ms for files < 64 MiB, 1000 ms otherwise
on timer(N):
  st = stat(N) (follow renames by path, never by cached handle)
  if st.size == 0 or st.size < expectedSize(N):   # expectedSize from schema or alternative sizes of §5
      re-arm timer (write in progress or truncated); after 30 s show "incomplete (size X of Y)"
  elif st.size == expectedSize and (st.size, st.mtime_ns, st.ino) unchanged since the previous timer run:
      accept: reload file -> decode -> compute chunk CVs -> diff against previous CVs -> publish change set
  else re-arm
IN_CLOSE_WRITE (Linux): run the timer body immediately (still verify size)

directory events:
  ep<E>.tmp appears      -> show "snapshot in progress" badge, do not read from it
  IN_MOVED_TO ep<E>      -> treat every file inside as new; add watch on the new directory
  IN_MOVE_SELF / IN_DELETE_SELF on a watched ep<E> -> drop its watch; wait for the replacement
  for in-place (UEFI-style) snapshot dirs: read snapshotMetadata.EEE (32 bytes) and require epoch != 0 before trusting the set

set-level completion heuristics:
  epoch set E (names *.EEE): complete when contract_exec_fees_rec.EEE (last writer) is stable, or all contract files 0..N-1 are stable
  snapshot dir: complete when it was renamed into place (core-lite) or metadata.epoch == E (core)
```

Hard rules learned from the code:

1. **Never hold long-lived handles or mappings on files inside a watched directory on Windows**: core-lite promotes a snapshot with `std::filesystem::rename(ep<E>.tmp → ep<E>)` after renaming the old `ep<E>` away; an open handle without `FILE_SHARE_DELETE` inside `ep<E>` makes the rename fail and the node logs "Failed to promote snapshot" (`6474-6478`) — the viewer would break the node's persistence. Read into memory (or map, copy, unmap) and close immediately.
2. On Linux a mapping stays valid but points at the **old inode** after the rename; re-open by path on every accepted change and compare `(st_dev, st_ino, size, mtime)`.
3. Treat any size that is not one of the expected sizes (§5) as "not ready"; `fopen("wb")` guarantees that partial files are shorter than the final file.
4. Epoch files are rewritten in place at the next transition **with a new name**, so a `.233` set never changes after completion; only `.000` files and `ep<E>/` are overwritten (F6 re-saves overwrite the same `.000` names in place: expect the truncate-then-grow pattern again).

---

## 9. Sibling files: layouts in brief

All structs are plain little-endian x86-64 dumps (`save(name, sizeof(x), &x)`).

| file | struct / element | element size | count / file size | writer (core HEAD) |
|---|---|---|---|---|
| `spectrum.EEE` | `EntityRecord { m256i publicKey; long long incomingAmount, outgoingAmount; unsigned int numberOfIncomingTransfers, numberOfOutgoingTransfers; unsigned int latestIncomingTransferTick, latestOutgoingTransferTick; }` (`src/network_messages/entity.h:5-16`) | 64 | `SPECTRUM_CAPACITY = 1 << 24` → 1,073,741,824 bytes; open-addressing hash table, slot = `publicKey.m256i_u32[0] & (CAPACITY-1)`, linear probing, empty = zero key (`src/spectrum/spectrum.h:248-280`); balance = incoming − outgoing | `saveSpectrum()` `spectrum.h:527-547` |
| `universe.EEE` | `AssetRecord` union: issuance `{ m256i publicKey; u8 type=1; char name[7]; char numberOfDecimalPlaces; char unitOfMeasurement[7]; }`, ownership `{ m256i publicKey; u8 type=2; char pad; u16 managingContractIndex; u32 issuanceIndex; i64 numberOfShares; }`, possession `{ … type=3 … u32 ownershipIndex; i64 numberOfShares; }` (`src/network_messages/assets.h:18-57`) | 48 | `ASSETS_CAPACITY = 0x1000000` → 805,306,368 bytes; hash slot = `publicKey.m256i_u32[0] & (CAPACITY-1)` with probing (`src/assets/assets.h:146-157, 204-282`) | `saveUniverse()` `assets.h:809-831` |
| `contract_exec_fees_rec.EEE` | `ExecutionFeeReportCollector { unsigned long long executionFeeReports[contractCount][676]; }` — per contract, per computor: last reported execution fee (microseconds-derived) of the current reporting phase; row 0 unused | 8 | `contractCount × 676 × 8` (29 → 156,832) | `saveToFile()` `src/ticking/execution_fee_report_collector.h:158-166`; written at transition/F6/snapshot (`7222`) |
| `contract_exec_fees_acc.EEE` | `ExecutionTimeAccumulator { unsigned long long contractExecutionTimePerPhase[2][contractCount]; bool activeIndex; volatile char lock; }` | — | `16·contractCount + 8` (29 → 472, 31 → 504) | `src/contract_core/execution_time_accumulator.h:80-88`; **snapshots only** (`saveAccumulatedTime=true` only in `saveAllNodeStates`, `5382`) |
| `contract0000.EEE` | `Contract0State { long long contractFeeReserves[1024]; }` (`contract_def.h:385-388`) | 8 | 8192 | with the other contracts |
| `system`, `system.eoe`, `ep<E>/system.snp` | `System { short version; unsigned short epoch; unsigned int tick; unsigned int initialTick; unsigned int latestCreatedTick; unsigned int latestLedTick; unsigned short initialMillisecond; unsigned char initialSecond, initialMinute, initialHour, initialDay, initialMonth, initialYear; [pad 4]; unsigned long long latestOperatorNonce; unsigned int numberOfSolutions; [pad 4]; Solution solutions[65536] (104 B each: 3×m256i + u32 score + u32 reserved); m256i futureComputors[676]; }` (`src/system.h:12-44`) | — | 6,837,424 bytes; `epoch` at offset 2 (u16), `tick` at 4 (u32), `initialTick` at 8 | `saveSystem()` `7237-7254` (`.eoe` when `epochTransitionState == 1`); `system.snp` `5391-5392` |
| `ep<E>/snapshotMetadata.EEE` | `TickStorage::MetaData { u32 epoch; u32 tickBegin; u32 tickEnd; [pad 4]; i64 outTotalTransactionSize; u64 outNextTickTransactionOffset; }` (`src/ticking/tick_storage.h:103-110`) | — | 32; all-zero = snapshot invalid/in progress | `tick_storage.h:126-136, 419-431` |
| `ep<E>/snapshotComputerDigest` | `m256i contractStateDigests[2047]` | 32 | 65,504 | `5468-5475` |
| `ep<E>/snapshotSpectrumDigest`, `snapshotUniverseDigest` | `m256i[2^25 − 1]` | 32 | 1,073,741,792 each | `5450-5466` |
| `ep<E>/snapshotNodeMiningState` | anonymous `nodeStateBuffer` (`etalonTick`, miner tables, vote counter data, …) (`src/qubic.cpp:649-670`) | — | version-specific | `5420-5427` |
| `ep<E>/snapshotUniverseIndex` | `as.indexLists` (`assets.h:852-862`) | — | version-specific | |
| `revenue_data.EEE` / `.eoe`, `revenue_data_multi.EEE` / `.eoe` | `EpochRevenueData` (`src/revenue.h:403-423`), `MultiDimRevenue` (`revenue.h:573-585`) | — | version-specific; loader remaps other sizes (`5653-5667`) | `5430-5448`, `6889-6891` |
| `score.EEE`, `antColonyReplayCache.EEE` | score caches (`src/score_cache.h:184-193`, `mining/ant_colony/ant_colony.h:827`) | — | node-local, not consensus | periodic / shutdown / snapshot |
| `ep<E>/snapshotTickdata.EEE[.NNN]`, `snapshotTicks.EEE`, `snapshotTickTransactionOffsets.EEE`, `snapshotTickTransaction.EEE` | tick storage arrays (`TickData`, `Tick`, offsets, transactions); files ≥ 200 MB are split into `name.000`, `name.001`, … (`saveLargeFile`, `file_io.h:1308-1338`) | — | huge | `tick_storage.h:138-205` |
| `ep<E>/snapshotOracle*.EEE`, `snapshotOc*.EEE`, `snapshotAntColony*.EEE`, `snapshotMinerSolutionFlag`, `snapshotAntSolutionFlag`, `logEventState.db` | engine snapshots (`src/oracle_core/snapshot_files.h:10-15`, `src/oc_core/snapshot_files.h:10-14`, `ant_colony.h:1346-1362`, `logging/logging.h:724`) | — | — | |

---

## 10. The contract 0 file

`contract0000.EEE` is the 8192-byte array `long long contractFeeReserves[MAX_NUMBER_OF_CONTRACTS]` indexed by contract index: the execution-fee reserve (in QU) each contract can spend; ≤ 0 means the contract's procedures are skipped (`contractProcessor` checks `getContractFeeReserve(i) <= 0`, `src/qubic.cpp:2867,2894,3782`; `doc/execution_fees.md`). Accessors `getContractFeeReserve / setContractFeeReserve / addToContractFeeReserve / subtractFromContractFeeReserve` live in `src/qpi/impl/qpi_spectrum_impl.h:36-86`; writers: IPO completion (`ipo.h:185`, `finalPrice × 676`), burns (`QpiContextProcedureCall::burn`), and the fee report quorum deduction every 676 ticks (`execution_fee_report_collector.h:140-155`, logged as `ContractReserveDeduction`). Contract 0 has no code, no digest exemption (it is leaf 0 of the computer digest) and is never in IPO. Sample decode of `contract0000.229`: 28 non-zero entries at indices 1..28 (e.g. `[1] = 10,082,305,477,318`, `[7] = 17,784,884,570,411`), everything from 29 on zero. The viewer should render it as a table `index → name → reserve` using the chosen schema's `contractDescriptions`.

---

## 11. core-lite specifics and differences from core

Same persistence code (file names, triggers, formats, digest), with these differences:

1. **Host file I/O** (`core-lite src/platform/file_io.h`): `getFileSize` via `std::filesystem::file_size` (84-100), `load` via `fopen("rb")`+`fread` (369-394), `save` via `fopen("wb")`+`fwrite`+`fclose` (458-487), new `removeDir`/`renameDir` (317-366). Paths are cwd-relative; `CHAR16` is 16-bit `wchar_t` (`-fshort-wchar`), converted with `wchar_to_string`.
2. **Snapshot atomicity**: `ep<E>.tmp` staging + rename promotion (`6228-6483`); old directory kept as `ep<E>.old` until the promotion succeeded. Snapshot persistence mode: `TICK_STORAGE_AUTOSAVE_MODE 2` (manual/remote only), period 1337 if mode 1 (`private_settings.h:1453-1466`). Triggers: F8, `SPECIAL_COMMAND_SAVE_SNAPSHOT` (`2886-2908`), `GET http://localhost:41841/request-save-snapshot` (no passcode, `rpc_routes.h:164-171`). `GET /tick-info` returns `{epoch, tick, initialTick, isSavingSnapshot, …}` (`rpc_routes.h:82-95`); `GET /spectrum` and `/universe` (passcode) serve the current epoch files, optionally zipped into `.qubic-tmp/` (`rpc_routes.h:50-74, 173-183`).
3. **Fork-based tick rollback** (`extensions/tick_fork_*.h`, `disk_shadow.h`): the parent process forks per tick; writes to *registered* swap-page directories are diverted to `<dir>/s/` and committed by rename. Contract state files and `ep<E>.tmp` are **not** registered (only `VirtualMemory` page dirs register, `virtual_memory.h:441`), so epoch/snapshot files are unaffected; the viewer only has to ignore `*/s/`.
4. **Epoch switching**: plain `TESTNET` → every `TESTNET_EPOCH_DURATION = 2701` ticks (`core-lite src/public_settings.h:238`; README says 3000); `LONG_RUN_LOCAL_TESTNET` (this checkout) → `LONG_RUN_EPOCH_TICK_CAPACITY = 5,184,244` ticks (≈60 days at 1 s/tick) or F7 / force-switch special command (`8396-8410`). Mainnet builds use the Wednesday 12:00 UTC rule. Each transition writes a fresh `.E+1` set into the cwd (sample set ≈ 8.1 GB, though contract states of a fresh testnet compress well) and never deletes old sets.
5. **Start without files** (`TESTNET`): `canObmitLoadNodeState = true` (`9267-9271`), so a failing `loadSpectrum/loadUniverse/loadContractStateFiles` is tolerated and the node starts with zero states; `loadContractStateFiles()` returns at the first missing file (contract 0), leaving all states as allocated, i.e. zero (`allocPoolWithErrorLog` zero-fills, `core-lite src/platform/memory_util.h:82,159`); only contracts with `constructionEpoch == EPOCH` get `INITIALIZE`. Shares for all contracts are issued to the 676 testnet computors if the universe is empty (`9370-9395`). The README's "no initial files are needed" refers to this.
6. **`LITE_WASM_SC` builds** (off here; requires `TESTNET` + `TESTNET_LITE_RAM`): 48 extra reserved slots `LITEDYN0..47` at indices `contractCount..contractCount+47` with `stateSize = sizeof(LITEDYNn::StateData)` (`core-lite src/contract_core/contract_def.h:358-807, 894-943`); files are saved with `Wasm::Runtime::effectiveStateSize()` (`8846-8850`, `extensions/wasm/runtime/engine_state.h:118-127`), so sizes are not derivable from the headers. These builds also expose `GET /live/v1/dev/state-bytes?slot=&off=&len=` (raw bytes, header `X-State-Size`), `/live/v1/dev/state-read` (hex JSON with a seqlock `version`), `/live/v1/dev/contract-digest?slot=` (K12 of the effective state) and `/live/v1/dev/epoch-info` (`rpc_live_controller.h:832-949`, inside `#ifdef LITE_WASM_SC`, lines 523-1356). A future "HTTP live source" for the viewer can use them; the default build does not have them.
7. Startup/argument differences: `--peers`, `--node-mode`, `--security-tick`, `--http-port` (default 41841), `--tick-duration` (long-run), `--max-sc-mem` (pager builds) (`11877-11937`). Port 31841 (TESTNET) / 21841 for the node protocol (`247-253`).
8. core-lite reads keys from the terminal through a UEFI-console shim (`processKeyPresses`, `10191`), so F6/F7/F8 work in an interactive terminal only; the HTTP route is the scriptable way to get a fresh snapshot.
9. The Linux K12 state-digest cache (`k12_state_digest_cache.h`, default on, `--no-k12-state-cache`) changes nothing about the digest values.

Observed state on this machine: no core-lite binary and no runtime directories exist yet (CMake cache configured for clang++-20 with `LONG_RUN_LOCAL_TESTNET=ON`), so the behaviours above are from code, not from running the node.

---

## 12. Auto-detecting "this directory holds epoch E state files" and mapping files to contracts

```
scan(dir):
  entries = list(dir) (one level; also peek into subdirs matching ^ep\d+(\.tmp|\.old)?$ and offer them as separate sets)
  for each entry matching ^contract(\d{4})\.(\d{3})$: sets[ext].contracts[int(idx)] = {path, size, mtime}
  also record spectrum.ext, universe.ext, contract_exec_fees_rec.ext, contract_exec_fees_acc.ext, system, system.eoe, system.snp,
       snapshotMetadata.ext, snapshotComputerDigest, revenue_data*.ext (same ext family), *.zip
  for each ext in sets:
    indices must be contiguous 0..N-1 (N = count); gaps -> warn, still usable per file
    writerContractCount = N; if contract_exec_fees_rec.ext exists: assert size % 5408 == 0 and size/5408 == N (else warn "mixed sets")
    epoch =
      ext != "000"                      -> int(ext)                         (regard as epoch % 1000)
      dir name matches ^ep(\d+)         -> that number                      (snapshot)
      system.snp / system / system.eoe  -> uint16 at offset 2 (+ tick at 4)  (system.eoe = end of previous epoch: epoch already incremented)
      snapshotMetadata.EEE present      -> EEE (and MetaData.epoch/tickEnd inside, 0 = incomplete snapshot)
      otherwise                         -> ask the user; default to the newest schema whose contractCount == N
    kind = ext != "000" ? "epoch-start set (or mid-epoch restart set)" : dir is ep<E> ? "node snapshot after tick <system.snp.tick - 1>" : "F6 manual save"
    schema = checkout/tag with #define EPOCH == epoch (fall back: nearest tag with EPOCH <= epoch and the same contractCount);
             verify every file size against the §5 table; report the per-contract verdict (ok / padded / old layout / IPO / unknown)
  if several ext groups exist (node cwd after transitions): present them as versions, newest epoch first
  zip files (ep229-full.zip): read the central directory only; offer extraction to a cache dir (entries are deflated; 1 GB files) — do not inflate into RAM on demand for the big ones
```

Mapping `NNNN → contract`: index into the schema's `contractDescriptions[]` (§4.4) to get asset name, `CONTRACT_STATE_TYPE`, header file, `constructionEpoch`, `stateSize` expression; the asset name is what the node and explorers display (QTRY for QUOTTERY, VOTTUN for VOTTUNBRIDGE, GGWP for WOLFPACK, MLM for MyLastMatch).

For a core-lite working directory the viewer should additionally watch for `ep<E>` and `ep<E>.tmp` subdirectories (snapshots) and show the tick from `ep<E>/system.snp` (`tick` − 1 is the last processed tick) next to each snapshot; for epoch sets it should display "state at start of epoch E" and the epoch's wall-clock start (`2022-04-13 12:00 UTC + 7·E days`) for mainnet files.

Ignore list: directories `^[a-z]{4}\.\d{3}$` with page files, `s/`, `.qubic-tmp/`, `efi/`; files `bpp9000.task`, `debug.log`, `profiling.csv`, `crash.dump`, `*.efi`, `startup.nsh`.

---

## 13. Spike artifacts (all under `/tmp/claude-1000/-home-yeti-devwork-space/51418eaa-0459-4cf2-bc49-43ecdf26d5ce/scratchpad/research/06-spike/`)

| file | purpose |
|---|---|
| `k12.hpp` | standalone KT128 (empty customization) with one-shot `hash()`, per-chunk `leafCV()`, `hashFromCVs()`, `hash64to32()`; C++17, no deps, ~1.3 GB/s/core |
| `k12_vectors.cpp` | RFC 9861 KT128 test vectors (8/8 pass): `g++ -std=c++17 -O3 -o k12_vectors k12_vectors.cpp && ./k12_vectors` |
| `k12_vs_qubic.cpp` | bit-compares `k12.hpp` with core's `KangarooTwelve`/`KangarooTwelve64To32` compiled from `src/kangaroo_twelve.h`: `g++ -std=c++20 -O2 -mavx2 -mbmi -mbmi2 -mlzcnt -mrdrnd -fpermissive -w -DNO_UEFI -I<core> -I<core>/src -o k12_vs_qubic k12_vs_qubic.cpp` (defines `setMem`/`copyMem` shims) → ALL MATCH |
| `computer_digest.cpp` | multithreaded per-contract digests + Merkle root + identity string for a directory: `g++ -std=c++17 -O3 -pthread -o computer_digest computer_digest.cpp && ./computer_digest /home/yeti/devwork/space/229 229 8` (5.1 s) |
| `digest_229.txt` | output for the sample set (§7.5) |
| `nost_state_v303.txt`, `nost_oldstate_head.txt` | textual proof that HEAD `NOST::OldStateData` == v1.303.2 `NOST::StateData` |

---

## 14. Open questions / risks

1. End-to-end confirmation of the computer digest `9271f1ec…` against the network's `prevComputerDigest` of tick 77700000 was not possible: `rpc.qubic.org` returns 404 for `/v1/ticks/{t}/quorum-tick-data` and the `/query/v1/*` vote endpoints I tried; `tickData` for 77700000 is null. A node's debug log line `Computer digest = kyefkvylhpkb…` from the epoch-229 start would confirm it (the identity string is deterministic).
2. Same-size layout changes between the writer and the reader of a file are undetectable from the files; the viewer should warn whenever the file epoch and the checkout's `EPOCH` differ by more than a seamless chain.
3. The epoch extension has only three digits; nothing in core handles epoch ≥ 1000 yet.
4. `LITE_WASM_SC` core-lite builds write extra slot files whose size is runtime-defined (not derivable from headers).
5. Windows-specific behaviour (MSVC `fopen_s` non-shared opens, directory rename blocked by open handles) is derived from the code and MSVC documentation, not observed here.
6. Mid-epoch network restarts (`TICK_IS_FIRST_TICK_OF_EPOCH 0`, e.g. v1.295.3 with `TICK 58257600`) ship `.EEE` files that are *not* start-of-epoch states (they are F6 `.000` saves renamed by operators); the only tell-tale is the non-round `TICK` of the corresponding tag.
