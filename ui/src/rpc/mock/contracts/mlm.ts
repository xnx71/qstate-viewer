import { F } from '../types';
import { G, each } from '../gens';
import type { ContractDef, Kit } from './kit';

/**
 * MLM: its layout "cannot be computed" (schema-error). The mock keeps an opaque byte blob so that raw bytes,
 * search and the digest still work, but no tree is exposed.
 */
export const MLM_FILE_SIZE = 4_872_192;

export function buildMlm(k: Kit): ContractDef {
  const { t } = k;
  const root = t.struct('MLM::StateData', [F('_opaque', t.arr(t.u64, MLM_FILE_SIZE / 8), each(G.u64Random()))], {
    source: { file: 'src/contracts/MLM.h', line: 88 },
  });
  return {
    index: 5,
    name: 'MLM',
    structName: 'MLM',
    stateTypeName: 'MLM::StateData',
    headerFile: 'src/contracts/MLM.h',
    constructionEpoch: 73,
    destructionEpoch: 65535,
    root,
    salt: k.tag('MLM', 'root'),
    opaque: true,
  };
}
