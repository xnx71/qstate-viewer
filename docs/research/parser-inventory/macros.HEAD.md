## macro definitions on the include path (384 definitions in 18 files)

| macro | kind / features | defined at | expands to (truncated) | uses other macros | use count by context (outside directives) |
|---|---|---|---|---|---|
| `PRAGMA(x)` | function-like(1 params), redefined 1 more time(s) | `lib/platform_common/compiler_warnings.h:19` | `__pragma(x) // MSVC uses __pragma instead of _Pragma` |  |  |
| `WARNING_PUSH` | object-like, redefined 2 more time(s) | `lib/platform_common/compiler_warnings.h:20` | `__pragma(warning(push))` |  |  |
| `WARNING_POP` | object-like, redefined 2 more time(s) | `lib/platform_common/compiler_warnings.h:21` | `__pragma(warning(pop))` |  |  |
| `WARNING_IGNORE_CAST_ALIGN` | object-like, redefined 2 more time(s) | `lib/platform_common/compiler_warnings.h:22` | `// No direct common MSVC equivalent for -Wcast-align, often handled differently or via oth…` |  |  |
| `WARNING_IGNORE_UNUSED` | object-like, redefined 2 more time(s) | `lib/platform_common/compiler_warnings.h:23` | `__pragma(warning(disable: 4100)) // C4100: 'identifier' : unreferenced formal parameter` |  |  |
| `WARNING_IGNORE_SELFASSIGNMENT` | object-like, redefined 2 more time(s) | `lib/platform_common/compiler_warnings.h:24` | `// No direct common MSVC equivalent for -Wself-assign-overloaded` |  |  |
| `WARNING_IGNORE_CONVERSION_DATALOSS` | object-like, redefined 2 more time(s) | `lib/platform_common/compiler_warnings.h:25` | `__pragma(warning(disable: 4310)) // C4310: cast truncates constant value` |  |  |
| `SUPPRESS_WARNINGS_BEGIN` | object-like | `lib/platform_common/compiler_warnings.h:46` | `WARNING_PUSH` | WARNING_PUSH |  |
| `SUPPRESS_WARNINGS_END` | object-like | `lib/platform_common/compiler_warnings.h:47` | `WARNING_POP` | WARNING_POP |  |
| `IGNORE_CAST_ALIGN_WARNING` | object-like | `lib/platform_common/compiler_warnings.h:48` | `WARNING_IGNORE_CAST_ALIGN` | WARNING_IGNORE_CAST_ALIGN |  |
| `IGNORE_UNUSED_WARNING` | object-like | `lib/platform_common/compiler_warnings.h:49` | `WARNING_IGNORE_UNUSED` | WARNING_IGNORE_UNUSED |  |
| `IGNORE_SELFASSIGNMENT_WARNING` | object-like | `lib/platform_common/compiler_warnings.h:50` | `WARNING_IGNORE_SELFASSIGNMENT` | WARNING_IGNORE_SELFASSIGNMENT |  |
| `IGNORE_CONVERSION_DATALOSS_WARNING` | object-like | `lib/platform_common/compiler_warnings.h:51` | `WARNING_IGNORE_CONVERSION_DATALOSS` | WARNING_IGNORE_CONVERSION_DATALOSS |  |
| `STDINT_USING_STD` | object-like, redefined 1 more time(s) | `lib/platform_common/qstdint.h:9` | `1` |  |  |
| `FALSE` | object-like | `lib/platform_efi/uefi.h:5` | `((BOOLEAN)0)` |  | parenthesized/args:9 |
| `IN` | object-like | `lib/platform_efi/uefi.h:6` | `` |  | parenthesized/args:240 |
| `OPTIONAL` | object-like | `lib/platform_efi/uefi.h:7` | `` |  | parenthesized/args:38 |
| `OUT` | object-like | `lib/platform_efi/uefi.h:8` | `` |  | parenthesized/args:77 |
| `TRUE` | object-like | `lib/platform_efi/uefi.h:9` | `((BOOLEAN)1)` |  | parenthesized/args:2 |
| `EFI_SUCCESS` | object-like | `lib/platform_efi/uefi.h:11` | `0` |  | parenthesized/args:3, other:1 |
| `EFI_LOAD_ERROR` | object-like | `lib/platform_efi/uefi.h:12` | `(1 \| 0x8000000000000000)` |  | other:1 |
| `EFI_INVALID_PARAMETER` | object-like | `lib/platform_efi/uefi.h:13` | `(2 \| 0x8000000000000000)` |  | other:1, parenthesized/args:1 |
| `EFI_UNSUPPORTED` | object-like | `lib/platform_efi/uefi.h:14` | `(3 \| 0x8000000000000000)` |  | other:1 |
| `EFI_BAD_BUFFER_SIZE` | object-like | `lib/platform_efi/uefi.h:15` | `(4 \| 0x8000000000000000)` |  | other:1 |
| `EFI_BUFFER_TOO_SMALL` | object-like | `lib/platform_efi/uefi.h:16` | `(5 \| 0x8000000000000000)` |  | parenthesized/args:2, other:1 |
| `EFI_NOT_READY` | object-like | `lib/platform_efi/uefi.h:17` | `(6 \| 0x8000000000000000)` |  | other:1 |
| `EFI_DEVICE_ERROR` | object-like | `lib/platform_efi/uefi.h:18` | `(7 \| 0x8000000000000000)` |  | other:1 |
| `EFI_WRITE_PROTECTED` | object-like | `lib/platform_efi/uefi.h:19` | `(8 \| 0x8000000000000000)` |  | other:1 |
| `EFI_OUT_OF_RESOURCES` | object-like | `lib/platform_efi/uefi.h:20` | `(9 \| 0x8000000000000000)` |  | other:1 |
| `EFI_VOLUME_CORRUPTED` | object-like | `lib/platform_efi/uefi.h:21` | `(10 \| 0x8000000000000000)` |  | other:1 |
| `EFI_VOLUME_FULL` | object-like | `lib/platform_efi/uefi.h:22` | `(11 \| 0x8000000000000000)` |  | other:1 |
| `EFI_NO_MEDIA` | object-like | `lib/platform_efi/uefi.h:23` | `(12 \| 0x8000000000000000)` |  | other:1 |
| `EFI_MEDIA_CHANGED` | object-like | `lib/platform_efi/uefi.h:24` | `(13 \| 0x8000000000000000)` |  | other:1 |
| `EFI_NOT_FOUND` | object-like | `lib/platform_efi/uefi.h:25` | `(14 \| 0x8000000000000000)` |  | other:1 |
| `EFI_ACCESS_DENIED` | object-like | `lib/platform_efi/uefi.h:26` | `(15 \| 0x8000000000000000)` |  | other:1 |
| `EFI_NO_RESPONSE` | object-like | `lib/platform_efi/uefi.h:27` | `(16 \| 0x8000000000000000)` |  | other:1 |
| `EFI_NO_MAPPING` | object-like | `lib/platform_efi/uefi.h:28` | `(17 \| 0x8000000000000000)` |  | other:1 |
| `EFI_TIMEOUT` | object-like | `lib/platform_efi/uefi.h:29` | `(18 \| 0x8000000000000000)` |  | other:1 |
| `EFI_NOT_STARTED` | object-like | `lib/platform_efi/uefi.h:30` | `(19 \| 0x8000000000000000)` |  | other:1 |
| `EFI_ALREADY_STARTED` | object-like | `lib/platform_efi/uefi.h:31` | `(20 \| 0x8000000000000000)` |  | other:1 |
| `EFI_ABORTED` | object-like | `lib/platform_efi/uefi.h:32` | `(21 \| 0x8000000000000000)` |  | other:1 |
| `EFI_ICMP_ERROR` | object-like | `lib/platform_efi/uefi.h:33` | `(22 \| 0x8000000000000000)` |  | other:1 |
| `EFI_TFTP_ERROR` | object-like | `lib/platform_efi/uefi.h:34` | `(23 \| 0x8000000000000000)` |  | other:1 |
| `EFI_PROTOCOL_ERROR` | object-like | `lib/platform_efi/uefi.h:35` | `(24 \| 0x8000000000000000)` |  | other:1 |
| `EFI_INCOMPATIBLE_VERSION` | object-like | `lib/platform_efi/uefi.h:36` | `(25 \| 0x8000000000000000)` |  | other:1 |
| `EFI_SECURITY_VIOLATION` | object-like | `lib/platform_efi/uefi.h:37` | `(26 \| 0x8000000000000000)` |  | other:1 |
| `EFI_CRC_ERROR` | object-like | `lib/platform_efi/uefi.h:38` | `(27 \| 0x8000000000000000)` |  | other:1 |
| `EFI_END_OF_MEDIA` | object-like | `lib/platform_efi/uefi.h:39` | `(28 \| 0x8000000000000000)` |  | other:1 |
| `EFI_END_OF_FILE` | object-like | `lib/platform_efi/uefi.h:40` | `(31 \| 0x8000000000000000)` |  | other:1 |
| `EFI_INVALID_LANGUAGE` | object-like | `lib/platform_efi/uefi.h:41` | `(32 \| 0x8000000000000000)` |  | other:1 |
| `EFI_COMPROMISED_DATA` | object-like | `lib/platform_efi/uefi.h:42` | `(33 \| 0x8000000000000000)` |  | other:1 |
| `EFI_IP_ADDRESS_CONFLICT` | object-like | `lib/platform_efi/uefi.h:43` | `(34 \| 0x8000000000000000)` |  | other:1 |
| `EFI_HTTP_ERROR` | object-like | `lib/platform_efi/uefi.h:44` | `(35 \| 0x8000000000000000)` |  | other:1 |
| `EFI_NETWORK_UNREACHABLE` | object-like | `lib/platform_efi/uefi.h:45` | `(100 \| 0x8000000000000000)` |  | other:1 |
| `EFI_HOST_UNREACHABLE` | object-like | `lib/platform_efi/uefi.h:46` | `(101 \| 0x8000000000000000)` |  | other:1 |
| `EFI_PROTOCOL_UNREACHABLE` | object-like | `lib/platform_efi/uefi.h:47` | `(102 \| 0x8000000000000000)` |  | other:1 |
| `EFI_PORT_UNREACHABLE` | object-like | `lib/platform_efi/uefi.h:48` | `(103 \| 0x8000000000000000)` |  | other:1 |
| `EFI_CONNECTION_FIN` | object-like | `lib/platform_efi/uefi.h:49` | `(104 \| 0x8000000000000000)` |  | other:1 |
| `EFI_CONNECTION_RESET` | object-like | `lib/platform_efi/uefi.h:50` | `(105 \| 0x8000000000000000)` |  | other:1 |
| `EFI_CONNECTION_REFUSED` | object-like | `lib/platform_efi/uefi.h:51` | `(106 \| 0x8000000000000000)` |  | other:1 |
| `EFI_FILE_SYSTEM_INFO_ID` | object-like | `lib/platform_efi/uefi.h:53` | `{0x09576e93, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}` |  |  |
| `EFI_MP_SERVICES_PROTOCOL_GUID` | object-like | `lib/platform_efi/uefi.h:54` | `{0x3fdda605, 0xa76e, 0x4f46, {0xad, 0x29, 0x12, 0xf4, 0x53, 0x1b, 0x3d, 0x08}}` |  |  |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID` | object-like | `lib/platform_efi/uefi.h:55` | `{0x0964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}}` |  |  |
| `EFI_TCP4_PROTOCOL_GUID` | object-like | `lib/platform_efi/uefi.h:56` | `{0x65530BC7, 0xA359, 0x410f, {0xB0, 0x10, 0x5A, 0xAD, 0xC7, 0xEC, 0x2B, 0x62}}` |  |  |
| `EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID` | object-like | `lib/platform_efi/uefi.h:57` | `{0x00720665, 0x67EB, 0x4a99, {0xBA, 0xF7, 0xD3, 0xC3, 0x3A, 0x1C, 0x7C, 0xC9}}` |  |  |
| `EFI_FILE_INFO_ID` | object-like | `lib/platform_efi/uefi.h:58` | `{ 0x9576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x0, 0xa0, 0xc9, 0x69, 0x72, 0x3b }}` |  |  |
| `EFI_FILE_MODE_READ` | object-like | `lib/platform_efi/uefi.h:60` | `0x0000000000000001` |  |  |
| `EFI_FILE_MODE_WRITE` | object-like | `lib/platform_efi/uefi.h:61` | `0x0000000000000002` |  |  |
| `EFI_FILE_MODE_CREATE` | object-like | `lib/platform_efi/uefi.h:62` | `0x8000000000000000` |  |  |
| `EFI_FILE_READ_ONLY` | object-like | `lib/platform_efi/uefi.h:63` | `0x0000000000000001` |  |  |
| `EFI_FILE_HIDDEN` | object-like | `lib/platform_efi/uefi.h:64` | `0x0000000000000002` |  |  |
| `EFI_FILE_SYSTEM` | object-like | `lib/platform_efi/uefi.h:65` | `0x0000000000000004` |  |  |
| `EFI_FILE_RESERVED` | object-like | `lib/platform_efi/uefi.h:66` | `0x0000000000000008` |  |  |
| `EFI_FILE_DIRECTORY` | object-like | `lib/platform_efi/uefi.h:67` | `0x0000000000000010` |  |  |
| `EFI_FILE_ARCHIVE` | object-like | `lib/platform_efi/uefi.h:68` | `0x0000000000000020` |  |  |
| `EFI_FILE_VALID_ATTR` | object-like | `lib/platform_efi/uefi.h:69` | `0x0000000000000037` |  |  |
| `EFI_FILE_PROTOCOL_REVISION` | object-like | `lib/platform_efi/uefi.h:70` | `0x00010000` |  |  |
| `EFI_FILE_PROTOCOL_REVISION2` | object-like | `lib/platform_efi/uefi.h:71` | `0x00020000` |  |  |
| `EFI_FILE_PROTOCOL_LATEST_REVISION` | object-like | `lib/platform_efi/uefi.h:72` | `EFI_FILE_PROTOCOL_REVISION2` | EFI_FILE_PROTOCOL_REVISION2 |  |
| `EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER` | object-like | `lib/platform_efi/uefi.h:73` | `0x00000008` |  |  |
| `EFI_OPEN_PROTOCOL_BY_DRIVER` | object-like | `lib/platform_efi/uefi.h:74` | `0x00000010` |  |  |
| `EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL` | object-like | `lib/platform_efi/uefi.h:75` | `0x00000001` |  |  |
| `EFI_OPEN_PROTOCOL_EXCLUSIVE` | object-like | `lib/platform_efi/uefi.h:76` | `0x00000020` |  |  |
| `EFI_OPEN_PROTOCOL_GET_PROTOCOL` | object-like | `lib/platform_efi/uefi.h:77` | `0x00000002` |  |  |
| `EFI_OPEN_PROTOCOL_TEST_PROTOCOL` | object-like | `lib/platform_efi/uefi.h:78` | `0x00000004` |  |  |
| `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION` | object-like | `lib/platform_efi/uefi.h:79` | `0x00010000` |  |  |
| `EFI_UNSPECIFIED_TIMEZONE` | object-like | `lib/platform_efi/uefi.h:80` | `0x07FF` |  |  |
| `END_OF_CPU_LIST` | object-like | `lib/platform_efi/uefi.h:81` | `0xFFFFFFFF` |  |  |
| `EVT_NOTIFY_SIGNAL` | object-like | `lib/platform_efi/uefi.h:82` | `0x00000200` |  |  |
| `EVT_NOTIFY_WAIT` | object-like | `lib/platform_efi/uefi.h:83` | `0x00000100` |  |  |
| `EVT_RUNTIME` | object-like | `lib/platform_efi/uefi.h:84` | `0x40000000` |  |  |
| `EVT_SIGNAL_EXIT_BOOT_SERVICES` | object-like | `lib/platform_efi/uefi.h:85` | `0x00000201` |  |  |
| `EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE` | object-like | `lib/platform_efi/uefi.h:86` | `0x60000202` |  |  |
| `EVT_TIMER` | object-like | `lib/platform_efi/uefi.h:87` | `0x80000000` |  |  |
| `EXCEPT_X64_DIVIDE_ERROR` | object-like | `lib/platform_efi/uefi.h:88` | `0` |  |  |
| `MAX_MCAST_FILTER_CNT` | object-like | `lib/platform_efi/uefi.h:89` | `16` |  | array-extent/subscript:1 |
| `PROCESSOR_AS_BSP_BIT` | object-like | `lib/platform_efi/uefi.h:90` | `0x00000001` |  |  |
| `PROCESSOR_ENABLED_BIT` | object-like | `lib/platform_efi/uefi.h:91` | `0x00000002` |  |  |
| `PROCESSOR_HEALTH_STATUS_BIT` | object-like | `lib/platform_efi/uefi.h:92` | `0x00000004` |  |  |
| `TPL_APPLICATION` | object-like | `lib/platform_efi/uefi.h:93` | `4` |  |  |
| `TPL_CALLBACK` | object-like | `lib/platform_efi/uefi.h:94` | `8` |  |  |
| `TPL_HIGH_LEVEL` | object-like | `lib/platform_efi/uefi.h:95` | `31` |  |  |
| `TPL_NOTIFY` | object-like | `lib/platform_efi/uefi.h:96` | `16` |  |  |
| `QX_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:24` | `1` |  | initializer:9, parenthesized/args:3 |
| `CONTRACT_INDEX` | object-like, redefined 33 more time(s) | `src/contract_core/contract_def.h:25` | `QX_CONTRACT_INDEX` | QX_CONTRACT_INDEX | initializer:112, template-arg:31, parenthesized/args:10 |
| `CONTRACT_STATE_TYPE` | object-like, redefined 33 more time(s) | `src/contract_core/contract_def.h:26` | `QX` |  |  |
| `CONTRACT_STATE2_TYPE` | object-like, redefined 33 more time(s) | `src/contract_core/contract_def.h:27` | `QX2` |  |  |
| `QUOTTERY_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:34` | `2` |  |  |
| `RANDOM_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:44` | `3` |  | parenthesized/args:1 |
| `QUTIL_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:54` | `4` |  |  |
| `MLM_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:64` | `5` |  |  |
| `GQMPROP_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:74` | `6` |  |  |
| `SWATCH_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:84` | `7` |  |  |
| `CCF_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:94` | `8` |  | initializer:1 |
| `QEARN_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:104` | `9` |  | initializer:12, parenthesized/args:2 |
| `QVAULT_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:114` | `10` |  | parenthesized/args:3 |
| `MSVAULT_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:124` | `11` |  |  |
| `QBAY_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:134` | `12` |  | initializer:115 |
| `QSWAP_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:144` | `13` |  |  |
| `NOST_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:154` | `14` |  | initializer:1 |
| `QDRAW_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:164` | `15` |  |  |
| `RL_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:174` | `16` |  | parenthesized/args:1 |
| `QBOND_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:184` | `17` |  |  |
| `QIP_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:194` | `18` |  |  |
| `QRAFFLE_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:204` | `19` |  | initializer:73 |
| `QRWA_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:214` | `20` |  |  |
| `QRP_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:224` | `21` |  | initializer:1, parenthesized/args:1 |
| `QTF_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:234` | `22` |  |  |
| `QDUEL_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:244` | `23` |  |  |
| `PULSE_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:254` | `24` |  |  |
| `VOTTUNBRIDGE_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:264` | `25` |  |  |
| `QUSINO_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:274` | `26` |  |  |
| `ESCROW_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:284` | `27` |  |  |
| `WOLFPACK_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:294` | `28` |  |  |
| `QPAYHUB_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:304` | `29` |  |  |
| `QTREAT_CONTRACT_INDEX` | object-like | `src/contract_core/contract_def.h:314` | `30` |  |  |
| `MAX_CONTRACT_ITERATION_DURATION` | object-like | `src/contract_core/contract_def.h:358` | `0 // In milliseconds, must be above 0; for now set to 0 to disable timeout, because a roll…` |  |  |
| `REGISTER_CONTRACT_FUNCTIONS_AND_PROCEDURES(contractName)` | function-like(1 params), ## paste, # stringize, multi-line(34) | `src/contract_core/contract_def.h:503` | `{  constexpr unsigned int contractIndex = contractName##_CONTRACT_INDEX;  if (!contractNam…` | BEGIN_EPOCH, BEGIN_TICK, END_EPOCH, END_TICK, INITIALIZE, PO | other:34 |
| `CURVE_ORDER_0` | object-like | `src/four_q.h:14` | `0x2FB2540EC7768CE7` |  | parenthesized/args:5, initializer:3 |
| `CURVE_ORDER_1` | object-like | `src/four_q.h:15` | `0xDFBD004DFE0F7999` |  | parenthesized/args:5, initializer:3 |
| `CURVE_ORDER_2` | object-like | `src/four_q.h:16` | `0xF05397829CBC14E5` |  | parenthesized/args:5, initializer:3 |
| `CURVE_ORDER_3` | object-like | `src/four_q.h:17` | `0x0029CBC14E5E0A72` |  | parenthesized/args:6, initializer:2 |
| `MONTGOMERY_SMALL_R_PRIME_0` | object-like | `src/four_q.h:18` | `0xE12FE5F079BC3929` |  | initializer:3, parenthesized/args:1 |
| `MONTGOMERY_SMALL_R_PRIME_1` | object-like | `src/four_q.h:19` | `0xD75E78B8D1FCDCF3` |  | initializer:2, parenthesized/args:1 |
| `MONTGOMERY_SMALL_R_PRIME_2` | object-like | `src/four_q.h:20` | `0xBCE409ED76B5DB21` |  | initializer:1, parenthesized/args:1 |
| `MONTGOMERY_SMALL_R_PRIME_3` | object-like | `src/four_q.h:21` | `0xF32702FDAFC1C074` |  | parenthesized/args:1 |
| `B11` | object-like | `src/four_q.h:23` | `0xF6F900D81F5F5E6A` |  | initializer:2 |
| `B12` | object-like | `src/four_q.h:24` | `0x1363E862C22A2DA0` |  | initializer:2 |
| `B13` | object-like | `src/four_q.h:25` | `0xF8BD9FCE1337FCF1` |  | initializer:2 |
| `B14` | object-like | `src/four_q.h:26` | `0x084F739986B9E651` |  | initializer:2 |
| `B21` | object-like | `src/four_q.h:27` | `0xE2B6A4157B033D2C` |  | initializer:2 |
| `B22` | object-like | `src/four_q.h:28` | `0x0000000000000001` |  | initializer:2 |
| `B23` | object-like | `src/four_q.h:29` | `0xFFFFFFFFFFFFFFFF` |  | initializer:2 |
| `B24` | object-like | `src/four_q.h:30` | `0xDA243A43722E9830` |  | initializer:2 |
| `B31` | object-like | `src/four_q.h:31` | `0xE85452E2DCE0FCFE` |  | initializer:2 |
| `B32` | object-like | `src/four_q.h:32` | `0xFD3BDEE51C7725AF` |  | initializer:2 |
| `B33` | object-like | `src/four_q.h:33` | `0x2E4D21C98927C49F` |  | initializer:2 |
| `B34` | object-like | `src/four_q.h:34` | `0xF56190BB3FD13269` |  | initializer:2 |
| `B41` | object-like | `src/four_q.h:35` | `0xEC91CBF56EF737C1` |  | initializer:3 |
| `B42` | object-like | `src/four_q.h:36` | `0xCEDD20D23C1F00CE` |  | initializer:3 |
| `B43` | object-like | `src/four_q.h:37` | `0x068A49F02AA8A9B5` |  | initializer:3 |
| `B44` | object-like | `src/four_q.h:38` | `0x18D5087896DE0AEA` |  | initializer:3 |
| `C1` | object-like | `src/four_q.h:39` | `0x72482C5251A4559C` |  | initializer:2 |
| `C2` | object-like | `src/four_q.h:40` | `0x59F95B0ADD276F6C` |  | initializer:2 |
| `C3` | object-like | `src/four_q.h:41` | `0x7DD2D17C4625FA78` |  | initializer:2 |
| `C4` | object-like | `src/four_q.h:42` | `0x6BC57DEF56CE8877` |  | initializer:2 |
| `GENERIC_K12` | object-like | `src/kangaroo_twelve.h:10` | `1` |  |  |
| `KeccakF1600RoundConstant0` | object-like | `src/kangaroo_twelve.h:15` | `0x000000008000808bULL` |  |  |
| `KeccakF1600RoundConstant1` | object-like | `src/kangaroo_twelve.h:16` | `0x800000000000008bULL` |  |  |
| `KeccakF1600RoundConstant2` | object-like | `src/kangaroo_twelve.h:17` | `0x8000000000008089ULL` |  |  |
| `KeccakF1600RoundConstant3` | object-like | `src/kangaroo_twelve.h:18` | `0x8000000000008003ULL` |  |  |
| `KeccakF1600RoundConstant4` | object-like | `src/kangaroo_twelve.h:19` | `0x8000000000008002ULL` |  |  |
| `KeccakF1600RoundConstant5` | object-like | `src/kangaroo_twelve.h:20` | `0x8000000000000080ULL` |  |  |
| `KeccakF1600RoundConstant6` | object-like | `src/kangaroo_twelve.h:21` | `0x000000000000800aULL` |  |  |
| `KeccakF1600RoundConstant7` | object-like | `src/kangaroo_twelve.h:22` | `0x800000008000000aULL` |  |  |
| `KeccakF1600RoundConstant8` | object-like | `src/kangaroo_twelve.h:23` | `0x8000000080008081ULL` |  |  |
| `KeccakF1600RoundConstant9` | object-like | `src/kangaroo_twelve.h:24` | `0x8000000000008080ULL` |  |  |
| `KeccakF1600RoundConstant10` | object-like | `src/kangaroo_twelve.h:25` | `0x0000000080000001ULL` |  |  |
| `declareABCDEScalar` | object-like, multi-line(18) | `src/kangaroo_twelve.h:30` | `unsigned long long Aba, Abe, Abi, Abo, Abu;      unsigned long long Aga, Age, Agi, Ago, Ag…` |  |  |
| `declareBCDEScalar` | object-like, multi-line(13) | `src/kangaroo_twelve.h:49` | `unsigned long long Bba, Bbe, Bbi, Bbo, Bbu;      unsigned long long Bga, Bge, Bgi, Bgo, Bg…` |  |  |
| `copyFromStateScalar(state)` | function-like(1 params), multi-line(26) | `src/kangaroo_twelve.h:63` | `Aba = state[0];               Abe = state[1];               Abi = state[2];               …` |  |  |
| `copyToStateScalar(state)` | function-like(1 params), multi-line(26) | `src/kangaroo_twelve.h:90` | `state[0] = Aba;             state[1] = Abe;             state[2] = Abi;             state[…` |  |  |
| `thetaRhoPiChiIotaPrepareThetaScalar(i, A, E)` | function-like(3 params), ## paste, multi-line(107) | `src/kangaroo_twelve.h:117` | `Da = Cu^ROL64(Ce, 1);      De = Ca^ROL64(Ci, 1);      Di = Ce^ROL64(Co, 1);      Do = Ci^R…` | ROL64 |  |
| `rounds12Scalar` | object-like, multi-line(98) | `src/kangaroo_twelve.h:225` | `Ca = Aba ^ Aga ^ Aka ^ Ama ^ Asa;                                                    Ce = …` | ROL64, thetaRhoPiChiIotaPrepareThetaScalar |  |
| `ROL64(a, offset)` | function-like(2 params), redefined 1 more time(s) | `src/kangaroo_twelve.h:325` | `_rotl64(a, offset)` |  | initializer:328 |
| `declareABCDE` | object-like, multi-line(19) | `src/kangaroo_twelve.h:371` | `unsigned long long Aba, Abe, Abi, Abo, Abu;      unsigned long long Aga, Age, Agi, Ago, Ag…` |  | other:2 |
| `thetaRhoPiChiIotaPrepareTheta(i, A, E)` | function-like(3 params), ## paste, multi-line(107) | `src/kangaroo_twelve.h:390` | `Da = Cu^ROL64(Ce, 1);      De = Ca^ROL64(Ci, 1);      Di = Ce^ROL64(Co, 1);      Do = Ci^R…` | ROL64 |  |
| `copyFromState(state)` | function-like(1 params), multi-line(26) | `src/kangaroo_twelve.h:498` | `Aba = state[ 0];      Abe = state[ 1];      Abi = state[ 2];      Abo = state[ 3];      Ab…` |  | other:2 |
| `copyToState(state)` | function-like(1 params), multi-line(26) | `src/kangaroo_twelve.h:525` | `state[ 0] = Aba;      state[ 1] = Abe;      state[ 2] = Abi;      state[ 3] = Abo;      st…` |  | other:2 |
| `rounds12` | object-like, multi-line(98) | `src/kangaroo_twelve.h:552` | `Ca = Aba^Aga^Aka^Ama^Asa;      Ce = Abe^Age^Ake^Ame^Ase;      Ci = Abi^Agi^Aki^Ami^Asi;   …` | ROL64, thetaRhoPiChiIotaPrepareTheta | other:2 |
| `K12_security` | object-like | `src/kangaroo_twelve.h:652` | `128` |  |  |
| `K12_capacity` | object-like | `src/kangaroo_twelve.h:653` | `(2 * K12_security)` | K12_security |  |
| `K12_capacityInBytes` | object-like | `src/kangaroo_twelve.h:654` | `(K12_capacity / 8)` | K12_capacity | parenthesized/args:3, initializer:2 |
| `K12_rateInBytes` | object-like | `src/kangaroo_twelve.h:655` | `((1600 - K12_capacity) / 8)` | K12_capacity | parenthesized/args:8, array-extent/subscript:4, initializer:3 |
| `K12_chunkSize` | object-like | `src/kangaroo_twelve.h:656` | `8192` |  | initializer:4, parenthesized/args:4, template-arg:1 |
| `K12_suffixLeaf` | object-like | `src/kangaroo_twelve.h:657` | `0x0B` |  | initializer:3 |
| `EMPTY` | object-like | `src/network_messages/assets.h:5` | `0` |  |  |
| `ISSUANCE` | object-like | `src/network_messages/assets.h:6` | `1` |  |  |
| `OWNERSHIP` | object-like | `src/network_messages/assets.h:7` | `2` |  |  |
| `POSSESSION` | object-like | `src/network_messages/assets.h:8` | `3` |  |  |
| `AMPERE` | object-like | `src/network_messages/assets.h:10` | `0` |  |  |
| `CANDELA` | object-like | `src/network_messages/assets.h:11` | `1` |  |  |
| `KELVIN` | object-like | `src/network_messages/assets.h:12` | `2` |  |  |
| `KILOGRAM` | object-like | `src/network_messages/assets.h:13` | `3` |  |  |
| `METER` | object-like | `src/network_messages/assets.h:14` | `4` |  |  |
| `MOLE` | object-like | `src/network_messages/assets.h:15` | `5` |  |  |
| `SECOND` | object-like | `src/network_messages/assets.h:16` | `6` |  |  |
| `SIGNATURE_SIZE` | object-like | `src/network_messages/common_def.h:3` | `64` |  | array-extent/subscript:1, parenthesized/args:1 |
| `NUMBER_OF_TRANSACTIONS_PER_TICK` | object-like | `src/network_messages/common_def.h:4` | `4096ULL // Must be 2^N` |  |  |
| `MAX_NUMBER_OF_CONTRACTS` | object-like | `src/network_messages/common_def.h:5` | `1024 // Must be 1024` |  | array-extent/subscript:1, parenthesized/args:1 |
| `NUMBER_OF_COMPUTORS` | object-like, redefined 1 more time(s) | `src/network_messages/common_def.h:6` | `676` |  | initializer:30, template-arg:8, parenthesized/args:6, array-extent/subscript:5 |
| `QUORUM` | object-like, redefined 1 more time(s) | `src/network_messages/common_def.h:7` | `(NUMBER_OF_COMPUTORS * 2 / 3 + 1)` | NUMBER_OF_COMPUTORS | parenthesized/args:8, template-arg:3 |
| `NUMBER_OF_EXCHANGED_PEERS` | object-like | `src/network_messages/common_def.h:8` | `4` |  |  |
| `SPECTRUM_DEPTH` | object-like | `src/network_messages/common_def.h:10` | `24 // Defines SPECTRUM_CAPACITY (1 << SPECTRUM_DEPTH)` | SPECTRUM_CAPACITY | array-extent/subscript:1, parenthesized/args:1 |
| `SPECTRUM_CAPACITY` | object-like | `src/network_messages/common_def.h:11` | `(1ULL << SPECTRUM_DEPTH) // Must be 2^N` | SPECTRUM_DEPTH | initializer:1 |
| `ASSETS_CAPACITY` | object-like | `src/network_messages/common_def.h:13` | `0x1000000ULL // Must be 2^N` |  | initializer:1 |
| `ASSETS_DEPTH` | object-like | `src/network_messages/common_def.h:14` | `24 // Is derived from ASSETS_CAPACITY (=N)` | ASSETS_CAPACITY | array-extent/subscript:4 |
| `MAX_INPUT_SIZE` | object-like | `src/network_messages/common_def.h:16` | `1024ULL` |  | initializer:2 |
| `ISSUANCE_RATE` | object-like | `src/network_messages/common_def.h:17` | `1000000000000LL` |  |  |
| `MAX_AMOUNT` | object-like | `src/network_messages/common_def.h:18` | `(ISSUANCE_RATE * 1000LL)` | ISSUANCE_RATE | parenthesized/args:70, template-arg:3, initializer:1 |
| `MAX_SUPPLY` | object-like | `src/network_messages/common_def.h:19` | `(ISSUANCE_RATE * 200ULL)` | ISSUANCE_RATE |  |
| `LOG_TX_NUMBER_OF_SPECIAL_EVENT` | object-like | `src/network_messages/logging.h:5` | `6` |  |  |
| `LOG_TX_PER_TICK` | object-like | `src/network_messages/logging.h:6` | `(NUMBER_OF_TRANSACTIONS_PER_TICK + LOG_TX_NUMBER_OF_SPECIAL_EVENT) // normal tx + special …` | LOG_TX_NUMBER_OF_SPECIAL_EVENT, NUMBER_OF_TRANSACTIONS_PER_T | array-extent/subscript:2, initializer:1 |
| `DEFINE_OC()` | function-like(0 params) | `src/oc_core/oc_interfaces_def.h:6` | `` |  |  |
| `OC_INTERFACE_INDEX` | object-like | `src/oc_core/oc_interfaces_def.h:8` | `0` |  | initializer:1 |
| `DEFINE_OC_INTERFACE(Interface)` | function-like(1 params) | `src/oc_core/oc_interfaces_def.h:14` | `{sizeof(Interface::OcRequest)}` |  | initializer:1 |
| `REGISTER_OC_INTERFACE(Interface)` | function-like(1 params), multi-line(5) | `src/oc_core/oc_interfaces_def.h:31` | `{  		getOcInvocationFeeFunc[Interface::ocInterfaceIndex] = (__GetInvocationFeeFunc)Interfa…` |  | other:1 |
| `DEFINE_ORACLE()` | function-like(0 params) | `src/oracle_core/oracle_interfaces_def.h:6` | `` |  |  |
| `ORACLE_INTERFACE_INDEX` | object-like, redefined 4 more time(s) | `src/oracle_core/oracle_interfaces_def.h:8` | `0` |  | initializer:5 |
| `DEFINE_ORACLE_INTERFACE(Interface)` | function-like(1 params) | `src/oracle_core/oracle_interfaces_def.h:30` | `{sizeof(Interface::OracleQuery), sizeof(Interface::OracleReply)}` |  | initializer:5 |
| `REGISTER_ORACLE_INTERFACE(Interface)` | function-like(1 params), multi-line(7) | `src/oracle_core/oracle_interfaces_def.h:52` | `{  		getOracleQueryFeeFunc[Interface::oracleInterfaceIndex] = (__GetQueryFeeFunc)Interface…` |  | other:5 |
| `ENABLE_ORACLE_STATS_RECORD` | object-like | `src/oracle_core/oracle_interfaces_def.h:84` | `1` |  |  |
| `ASSERT` | object-like, redefined 2 more time(s) | `src/platform/assert.h:6` | `EXPECT_TRUE` |  | other:116 |
| `ACQUIRE_WITHOUT_DEBUG_LOGGING(lock)` | function-like(1 params) | `src/platform/concurrency.h:6` | `while (_InterlockedCompareExchange8(&lock, 1, 0)) _mm_pause()` |  |  |
| `ACQUIRE(lock)` | function-like(1 params), redefined 1 more time(s) | `src/platform/concurrency.h:11` | `ACQUIRE_WITHOUT_DEBUG_LOGGING(lock)` | ACQUIRE_WITHOUT_DEBUG_LOGGING | other:1 |
| `TRY_ACQUIRE(lock)` | function-like(1 params) | `src/platform/concurrency.h:41` | `(_InterlockedCompareExchange8(&lock, 1, 0) == 0)` |  | parenthesized/args:2 |
| `RELEASE(lock)` | function-like(1 params) | `src/platform/concurrency.h:44` | `lock = 0` |  | other:3 |
| `BEGIN_WAIT_WHILE(condition)` | function-like(1 params), multi-line(2), redefined 1 more time(s) | `src/platform/concurrency.h:67` | `while (condition) {` |  | other:1 |
| `END_WAIT_WHILE()` | function-like(0 params), redefined 1 more time(s) | `src/platform/concurrency.h:71` | `_mm_pause(); }` |  | other:1 |
| `WAIT_WHILE(condition)` | function-like(1 params), multi-line(3) | `src/platform/concurrency.h:88` | `BEGIN_WAIT_WHILE(condition)      END_WAIT_WHILE()` | BEGIN_WAIT_WHILE, END_WAIT_WHILE |  |
| `ATOMIC_STORE8(target, val)` | function-like(2 params) | `src/platform/concurrency.h:92` | `_InterlockedExchange8(&target, val)` |  |  |
| `ATOMIC_STORE32(target, val)` | function-like(2 params) | `src/platform/concurrency.h:95` | `_InterlockedExchange((volatile long*)&target, val)` |  |  |
| `ATOMIC_LOAD32(target)` | function-like(1 params) | `src/platform/concurrency.h:96` | `_InterlockedCompareExchange((volatile long*)&target, 0, 0)` |  |  |
| `ATOMIC_INC64(target)` | function-like(1 params) | `src/platform/concurrency.h:97` | `_InterlockedIncrement64(&target)` |  |  |
| `ATOMIC_AND64(target, val)` | function-like(2 params) | `src/platform/concurrency.h:98` | `_InterlockedAnd64(&target, val)` |  |  |
| `ATOMIC_STORE64(target, val)` | function-like(2 params) | `src/platform/concurrency.h:99` | `_InterlockedExchange64(&target, val)` |  |  |
| `ATOMIC_LOAD64(target)` | function-like(1 params) | `src/platform/concurrency.h:100` | `_InterlockedCompareExchange64(&target, 0, 0)` |  |  |
| `ATOMIC_ADD64(target, val)` | function-like(2 params) | `src/platform/concurrency.h:101` | `_InterlockedExchangeAdd64(&target, val)` |  |  |
| `ATOMIC_MAX64(target, val)` | function-like(2 params) | `src/platform/concurrency.h:102` | `atomicMax64(&target, val)` |  |  |
| `GLOBAL_VAR_DECL` | object-like, redefined 2 more time(s) | `src/platform/global_var.h:10` | `static` |  | other:26 |
| `GLOBAL_VAR_INIT(val)` | function-like(1 params), redefined 2 more time(s) | `src/platform/global_var.h:11` | `=val` |  | other:3 |
| `OPERATOR` | object-like | `src/private_settings.h:7` | `"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"` |  | parenthesized/args:1 |
| `NUMBER_OF_PRIVATE_IP` | object-like | `src/private_settings.h:15` | `2` |  |  |
| `ENABLE_QUBIC_LOGGING_EVENT` | object-like | `src/private_settings.h:44` | `0 // turn on logging events` |  |  |
| `USE_PARALLEL_SIGN_VOTES` | object-like | `src/private_settings.h:46` | `1` |  |  |
| `LOG_BUFFER_PAGE_SIZE` | object-like | `src/private_settings.h:49` | `300000000ULL` |  | initializer:1 |
| `PMAP_LOG_PAGE_SIZE` | object-like | `src/private_settings.h:50` | `30000000ULL` |  | initializer:1 |
| `IMAP_LOG_PAGE_SIZE` | object-like | `src/private_settings.h:51` | `10000ULL` |  | initializer:1 |
| `VM_NUM_CACHE_PAGE` | object-like | `src/private_settings.h:52` | `8` |  |  |
| `LOG_UNIVERSE` | object-like, redefined 1 more time(s) | `src/private_settings.h:56` | `1` |  |  |
| `LOG_SPECTRUM` | object-like, redefined 1 more time(s) | `src/private_settings.h:57` | `1` |  |  |
| `LOG_CONTRACT_ERROR_MESSAGES` | object-like, redefined 1 more time(s) | `src/private_settings.h:58` | `1` |  |  |
| `LOG_CONTRACT_WARNING_MESSAGES` | object-like, redefined 1 more time(s) | `src/private_settings.h:59` | `1` |  |  |
| `LOG_CONTRACT_INFO_MESSAGES` | object-like, redefined 1 more time(s) | `src/private_settings.h:60` | `1` |  |  |
| `LOG_CONTRACT_DEBUG_MESSAGES` | object-like, redefined 1 more time(s) | `src/private_settings.h:61` | `1` |  |  |
| `LOG_CUSTOM_MESSAGES` | object-like, redefined 1 more time(s) | `src/private_settings.h:62` | `1` |  |  |
| `LOG_ORACLES` | object-like, redefined 1 more time(s) | `src/private_settings.h:63` | `1` |  |  |
| `LOG_OC` | object-like, redefined 1 more time(s) | `src/private_settings.h:64` | `1` |  |  |
| `TICK_STORAGE_AUTOSAVE_MODE` | object-like | `src/private_settings.h:85` | `0` |  |  |
| `TICK_STORAGE_AUTOSAVE_TICK_PERIOD` | object-like | `src/private_settings.h:91` | `1000` |  |  |
| `MAX_NUMBER_OF_PROCESSORS` | object-like | `src/public_settings.h:13` | `32` |  |  |
| `NUMBER_OF_SOLUTION_PROCESSORS` | object-like | `src/public_settings.h:14` | `12` |  |  |
| `NUMBER_OF_PREPROCESS_SOLUTION_PROCESSORS` | object-like | `src/public_settings.h:17` | `4` |  |  |
| `NUMBER_OF_CONTRACT_EXECUTION_BUFFERS` | object-like | `src/public_settings.h:22` | `10` |  |  |
| `USE_SCORE_CACHE` | object-like | `src/public_settings.h:24` | `1` |  |  |
| `SCORE_CACHE_SIZE` | object-like | `src/public_settings.h:25` | `2000000 // the larger the better` |  |  |
| `SCORE_CACHE_COLLISION_RETRIES` | object-like | `src/public_settings.h:26` | `20 // number of retries to find entry in cache in case of hash collision` |  |  |
| `ANT_USE_SCORE_CACHE` | object-like | `src/public_settings.h:29` | `1` |  |  |
| `TICKS_TO_KEEP_FROM_PRIOR_EPOCH` | object-like | `src/public_settings.h:32` | `100` |  |  |
| `TARGET_TICK_DURATION` | object-like | `src/public_settings.h:35` | `1000` |  | parenthesized/args:1 |
| `TICK_DURATION_FOR_ALLOCATION_MS` | object-like | `src/public_settings.h:39` | `225` |  |  |
| `TRANSACTION_SPARSENESS` | object-like | `src/public_settings.h:40` | `10` |  |  |
| `PENDING_TXS_POOL_NUM_TICKS` | object-like | `src/public_settings.h:43` | `(1000 * 60 * 10ULL / TICK_DURATION_FOR_ALLOCATION_MS) // 10 minutes` | TICK_DURATION_FOR_ALLOCATION_MS |  |
| `AUTO_FORCE_NEXT_TICK_THRESHOLD` | object-like | `src/public_settings.h:46` | `0ULL // Multiplier of TARGET_TICK_DURATION for the system to detect "F5 case" \| set to 0 …` | TARGET_TICK_DURATION | template-arg:1, parenthesized/args:1 |
| `PROBABILITY_TO_FORCE_EMPTY_TICK` | object-like | `src/public_settings.h:51` | `800 // after (AUTO_FORCE_NEXT_TICK_THRESHOLD x TARGET_TICK_DURATION) seconds, the node wil…` | AUTO_FORCE_NEXT_TICK_THRESHOLD, TARGET_TICK_DURATION |  |
| `NEXT_TICK_TIMEOUT_THRESHOLD` | object-like | `src/public_settings.h:54` | `5ULL // Multiplier of TARGET_TICK_DURATION for the system to discard next tick in tickData…` | TARGET_TICK_DURATION | parenthesized/args:1 |
| `PEER_REFRESHING_PERIOD` | object-like | `src/public_settings.h:57` | `120000ULL` |  | parenthesized/args:1 |
| `START_NETWORK_FROM_SCRATCH` | object-like | `src/public_settings.h:66` | `1` |  |  |
| `ADDON_TX_STATUS_REQUEST` | object-like | `src/public_settings.h:69` | `0` |  |  |
| `VERSION_A` | object-like | `src/public_settings.h:75` | `1` |  |  |
| `VERSION_B` | object-like | `src/public_settings.h:76` | `306` |  |  |
| `VERSION_C` | object-like | `src/public_settings.h:77` | `0` |  |  |
| `EPOCH` | object-like | `src/public_settings.h:80` | `233` |  |  |
| `TICK` | object-like | `src/public_settings.h:81` | `82400000` |  |  |
| `TICK_IS_FIRST_TICK_OF_EPOCH` | object-like | `src/public_settings.h:82` | `1 // Set to 0 if the network is restarted during the EPOCH with a new initial TICK` | EPOCH, TICK |  |
| `ARBITRATOR` | object-like | `src/public_settings.h:84` | `"AFZPUAIYVPNUYGJRQVLUKOPPVLHAZQTGLYAAUUNBXFTVTAMSBKQBLEIEPCVJ"` |  | parenthesized/args:1 |
| `DISPATCHER` | object-like | `src/public_settings.h:85` | `"XPXYKFLGSWRHRGAUKWFWVXCDVEYAPCPCNUTMUDWFGDYQCWZNJMWFZEEGCFFO"` |  | parenthesized/args:1 |
| `SOLUTION_SECURITY_DEPOSIT` | object-like | `src/public_settings.h:162` | `1000000` |  |  |
| `TARGET_TICK_VOTE_SIGNATURE` | object-like | `src/public_settings.h:165` | `0x000242ECU // ~28,980 signing operations per ID` |  |  |
| `MAX_NUMBER_OF_TICKS_PER_EPOCH` | object-like | `src/public_settings.h:170` | `(((((60ULL * 60 * 24 * 7 * 1000) / TICK_DURATION_FOR_ALLOCATION_MS) + NUMBER_OF_COMPUTORS …` | NUMBER_OF_COMPUTORS, TICK_DURATION_FOR_ALLOCATION_MS | initializer:1 |
| `FIRST_TICK_TRANSACTION_OFFSET` | object-like | `src/public_settings.h:171` | `sizeof(unsigned long long)` |  |  |
| `MAX_TRANSACTION_SIZE` | object-like | `src/public_settings.h:172` | `(MAX_INPUT_SIZE + sizeof(Transaction) + SIGNATURE_SIZE)` | MAX_INPUT_SIZE, SIGNATURE_SIZE |  |
| `DOGE_BROADCAST_CYCLE` | object-like | `src/public_settings.h:176` | `(2 * NUMBER_OF_COMPUTORS + 1)` | NUMBER_OF_COMPUTORS |  |
| `STACK_SIZE` | object-like | `src/public_settings.h:178` | `4194304` |  |  |
| `TRACK_MAX_STACK_BUFFER_SIZE` | object-like | `src/public_settings.h:179` | `` |  |  |
| `STATIC_ASSERT(condition, identifier)` | function-like(2 params), # stringize | `src/qpi/qpi_macros.h:60` | `static_assert(condition, #identifier);` |  | other:1 |
| `NO_IO_SYSTEM_PROC(CapLetterName, FuncName, InputType, OutputType)` | function-like(4 params), ## paste, multi-line(4) | `src/qpi/qpi_macros.h:63` | `public:  			typedef NoData CapLetterName##_locals;  			NO_IO_SYSTEM_PROC_WITH_LOCALS(CapLe…` | NO_IO_SYSTEM_PROC_WITH_LOCALS |  |
| `NO_IO_SYSTEM_PROC_WITH_LOCALS(CapLetterName, FuncName, InputType, OutputType)` | function-like(4 params), ## paste, # stringize, multi-line(6), __LINE__ | `src/qpi/qpi_macros.h:69` | `public:  			enum { FuncName##Empty = 0, FuncName##LocalsSize = sizeof(CapLetterName##_loca…` | CONTRACT_INDEX, CONTRACT_STATE_TYPE |  |
| `INITIALIZE()` | function-like(0 params) | `src/qpi/qpi_macros.h:77` | `NO_IO_SYSTEM_PROC(INITIALIZE, __initialize, NoData, NoData)` | NO_IO_SYSTEM_PROC | other:25 |
| `INITIALIZE_WITH_LOCALS()` | function-like(0 params) | `src/qpi/qpi_macros.h:80` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(INITIALIZE, __initialize, NoData, NoData)` | INITIALIZE, NO_IO_SYSTEM_PROC_WITH_LOCALS | other:2 |
| `BEGIN_EPOCH()` | function-like(0 params) | `src/qpi/qpi_macros.h:83` | `NO_IO_SYSTEM_PROC(BEGIN_EPOCH, __beginEpoch, NoData, NoData)` | NO_IO_SYSTEM_PROC | other:9, initializer:1 |
| `BEGIN_EPOCH_WITH_LOCALS()` | function-like(0 params) | `src/qpi/qpi_macros.h:86` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(BEGIN_EPOCH, __beginEpoch, NoData, NoData)` | BEGIN_EPOCH, NO_IO_SYSTEM_PROC_WITH_LOCALS | other:12 |
| `END_EPOCH()` | function-like(0 params) | `src/qpi/qpi_macros.h:89` | `NO_IO_SYSTEM_PROC(END_EPOCH, __endEpoch, NoData, NoData)` | NO_IO_SYSTEM_PROC | other:5, initializer:1 |
| `END_EPOCH_WITH_LOCALS()` | function-like(0 params) | `src/qpi/qpi_macros.h:92` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(END_EPOCH, __endEpoch, NoData, NoData)` | END_EPOCH, NO_IO_SYSTEM_PROC_WITH_LOCALS | other:19 |
| `BEGIN_TICK()` | function-like(0 params) | `src/qpi/qpi_macros.h:95` | `NO_IO_SYSTEM_PROC(BEGIN_TICK, __beginTick, NoData, NoData)` | NO_IO_SYSTEM_PROC | other:2, initializer:1 |
| `BEGIN_TICK_WITH_LOCALS()` | function-like(0 params) | `src/qpi/qpi_macros.h:98` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(BEGIN_TICK, __beginTick, NoData, NoData)` | BEGIN_TICK, NO_IO_SYSTEM_PROC_WITH_LOCALS | other:5 |
| `END_TICK()` | function-like(0 params) | `src/qpi/qpi_macros.h:101` | `NO_IO_SYSTEM_PROC(END_TICK, __endTick, NoData, NoData)` | NO_IO_SYSTEM_PROC | other:3, initializer:1 |
| `END_TICK_WITH_LOCALS()` | function-like(0 params) | `src/qpi/qpi_macros.h:104` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(END_TICK, __endTick, NoData, NoData)` | END_TICK, NO_IO_SYSTEM_PROC_WITH_LOCALS | other:8 |
| `PRE_ACQUIRE_SHARES()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:108` | `NO_IO_SYSTEM_PROC(PRE_ACQUIRE_SHARES, __preAcquireShares, PreManagementRightsTransfer_inpu…` | NO_IO_SYSTEM_PROC | other:18, initializer:1 |
| `PRE_ACQUIRE_SHARES_WITH_LOCALS()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:114` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(PRE_ACQUIRE_SHARES, __preAcquireShares, PreManagementRightsT…` | NO_IO_SYSTEM_PROC_WITH_LOCALS, PRE_ACQUIRE_SHARES | other:2 |
| `PRE_RELEASE_SHARES()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:120` | `NO_IO_SYSTEM_PROC(PRE_RELEASE_SHARES, __preReleaseShares, PreManagementRightsTransfer_inpu…` | NO_IO_SYSTEM_PROC | other:3, initializer:1 |
| `PRE_RELEASE_SHARES_WITH_LOCALS()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:126` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(PRE_RELEASE_SHARES, __preReleaseShares, PreManagementRightsT…` | NO_IO_SYSTEM_PROC_WITH_LOCALS, PRE_RELEASE_SHARES |  |
| `POST_ACQUIRE_SHARES()` | function-like(0 params), multi-line(2) | `src/qpi/qpi_macros.h:132` | `NO_IO_SYSTEM_PROC(POST_ACQUIRE_SHARES, __postAcquireShares, PostManagementRightsTransfer_i…` | NO_IO_SYSTEM_PROC | other:5, initializer:1 |
| `POST_ACQUIRE_SHARES_WITH_LOCALS()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:137` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(POST_ACQUIRE_SHARES, __postAcquireShares, PostManagementRigh…` | NO_IO_SYSTEM_PROC_WITH_LOCALS, POST_ACQUIRE_SHARES | other:2 |
| `POST_RELEASE_SHARES()` | function-like(0 params), multi-line(2) | `src/qpi/qpi_macros.h:143` | `NO_IO_SYSTEM_PROC(POST_RELEASE_SHARES, __postReleaseShares, PostManagementRightsTransfer_i…` | NO_IO_SYSTEM_PROC | other:3, initializer:1 |
| `POST_RELEASE_SHARES_WITH_LOCALS()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:148` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(POST_RELEASE_SHARES, __postReleaseShares, PostManagementRigh…` | NO_IO_SYSTEM_PROC_WITH_LOCALS, POST_RELEASE_SHARES |  |
| `POST_INCOMING_TRANSFER()` | function-like(0 params), multi-line(2) | `src/qpi/qpi_macros.h:154` | `NO_IO_SYSTEM_PROC(POST_INCOMING_TRANSFER, __postIncomingTransfer, PostIncomingTransfer_inp…` | NO_IO_SYSTEM_PROC | other:4, initializer:1 |
| `POST_INCOMING_TRANSFER_WITH_LOCALS()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:159` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(POST_INCOMING_TRANSFER, __postIncomingTransfer, PostIncoming…` | NO_IO_SYSTEM_PROC_WITH_LOCALS, POST_INCOMING_TRANSFER | other:7 |
| `SET_SHAREHOLDER_PROPOSAL()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:165` | `NO_IO_SYSTEM_PROC(SET_SHAREHOLDER_PROPOSAL, __setShareholderProposal, SET_SHAREHOLDER_PROP…` | NO_IO_SYSTEM_PROC | initializer:1 |
| `SET_SHAREHOLDER_PROPOSAL_WITH_LOCALS()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:172` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(SET_SHAREHOLDER_PROPOSAL, __setShareholderProposal, SET_SHAR…` | NO_IO_SYSTEM_PROC_WITH_LOCALS, SET_SHAREHOLDER_PROPOSAL | other:1 |
| `SET_SHAREHOLDER_VOTES()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:178` | `NO_IO_SYSTEM_PROC(SET_SHAREHOLDER_VOTES, __setShareholderVotes, SET_SHAREHOLDER_VOTES_inpu…` | NO_IO_SYSTEM_PROC | initializer:1, other:1 |
| `SET_SHAREHOLDER_VOTES_WITH_LOCALS()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:185` | `NO_IO_SYSTEM_PROC_WITH_LOCALS(SET_SHAREHOLDER_VOTES, __setShareholderVotes, SET_SHAREHOLDE…` | NO_IO_SYSTEM_PROC_WITH_LOCALS, SET_SHAREHOLDER_VOTES |  |
| `EXPAND()` | function-like(0 params), multi-line(5), __LINE__ | `src/qpi/qpi_macros.h:189` | `public:          enum { __expandEmpty = 0 };  		inline static void __expand(const QPI::Qpi…` | CONTRACT_INDEX, CONTRACT_STATE2_TYPE, CONTRACT_STATE_TYPE |  |
| `MIGRATE_WITH_LOCALS()` | function-like(0 params), multi-line(6), __LINE__ | `src/qpi/qpi_macros.h:195` | `public:          enum { __migrateEmpty = 0, __migrateOldStateSize = sizeof(CONTRACT_STATE_…` | CONTRACT_INDEX, CONTRACT_STATE_TYPE |  |
| `MIGRATE()` | function-like(0 params), multi-line(4) | `src/qpi/qpi_macros.h:202` | `public:  		typedef NoData MIGRATE_locals;  		MIGRATE_WITH_LOCALS()` | MIGRATE_WITH_LOCALS | other:3, initializer:1 |
| `LOG_DEBUG(message)` | function-like(1 params) | `src/qpi/qpi_macros.h:207` | `__logContractDebugMessage(CONTRACT_INDEX, message);` | CONTRACT_INDEX |  |
| `LOG_ERROR(message)` | function-like(1 params) | `src/qpi/qpi_macros.h:209` | `__logContractErrorMessage(CONTRACT_INDEX, message);` | CONTRACT_INDEX | other:1 |
| `LOG_INFO(message)` | function-like(1 params) | `src/qpi/qpi_macros.h:211` | `__logContractInfoMessage(CONTRACT_INDEX, message);` | CONTRACT_INDEX | other:494 |
| `LOG_WARNING(message)` | function-like(1 params) | `src/qpi/qpi_macros.h:213` | `__logContractWarningMessage(CONTRACT_INDEX, message);` | CONTRACT_INDEX | other:2 |
| `LOG_PAUSE()` | function-like(0 params) | `src/qpi/qpi_macros.h:215` | `__pauseLogMessage();` |  | other:1 |
| `LOG_RESUME()` | function-like(0 params) | `src/qpi/qpi_macros.h:217` | `__resumeLogMessage();` |  | other:1 |
| `PRIVATE_FUNCTION(function)` | function-like(1 params), ## paste, multi-line(4) | `src/qpi/qpi_macros.h:219` | `protected:  			typedef QPI::NoData function##_locals;  			PRIVATE_FUNCTION_WITH_LOCALS(fun…` | PRIVATE_FUNCTION_WITH_LOCALS | other:1 |
| `PRIVATE_FUNCTION_WITH_LOCALS(function)` | function-like(1 params), ## paste, multi-line(5), __LINE__ | `src/qpi/qpi_macros.h:224` | `protected:  			enum { __is_function_##function = true };  			inline static void function(c…` | CONTRACT_INDEX, CONTRACT_STATE_TYPE | other:43 |
| `PRIVATE_PROCEDURE(procedure)` | function-like(1 params), ## paste, multi-line(4) | `src/qpi/qpi_macros.h:230` | `protected:  			typedef QPI::NoData procedure##_locals;  			PRIVATE_PROCEDURE_WITH_LOCALS(p…` | PRIVATE_PROCEDURE_WITH_LOCALS | other:3 |
| `PRIVATE_PROCEDURE_WITH_LOCALS(procedure)` | function-like(1 params), ## paste, multi-line(5), __LINE__ | `src/qpi/qpi_macros.h:235` | `protected:  			enum { __is_function_##procedure = false, __id_##procedure = (CONTRACT_INDE…` | CONTRACT_INDEX, CONTRACT_STATE_TYPE | other:54 |
| `PUBLIC_FUNCTION(function)` | function-like(1 params), ## paste, multi-line(4) | `src/qpi/qpi_macros.h:241` | `public:  			typedef QPI::NoData function##_locals;  			PUBLIC_FUNCTION_WITH_LOCALS(functio…` | PUBLIC_FUNCTION_WITH_LOCALS | other:115 |
| `PUBLIC_FUNCTION_WITH_LOCALS(function)` | function-like(1 params), ## paste, multi-line(5), __LINE__ | `src/qpi/qpi_macros.h:246` | `public:  			enum { __is_function_##function = true };  			inline static void function(cons…` | CONTRACT_INDEX, CONTRACT_STATE_TYPE | other:143 |
| `PUBLIC_PROCEDURE(procedure)` | function-like(1 params), ## paste, multi-line(4) | `src/qpi/qpi_macros.h:252` | `public:  			typedef QPI::NoData procedure##_locals;  			PUBLIC_PROCEDURE_WITH_LOCALS(proce…` | PUBLIC_PROCEDURE_WITH_LOCALS | other:76 |
| `PUBLIC_PROCEDURE_WITH_LOCALS(procedure)` | function-like(1 params), ## paste, # stringize, multi-line(6), __LINE__ | `src/qpi/qpi_macros.h:257` | `public:  			enum { __is_function_##procedure = false, __id_##procedure = (CONTRACT_INDEX <…` | CONTRACT_INDEX, CONTRACT_STATE_TYPE, MAX_INPUT_SIZE | other:192 |
| `REGISTER_USER_FUNCTIONS_AND_PROCEDURES()` | function-like(0 params), multi-line(5), __LINE__ | `src/qpi/qpi_macros.h:264` | `public:  			enum { __contract_index = CONTRACT_INDEX };  			inline static void __registerU…` | CONTRACT_INDEX | other:34 |
| `REGISTER_USER_FUNCTION(userFunction, inputType)` | function-like(2 params), ## paste, # stringize, multi-line(7) | `src/qpi/qpi_macros.h:270` | `static_assert(__is_function_##userFunction, #userFunction " is procedure");  		static_asse…` |  | other:259 |
| `REGISTER_USER_PROCEDURE(userProcedure, inputType)` | function-like(2 params), ## paste, # stringize, multi-line(7) | `src/qpi/qpi_macros.h:278` | `static_assert(!__is_function_##userProcedure, #userProcedure " is function");  		static_as…` |  | other:267 |
| `REGISTER_USER_PROCEDURE_NOTIFICATION(userProcedure)` | function-like(1 params), ## paste, # stringize, multi-line(6) | `src/qpi/qpi_macros.h:287` | `static_assert(!__is_function_##userProcedure, #userProcedure " is function");  		static_as…` |  | other:4 |
| `CALL(functionOrProcedure, input, output)` | function-like(3 params), ## paste, # stringize, multi-line(4) | `src/qpi/qpi_macros.h:296` | `static_assert(sizeof(CONTRACT_STATE_TYPE::functionOrProcedure##_locals) <= MAX_SIZE_OF_CON…` | CONTRACT_STATE_TYPE | other:212 |
| `CALL_OTHER_CONTRACT_FUNCTION_E(contractStateType, function, input, output, errorVar)` | function-like(5 params), ## paste, # stringize, multi-line(17) | `src/qpi/qpi_macros.h:308` | `static_assert(sizeof(contractStateType::function##_locals) <= MAX_SIZE_OF_CONTRACT_LOCALS,…` | CALL, CONTRACT_STATE_TYPE |  |
| `CALL_OTHER_CONTRACT_FUNCTION(contractStateType, function, input, output)` | function-like(4 params), multi-line(2) | `src/qpi/qpi_macros.h:328` | `CALL_OTHER_CONTRACT_FUNCTION_E(contractStateType, function, input, output, interContractCa…` | CALL_OTHER_CONTRACT_FUNCTION_E | other:17 |
| `INVOKE_OTHER_CONTRACT_PROCEDURE_E(contractStateType, procedure, input, output, invocationReward, errorVar)` | function-like(6 params), ## paste, # stringize, multi-line(17) | `src/qpi/qpi_macros.h:334` | `static_assert(sizeof(contractStateType::procedure##_locals) <= MAX_SIZE_OF_CONTRACT_LOCALS…` | CALL, CONTRACT_STATE_TYPE | other:3 |
| `INVOKE_OTHER_CONTRACT_PROCEDURE(contractStateType, procedure, input, output, invocationReward)` | function-like(5 params), multi-line(2) | `src/qpi/qpi_macros.h:354` | `INVOKE_OTHER_CONTRACT_PROCEDURE_E(contractStateType, procedure, input, output, invocationR…` | INVOKE_OTHER_CONTRACT_PROCEDURE_E | other:21 |
| `QUERY_ORACLE(OracleInterface, query, userProcNotification, timeoutMillisec)` | function-like(4 params), ## paste | `src/qpi/qpi_macros.h:379` | `qpi.__qpiQueryOracle<OracleInterface>(query, userProcNotification, __id_##userProcNotifica…` |  | initializer:3, other:1 |
| `SUBSCRIBE_ORACLE(OracleInterface, query, userProcNotification, notificationPeriodInMilliseconds, notifyWithPreviousReply)` | function-like(5 params), ## paste | `src/qpi/qpi_macros.h:416` | `qpi.__qpiSubscribeOracle<OracleInterface>(query, userProcNotification, __id_##userProcNoti…` |  | initializer:3 |
| `INVOKE_OC(OcInterface, request)` | function-like(2 params) | `src/qpi/qpi_macros.h:433` | `qpi.__qpiInvokeOC<OcInterface>(request)` |  | initializer:1 |
| `SELF` | object-like | `src/qpi/qpi_macros.h:435` | `id(CONTRACT_INDEX, 0, 0, 0)` | CONTRACT_INDEX | parenthesized/args:199, initializer:139 |
| `SELF_INDEX` | object-like | `src/qpi/qpi_macros.h:437` | `CONTRACT_INDEX` | CONTRACT_INDEX | initializer:121, parenthesized/args:57 |
| `DEFINE_SHAREHOLDER_PROPOSAL_TYPES(numProposalSlots, assetNameInt64)` | function-like(2 params), multi-line(5) | `src/qpi/qpi_macros.h:441` | `public:  			typedef ProposalDataYesNo ProposalDataT;  			typedef ProposalAndVotingByShareh…` |  | other:1 |
| `IMPLEMENT_SetShareholderProposal(numFeeStateVariables, setProposalFeeVarOrValue)` | function-like(2 params), multi-line(17) | `src/qpi/qpi_macros.h:447` | `typedef ProposalDataT SetShareholderProposal_input;  		typedef uint16 SetShareholderPropos…` | PUBLIC_PROCEDURE |  |
| `IMPLEMENT_GetShareholderProposal()` | function-like(0 params), multi-line(6) | `src/qpi/qpi_macros.h:465` | `struct GetShareholderProposal_input { uint16 proposalIndex; };  		struct GetShareholderPro…` | PUBLIC_FUNCTION | other:1 |
| `IMPLEMENT_GetShareholderProposalIndices()` | function-like(0 params), multi-line(14) | `src/qpi/qpi_macros.h:472` | `struct GetShareholderProposalIndices_input { bit activeProposals; sint32 prevProposalIndex…` | PUBLIC_FUNCTION | other:1 |
| `IMPLEMENT_GetShareholderProposalFees(setProposalFeeVarOrValue)` | function-like(1 params), multi-line(6) | `src/qpi/qpi_macros.h:487` | `typedef NoData GetShareholderProposalFees_input;  		struct GetShareholderProposalFees_outp…` | PUBLIC_FUNCTION | other:1 |
| `IMPLEMENT_SetShareholderVotes()` | function-like(0 params), multi-line(6) | `src/qpi/qpi_macros.h:494` | `typedef ProposalMultiVoteDataV1 SetShareholderVotes_input;  		typedef bit SetShareholderVo…` | PUBLIC_PROCEDURE | other:1 |
| `IMPLEMENT_GetShareholderVotes()` | function-like(0 params), multi-line(5) | `src/qpi/qpi_macros.h:500` | `struct GetShareholderVotes_input { id voter; uint16 proposalIndex; };  		typedef ProposalM…` | PUBLIC_FUNCTION | other:1 |
| `IMPLEMENT_GetShareholderVotingResults()` | function-like(0 params), multi-line(5) | `src/qpi/qpi_macros.h:506` | `struct GetShareholderVotingResults_input { uint16 proposalIndex; };  		typedef ProposalSum…` | PUBLIC_FUNCTION | other:1 |
| `IMPLEMENT_SET_SHAREHOLDER_PROPOSAL()` | function-like(0 params), multi-line(5) | `src/qpi/qpi_macros.h:512` | `struct SET_SHAREHOLDER_PROPOSAL_locals { SetShareholderProposal_input userProcInput; };  	…` | CALL, SET_SHAREHOLDER_PROPOSAL_WITH_LOCALS | other:1 |
| `IMPLEMENT_SET_SHAREHOLDER_VOTES()` | function-like(0 params), multi-line(3) | `src/qpi/qpi_macros.h:518` | `SET_SHAREHOLDER_VOTES() {  			CALL(SetShareholderVotes, input, output); }` | CALL, SET_SHAREHOLDER_VOTES | other:1 |
| `IMPLEMENT_FinalizeShareholderStateVarProposals()` | function-like(0 params), multi-line(24) | `src/qpi/qpi_macros.h:523` | `struct FinalizeShareholderProposalSetStateVar_input {  			sint32 proposalIndex; ProposalDa…` | CALL, PRIVATE_PROCEDURE, PRIVATE_PROCEDURE_WITH_LOCALS | other:1 |
| `IMPLEMENT_DEFAULT_SHAREHOLDER_PROPOSAL_VOTING(numFeeStateVariables, setProposalFeeVarOrValue)` | function-like(2 params), multi-line(10) | `src/qpi/qpi_macros.h:548` | `IMPLEMENT_SetShareholderProposal(numFeeStateVariables, setProposalFeeVarOrValue)  		IMPLEM…` | IMPLEMENT_GetShareholderProposal, IMPLEMENT_GetShareholderPr | other:1 |
| `REGISTER_GetShareholderProposalFees()` | function-like(0 params) | `src/qpi/qpi_macros.h:559` | `REGISTER_USER_FUNCTION(GetShareholderProposalFees, 65531)` | REGISTER_USER_FUNCTION |  |
| `REGISTER_GetShareholderProposalIndices()` | function-like(0 params) | `src/qpi/qpi_macros.h:560` | `REGISTER_USER_FUNCTION(GetShareholderProposalIndices, 65532)` | REGISTER_USER_FUNCTION |  |
| `REGISTER_GetShareholderProposal()` | function-like(0 params) | `src/qpi/qpi_macros.h:561` | `REGISTER_USER_FUNCTION(GetShareholderProposal, 65533)` | REGISTER_USER_FUNCTION |  |
| `REGISTER_GetShareholderVotes()` | function-like(0 params) | `src/qpi/qpi_macros.h:562` | `REGISTER_USER_FUNCTION(GetShareholderVotes, 65534)` | REGISTER_USER_FUNCTION |  |
| `REGISTER_GetShareholderVotingResults()` | function-like(0 params) | `src/qpi/qpi_macros.h:563` | `REGISTER_USER_FUNCTION(GetShareholderVotingResults, 65535)` | REGISTER_USER_FUNCTION |  |
| `REGISTER_SetShareholderProposal()` | function-like(0 params) | `src/qpi/qpi_macros.h:564` | `REGISTER_USER_PROCEDURE(SetShareholderProposal, 65534)` | REGISTER_USER_PROCEDURE |  |
| `REGISTER_SetShareholderVotes()` | function-like(0 params) | `src/qpi/qpi_macros.h:565` | `REGISTER_USER_PROCEDURE(SetShareholderVotes, 65535)` | REGISTER_USER_PROCEDURE |  |
| `REGISTER_SHAREHOLDER_PROPOSAL_VOTING()` | function-like(0 params), multi-line(4) | `src/qpi/qpi_macros.h:567` | `REGISTER_GetShareholderProposalFees()  		REGISTER_GetShareholderProposalIndices(); REGISTE…` | REGISTER_GetShareholderProposal, REGISTER_GetShareholderProp | other:2 |
| `NULL_ID` | object-like | `src/qpi/qpi_types.h:22` | `id::zero()` |  | parenthesized/args:191, initializer:57, other:5, template-arg:1 |
