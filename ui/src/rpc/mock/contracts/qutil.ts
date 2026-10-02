import { F, BF } from '../types';
import { hashMap, hashSet } from '../containers';
import { G, gen, each, liveFirst, num } from '../gens';
import type { GenSpec } from '../gens';
import { mix, below, u01 } from '../prng';
import type { ContractDef, Kit } from './kit';
import { ASSET_NAMES, TITLES, uniformPick } from './kit';

export const QUTIL_USERS_CAPACITY = 1048576;

export function buildQutil(k: Kit): ContractDef {
  const { t } = k;
  const people = k.pool('QUTIL', 'people', 40);
  const status = t.enum('QUTIL::PollStatus', t.u8, [
    { name: 'Draft', value: 0 },
    { name: 'Open', value: 1 },
    { name: 'Closed', value: 2 },
    { name: 'Cancelled', value: 3 },
  ]);

  const pollCount = 7;
  const poll = t.struct(
    'QUTIL::PollInfo',
    [
      F('creator', t.id, gen(G.poolId(people, uniformPick(people.size)))),
      F('assetName', t.u64, gen(G.byRow((r) => r === 0, G.assetNameAt('QWALLET'), G.assetName(ASSET_NAMES)))),
      F('minAmount', t.u64, gen(G.range(8, 1, 5_000_000))),
      F('created', t.dateTime, gen(G.dateTime(2026, 2026))),
      F('closes', t.dateTime, gen(G.dateTime(2026, 2027, 0.15))),
      F('status', status),
      F('optionCount', t.u8, gen(G.range(1, 2, 6))),
      F('title', t.arr(t.char, 32), gen(G.text(TITLES))),
      F('votes', t.qarray(t.u32, 8), each(G.vol(4, 1000, 400))),
      F('tally', t.arr(t.i64, 4), each(G.num(8, (_c, salt) => below(mix(salt, 1), 2000000) - 700000))),
    ],
    { source: { file: 'src/contracts/QUtil.h', line: 74 } },
  );

  const limits = t.struct(
    'QUTIL::Limits',
    [
      F('daily', t.u64, gen(G.range(8, 1000, 900_000))),
      F('monthly', t.u64, gen(G.range(8, 100000, 90_000_000))),
      F('delegate', t.id, gen(G.idSparse(G.contractId([4, 9, 2]), 0.07))),
    ],
    { source: { file: 'src/contracts/QUtil.h', line: 112 } },
  );
  const stats = t.struct(
    'QUTIL::Stats',
    [
      F('transfers', t.u32, gen(G.range(4, 0, 4000))),
      F('votes', t.u32, gen(G.range(4, 0, 90))),
      F('limits', limits),
    ],
    { source: { file: 'src/contracts/QUtil.h', line: 121 } },
  );
  const user = t.struct(
    'QUTIL::UserEntry',
    [
      F('balance', t.i64, gen(num(8, (c, salt, row) => {
        const v = below(mix(salt, 0xba1), 2_000_000_000_000) - 300_000_000_000;
        return row % 997 === 0 ? v + c.gen * 1000 : v; // a sprinkle of volatile balances
      }))),
      F('lastTick', t.u64, gen(G.range(8, 20_000_000, 27_500_000))),
      F('stats', stats),
      F('lastSeen', t.dateTime, gen(G.dateTime(2025, 2026, 0.03))),
      BF('tier', t.u32, 4, { bit: (_c, salt) => below(mix(salt, 11), 6) }),
      BF('verified', t.u32, 1, { bit: (_c, salt) => (u01(mix(salt, 12)) < 0.6 ? 1 : 0) }),
      BF('banned', t.u32, 1, { bit: (_c, salt) => (u01(mix(salt, 13)) < 0.02 ? 1 : 0) }),
      BF('reserved', t.u32, 26, { bit: () => 0 }),
    ],
    { source: { file: 'src/contracts/QUtil.h', line: 133 } },
  );

  const blocked = hashSet(t, t.id, 1024, {
    tag: k.tag('QUTIL', 'blocked'),
    density: 0.31,
    removedFrac: 0.02,
    popDelta: 2, // stored counter disagrees with the flags: exercises ContainerStats.warning
    key: gen(G.idUnique(k.tag('QUTIL', 'blockedKey'))),
  });
  const keySpec: GenSpec = gen(G.idUnique(k.tag('QUTIL', 'userKey')));
  const users = hashMap(t, t.id, user, QUTIL_USERS_CAPACITY, {
    tag: k.tag('QUTIL', 'users'),
    density: 0.4,
    removedFrac: 0.004,
    key: keySpec,
  });

  const root = t.struct(
    'QUTIL::StateData',
    [
      F('_totalFees', t.u64, gen(G.growing(8, 912_345_000, 40_000))),
      F('_pollCount', t.u32, gen(G.constant(4, pollCount))),
      F('_flags', t.u32, gen(G.constant(4, 0x5))),
      F('_polls', t.qarray(poll, 64), liveFirst(() => pollCount)),
      F('_featureFlags', t.bitArray(1024), each(G.bitWords(1024, 0.3))),
      F('_blocked', blocked.type),
      F('_users', users.type),
    ],
    { source: { file: 'src/contracts/QUtil.h', line: 160 } },
  );
  return {
    index: 4,
    name: 'QUTIL',
    structName: 'QUTIL',
    stateTypeName: 'QUTIL::StateData',
    headerFile: 'src/contracts/QUtil.h',
    constructionEpoch: 70,
    destructionEpoch: 65535,
    root,
    salt: k.tag('QUTIL', 'root'),
  };
}
