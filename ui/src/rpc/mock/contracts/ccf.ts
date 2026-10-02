import { F } from '../types';
import { hashSet, linkedList } from '../containers';
import { G, gen, liveWhen, num } from '../gens';
import { mix, below } from '../prng';
import type { ContractDef, Kit } from './kit';
import { TITLES } from './kit';

export function buildCcf(k: Kit): ContractDef {
  const { t } = k;
  const ptype = t.enum('CCF::ProposalType', t.u16, [
    { name: 'Transfer', value: 0 },
    { name: 'Subscription', value: 1 },
    { name: 'Cancel', value: 2 },
    { name: 'Spec', value: 3 },
  ]);
  const transfer = t.struct(
    'CCF::TransferBody',
    [F('destination', t.id, gen(G.idUnique(k.tag('CCF', 'dest')))), F('amount', t.u64, gen(G.range(8, 1_000_000, 900_000_000)))],
    { source: { file: 'src/contracts/ComputorControlledFund.h', line: 40 } },
  );
  const subscription = t.struct(
    'CCF::SubscriptionBody',
    [
      F('destination', t.id, gen(G.idUnique(k.tag('CCF', 'dest2')))),
      F('amount', t.u64, gen(G.range(8, 1_000_000, 90_000_000))),
      F('periods', t.u32, gen(G.range(4, 1, 52))),
      F('weeksPerPeriod', t.u32, gen(G.range(4, 1, 4))),
      F('start', t.dateTime, gen(G.dateTime(2026, 2027))),
    ],
    { source: { file: 'src/contracts/ComputorControlledFund.h', line: 47 } },
  );
  const body = t.union(
    'CCF::Body',
    [F('transfer', transfer), F('subscription', subscription), F('raw', t.arr(t.u8, 64), gen(G.bytes(64)))],
    { source: { file: 'src/contracts/ComputorControlledFund.h', line: 58 } },
  );
  const proposal = t.struct(
    'CCF::Proposal',
    [
      F('proposer', t.id, gen(G.idUnique(k.tag('CCF', 'proposer')))),
      F('type', ptype),
      F('flags', t.u16, gen(G.range(2, 0, 7))),
      F('epoch', t.u32, gen(G.range(4, 186, 192))),
      F('url', t.arr(t.char, 64), gen(G.text(['https://ccf.example/p/17', 'https://forum.example/t/4412', 'ipfs://bafy2bzace']))),
      F('body', body, { active: (_c, salt) => below(mix(salt, 7), 3) }),
      F('yes', t.u32, gen(G.range(4, 0, 676))),
      F('no', t.u32, gen(G.range(4, 0, 300))),
      F('netScore', t.i32, gen(num(4, (_c, salt) => below(mix(salt, 9), 1301) - 400))),
    ],
    { source: { file: 'src/contracts/ComputorControlledFund.h', line: 71 } },
  );
  const history = t.struct(
    'CCF::HistoryEntry',
    [
      F('actor', t.id, gen(G.idUnique(k.tag('CCF', 'actor')))),
      F('value', t.u64, gen(G.range(8, 0, 5_000_000))),
      F('when', t.dateTime, gen(G.dateTime(2026, 2026, 0.02))),
    ],
    { source: { file: 'src/contracts/ComputorControlledFund.h', line: 90 } },
  );
  const ballots = hashSet(t, t.id, 4096, {
    tag: k.tag('CCF', 'ballots'),
    density: 0.37,
    removedFrac: 0.01,
    key: gen(G.idUnique(k.tag('CCF', 'ballotKey'))),
  });
  const hist = linkedList(t, history, 512, { population: () => 210 });
  const root = t.struct(
    'CCF::StateData',
    [
      F('_balanceHint', t.u64, gen(G.growing(8, 2_200_000_000, 5_000))),
      F('_proposals', t.qarray(proposal, 128), liveWhen((_c, _s, i) => i < 23)),
      F('_ballots', ballots.type),
      F('_history', hist),
      F('_lastTally', t.arr(t.i64, 16), { elem: { gen: G.num(8, (_c, salt) => below(mix(salt, 5), 2001) - 700) } }),
    ],
    { source: { file: 'src/contracts/ComputorControlledFund.h', line: 130 } },
  );
  void TITLES;
  return {
    index: 8,
    name: 'CCF',
    structName: 'CCF',
    stateTypeName: 'CCF::StateData',
    headerFile: 'src/contracts/ComputorControlledFund.h',
    constructionEpoch: 189,
    destructionEpoch: 65535,
    root,
    salt: k.tag('CCF', 'root'),
  };
}
