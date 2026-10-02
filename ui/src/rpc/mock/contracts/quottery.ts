import { F } from '../types';
import { G, gen, liveWhen } from '../gens';
import { mix, below, u01 } from '../prng';
import type { ContractDef, Kit } from './kit';
import { uniformPick } from './kit';

const BET_NAMES = [
  'BTC above 120k by Dec', 'Qubic tick 30M this epoch', 'ETH flips BTC fees', 'Next CCF proposal passes',
  'QX volume > 1B', 'Computor churn < 20', 'Epoch 193 starts on time', 'MLM listing on QX',
  'Hashrate record', 'Oracle quorum reached',
];
const DESCRIPTIONS = [
  'Resolves YES if the settlement oracle reports the condition satisfied at close time; fees follow the contract-defined schedule and unclaimed pools are burned after 30 epochs.',
  'Simple binary market. Settlement by quorum of five providers; disputes are handled off-chain by the computor governance process described in the contract documentation.',
  'Scalar market with fixed odds table; payouts are pro rata to stake.',
];

export function buildQuottery(k: Kit): ContractDef {
  const { t } = k;
  const people = k.pool('QUOTTERY', 'people', 120);
  const status = t.enum('QUOTTERY::BetStatus', t.u8, [
    { name: 'Draft', value: 0 },
    { name: 'Open', value: 1 },
    { name: 'Closed', value: 2 },
    { name: 'Resolved', value: 3 },
    { name: 'Cancelled', value: 4 },
  ]);

  const provider = t.struct(
    'QUOTTERY::Provider',
    [
      F('pubkey', t.id, gen(G.idSparse(G.poolId(people, uniformPick(people.size)), 0.85))),
      F('feeBp', t.u32, gen(G.range(4, 10, 250))),
      F('reserved', t.u32, gen(G.zero)),
      F('lastReport', t.dateTime, gen(G.dateTime(2026, 2026, 0.1))),
    ],
    { source: { file: 'src/contracts/Quottery.h', line: 58 } },
  );
  const oracle = t.struct(
    'QUOTTERY::OracleCfg',
    [
      F('providers', t.qarray(provider, 6)),
      F('quorum', t.u8, gen(G.range(1, 2, 5))),
      F('flags', t.u8, gen(G.range(1, 0, 7))),
      F('version', t.u16, gen(G.constant(2, 3))),
    ],
    { source: { file: 'src/contracts/Quottery.h', line: 71 } },
  );
  const terms = t.struct(
    'QUOTTERY::Terms',
    [
      F('oracle', oracle),
      F('maxPool', t.u128, gen({
        write: (out, o, _c, salt) => {
          // uint128: crosses 2^64 for roughly a third of the bets
          const big = u01(mix(salt, 5)) < 0.33;
          const lo = mix(salt, 6);
          const lo2 = mix(salt, 7);
          for (let i = 0; i < 4; i++) out[o + i] = (lo >>> (i * 8)) & 255;
          for (let i = 0; i < 4; i++) out[o + 4 + i] = (lo2 >>> (i * 8)) & 255;
          if (big) {
            const hi = 1 + below(mix(salt, 8), 5000);
            for (let i = 0; i < 4; i++) out[o + 8 + i] = (hi >>> (i * 8)) & 255;
          }
        },
      })),
      F('minStake', t.i64, gen(G.range(8, 1000, 5_000_000))),
      F('durationDays', t.u32, gen(G.range(4, 1, 90))),
    ],
    { source: { file: 'src/contracts/Quottery.h', line: 88 } },
  );
  const payload = t.union(
    'QUOTTERY::Payload',
    [
      F('numeric', t.u64, gen(G.range(8, 1, 1_000_000))),
      F('raw', t.arr(t.u8, 16), gen(G.bytes(16))),
      F('target', t.id, gen(G.poolId(people, uniformPick(people.size)))),
      F('ratio', t.f64, gen(G.float64(0, 1))),
    ],
    { source: { file: 'src/contracts/Quottery.h', line: 101 } },
  );
  const payloadSpec = { active: (_c: unknown, salt: number) => below(mix(salt, 99), 4) };

  const bet = t.struct(
    'QUOTTERY::BetInfo',
    [
      F('betId', t.u32, gen(G.seq(4, 1000))),
      F('status', status),
      F('grade', t.char, gen(G.num(1, (_c, salt) => 65 + (mix(salt, 8) % 6)))),
      F('settled', t.bool, gen(G.byRow((r) => r % 3 === 0, G.constant(1, 1), G.zero))),
      F('name', t.arr(t.char, 32), gen(G.text(BET_NAMES))),
      F('creator', t.id, gen(G.poolId(people, uniformPick(people.size)))),
      F('openDate', t.dateTime, gen(G.dateTime(2026, 2026))),
      F('closeDate', t.dateTime, gen(G.dateTime(2026, 2027, 0.1))),
      F('endDate', t.dateTime, gen(G.dateTime(2026, 2027, 0.3))),
      F('terms', terms),
      F('payload', payload, payloadSpec),
      F('totalPool', t.u128, gen({
        write: (out, o, c, salt) => {
          const v = 3_000_000 + below(mix(salt, 21), 900_000_000) + (c.gen % 7) * 1111;
          const lo = v >>> 0;
          for (let i = 0; i < 4; i++) out[o + i] = (lo >>> (i * 8)) & 255;
          out[o + 4] = Math.floor(v / 4294967296) & 255;
        },
      })),
      F('odds', t.f32, gen(G.float32(1.05, 9.5))),
      F('houseEdge', t.f64, gen(G.float64(0.005, 0.06, true))),
      F('delta', t.i32, gen(G.num(4, (_c, salt) => below(mix(salt, 31), 20001) - 10000))),
      F('description', t.arr(t.char, 160), gen(G.text(DESCRIPTIONS))),
      F('digest', t.arr(t.u8, 32), gen(G.bytes(32))),
      F('cache', t.ptr, gen(G.ptr(0.07))),
      F('blob', t.arr(t.u8, 40), gen(G.bytes(40))),
    ],
    { source: { file: 'src/contracts/Quottery.h', line: 120 } },
  );

  const LIVE = 41;
  const root = t.struct(
    'QUOTTERY::StateData',
    [
      F('_feeCollected', t.u64, gen(G.growing(8, 77_000_000, 9_000))),
      F('_numberOfBets', t.u32, gen(G.constant(4, LIVE))),
      F('_flags', t.u32, gen(G.constant(4, 0))),
      F('_bets', t.qarray(bet, 256), liveWhen((_c, _s, i) => i < LIVE)),
      F('_salt', t.arr(t.u8, 32), gen(G.bytes(32))),
      F('_admin', t.id, gen(G.poolId(people, () => 0))),
    ],
    { source: { file: 'src/contracts/Quottery.h', line: 210 } },
  );
  return {
    index: 2,
    name: 'QUOTTERY',
    structName: 'QUOTTERY',
    stateTypeName: 'QUOTTERY::StateData',
    headerFile: 'src/contracts/Quottery.h',
    constructionEpoch: 67,
    destructionEpoch: 65535,
    root,
    salt: k.tag('QUOTTERY', 'root'),
  };
}
