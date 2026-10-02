import { F } from '../types';
import { collection } from '../containers';
import { G, gen, liveWhen, num } from '../gens';
import { mix, below } from '../prng';
import type { ContractDef, Kit } from './kit';
import { skewedPick } from './kit';

export function buildQearn(k: Kit): ContractDef {
  const { t } = k;
  const lockers = k.pool('QEARN', 'lockers', 3000);
  const round = t.struct(
    'QEARN::RoundInfo',
    [
      F('totalLocked', t.u64, gen(G.range(8, 10_000_000_000, 90_000_000_000))),
      F('earned', t.u64, gen(G.vol(8, 120_000_000, 5_000_000))),
      F('startEpoch', t.u32, gen(G.seq(4, 140))),
      F('endEpoch', t.u32, gen(G.seq(4, 192))),
      F('apy', t.f32, gen(G.float32(2, 14))),
      F('ratio', t.f64, gen(G.float64(0.001, 0.4, true))),
      F('delta', t.i32, gen(num(4, (_c, salt) => below(mix(salt, 4), 4001) - 2000))),
      F('reserved', t.u32, gen(G.zero)),
    ],
    { source: { file: 'src/contracts/Qearn.h', line: 36 } },
  );
  const locker = t.struct(
    'QEARN::Locker',
    [
      F('owner', t.id, gen(G.poolId(lockers, skewedPick(lockers.size, 1.7)))),
      F('amount', t.u64, gen(G.range(8, 10_000_000, 12_000_000_000))),
      F('lockEpoch', t.u32, gen(G.range(4, 140, 192))),
      F('flags', t.u32, gen(G.range(4, 0, 3))),
    ],
    { source: { file: 'src/contracts/Qearn.h', line: 49 } },
  );
  const lk = collection(t, locker, 65536, {
    population: (c) => 11842 + (c.epoch - 192) * 40,
    povCount: () => 413,
    priority: G.num(8, (_c, salt) => 140 + below(mix(salt, 6), 53)),
    povValue: G.idUnique(k.tag('QEARN', 'pov')),
  });
  const root = t.struct(
    'QEARN::StateData',
    [
      F('_treasury', t.id, gen(G.idText(['QEARN-TREASURY']))),
      F('_qxContract', t.id, gen(G.idBytes(Uint8Array.from([1, ...new Array<number>(31).fill(0)])))),
      F('_totalLocked', t.u64, gen(G.u64Random())),
      F('_emissions', t.u128, gen({
        write: (out, o, c, salt) => {
          const lo = mix(salt, 1) + c.gen * 977;
          for (let i = 0; i < 4; i++) out[o + i] = (lo >>> (i * 8)) & 255;
          out[o + 8] = 3;
        },
      })),
      F('_rounds', t.qarray(round, 256), liveWhen((_c, _s, i) => i < 53)),
      F('_lockers', lk),
    ],
    { source: { file: 'src/contracts/Qearn.h', line: 77 } },
  );
  return {
    index: 9,
    name: 'QEARN',
    structName: 'QEARN',
    stateTypeName: 'QEARN::StateData',
    headerFile: 'src/contracts/Qearn.h',
    constructionEpoch: 188,
    destructionEpoch: 65535,
    root,
    salt: k.tag('QEARN', 'root'),
  };
}
