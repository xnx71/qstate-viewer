import { F } from '../types';
import { G, gen, each, liveWhen, num } from '../gens';
import { mix, below, u01 } from '../prng';
import type { ContractDef, Kit } from './kit';
import { TITLES } from './kit';

export function buildGqmprop(k: Kit): ContractDef {
  const { t } = k;
  const computor = t.struct(
    'GQMPROP::Computor',
    [
      F('pubkey', t.id, gen(G.idUnique(k.tag('GQMPROP', 'computor')))),
      F('votes', t.u64, gen(G.range(8, 0, 676))),
      F('rating', t.i64, gen(num(8, (_c, salt) => below(mix(salt, 3), 2001) - 1000))),
      F('lastVoteEpoch', t.u32, gen(G.range(4, 185, 192))),
      F('flags', t.u32, gen(G.range(4, 0, 15))),
    ],
    { source: { file: 'src/contracts/GeneralQuorumProposal.h', line: 52 } },
  );
  // Voters: only ~14% of the 1024 slots are used (the others are all-zero): the hideEmpty showcase.
  const voter = t.struct(
    'GQMPROP::Voter',
    [
      F('voter', t.id, gen(G.idUnique(k.tag('GQMPROP', 'voter')))),
      F('weight', t.u64, gen(G.range(8, 1, 250_000))),
      F('lastEpoch', t.u32, gen(G.range(4, 188, 192))),
      F('choice', t.u32, gen(G.range(4, 1, 4))),
    ],
    { source: { file: 'src/contracts/GeneralQuorumProposal.h', line: 63 } },
  );
  const proposal = t.struct(
    'GQMPROP::Proposal',
    [
      F('proposer', t.id, gen(G.idUnique(k.tag('GQMPROP', 'proposer')))),
      F('title', t.arr(t.char, 48), gen(G.text(TITLES))),
      F('opens', t.dateTime, gen(G.dateTime(2026, 2026))),
      F('closes', t.dateTime, gen(G.dateTime(2026, 2026, 0.2))),
      F('yes', t.u32, gen(G.vol(4, 100, 80))),
      F('no', t.u32, gen(G.vol(4, 40, 60))),
    ],
    { source: { file: 'src/contracts/GeneralQuorumProposal.h', line: 74 } },
  );
  const root = t.struct(
    'GQMPROP::StateData',
    [
      F('_epoch', t.u32, gen(G.constant(4, 192))),
      F('_computors', t.qarray(computor, 676), liveWhen((_c, salt, i) => u01(mix(salt, i + 0x6c)) < 0.97)),
      F('_voters', t.qarray(voter, 1024), liveWhen((_c, salt, i) => u01(mix(salt, i + 0x7e)) < 0.14)),
      F('_proposals', t.qarray(proposal, 64), liveWhen((_c, _s, i) => i < 9)),
      F('_computorFlags', t.bitArray(676), each(G.bitWords(676, 0.55))),
      F('_tallies', t.qarray(t.i64, 256), {
        live: (_c, salt, i) => u01(mix(salt, i + 0x99)) < 0.2,
        elem: { gen: num(8, (_c, salt) => below(mix(salt, 5), 90001) - 20000) },
      }),
    ],
    { source: { file: 'src/contracts/GeneralQuorumProposal.h', line: 120 } },
  );
  return {
    index: 6,
    name: 'GQMPROP',
    structName: 'GQMPROP',
    stateTypeName: 'GQMPROP::StateData',
    headerFile: 'src/contracts/GeneralQuorumProposal.h',
    constructionEpoch: 74,
    destructionEpoch: 65535,
    root,
    salt: k.tag('GQMPROP', 'root'),
  };
}
