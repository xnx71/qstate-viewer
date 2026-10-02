import { F } from '../types';
import { G, gen, num } from '../gens';
import { mix, below } from '../prng';
import type { ContractDef, Kit } from './kit';

export function buildContract0(k: Kit): ContractDef {
  const { t } = k;
  const root = t.struct(
    'Contract0State',
    [
      F('contractFeeReserves', t.arr(t.i64, 1024), {
        live: (_c, _s, i) => i < 10,
        elem: { gen: num(8, (c, salt, row) => 5_000_000_000 + below(mix(salt, 3), 90_000_000_000) + (row === 1 ? c.gen * 12_345 : 0)) },
      }),
      F('_burnedAmount', t.u64, gen(G.growing(8, 987_654_321_000, 100_000))),
      F('_lastEpoch', t.u32, gen(G.constant(4, 192))),
      F('_flags', t.u32, gen(G.constant(4, 3))),
      F('_burnAddress', t.id, gen(G.zero)),
    ],
    { source: { file: 'src/contract_core/contract_def.h', line: 12 } },
  );
  return {
    index: 0,
    name: '',
    stateTypeName: 'Contract0State',
    constructionEpoch: 0,
    destructionEpoch: 65535,
    root,
    salt: k.tag('C0', 'root'),
  };
}
