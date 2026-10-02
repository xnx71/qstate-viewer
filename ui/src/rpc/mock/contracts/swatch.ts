import { F } from '../types';
import { G, gen } from '../gens';
import type { ContractDef, Kit } from './kit';

/** SWATCH (supply watcher): known to the schema but has no state file in the mock directories. */
export function buildSwatch(k: Kit): ContractDef {
  const { t } = k;
  const root = t.struct(
    'SWATCH::StateData',
    [
      F('_lastCheckTick', t.u32, gen(G.constant(4, 26_800_000))),
      F('_alarms', t.u32, gen(G.constant(4, 0))),
      F('_watched', t.qarray(t.id, 256)),
    ],
    { source: { file: 'src/contracts/SupplyWatcher.h', line: 31 } },
  );
  return {
    index: 7,
    name: 'SWATCH',
    structName: 'SWATCH',
    stateTypeName: 'SWATCH::StateData',
    headerFile: 'src/contracts/SupplyWatcher.h',
    constructionEpoch: 177,
    destructionEpoch: 65535,
    root,
    salt: k.tag('SWATCH', 'root'),
  };
}
