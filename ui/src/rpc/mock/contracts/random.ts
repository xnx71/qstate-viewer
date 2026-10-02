import { F } from '../types';
import { hashSet } from '../containers';
import { G, gen, liveWhen } from '../gens';
import { mix, u01 } from '../prng';
import type { ContractDef, Kit } from './kit';
import { skewedPick } from './kit';

export function buildRandom(k: Kit): ContractDef {
  const { t } = k;
  const miners = k.pool('RANDOM', 'miners', 300);
  const commit = t.struct(
    'RANDOM::Commitment',
    [
      F('miner', t.id, gen(G.poolId(miners, skewedPick(miners.size, 1.5)))),
      F('amount', t.u64, gen(G.range(8, 1_000_000, 90_000_000))),
      F('tick', t.i64, gen(G.range(8, 25_000_000, 27_400_000))),
      F('digest', t.arr(t.u8, 32), gen(G.bytes(32))),
      F('round', t.u32, gen(G.range(4, 1, 400))),
      F('state', t.u8, gen(G.range(1, 0, 3))),
    ],
    { source: { file: 'src/contracts/Random.h', line: 44 } },
  );
  const revealed = hashSet(t, t.id, 2048, {
    tag: k.tag('RANDOM', 'revealed'),
    density: 0.5,
    removedFrac: 0.01,
    popDelta: 3,
    key: gen(G.poolId(miners, (_c, salt) => Math.floor(u01(mix(salt, 3)) * miners.size))),
  });
  const root = t.struct(
    'RANDOM::StateData',
    [
      F('_totalRevenue', t.u64, gen(G.growing(8, 21_450_000, 800))),
      F('_minDeposit', t.u32, gen(G.constant(4, 1_000_000))),
      F('_round', t.u32, gen(G.constant(4, 401))),
      F('_revealed', revealed.type),
      F('_commits', t.qarray(commit, 1024), liveWhen((_c, _s, i) => i < 700)),
    ],
    { source: { file: 'src/contracts/Random.h', line: 90 } },
  );
  return {
    index: 3,
    name: 'RANDOM',
    structName: 'RANDOM',
    stateTypeName: 'RANDOM::StateData',
    headerFile: 'src/contracts/Random.h',
    constructionEpoch: 69,
    destructionEpoch: 65535,
    root,
    salt: k.tag('RANDOM', 'root'),
  };
}
