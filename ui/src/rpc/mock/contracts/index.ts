import { Types } from '../types';
import type { TypeInfo } from '../../contract';
import { makeKit } from './kit';
import type { ContractDef } from './kit';
import { buildContract0 } from './contract0';
import { buildQx } from './qx';
import { buildQuottery } from './quottery';
import { buildRandom } from './random';
import { buildQutil } from './qutil';
import { buildMlm } from './mlm';
import { buildGqmprop } from './gqmprop';
import { buildSwatch } from './swatch';
import { buildCcf } from './ccf';
import { buildQearn } from './qearn';

export type { ContractDef } from './kit';

export interface World {
  seed: number;
  types: Types;
  contracts: ContractDef[];
  byIndex(index: number): ContractDef | undefined;
  typeInfo(id: number): TypeInfo | undefined;
}

/** Builds all synthetic contracts for a seed. The type table order is fixed, so TypeIds do not depend on the seed. */
export function buildWorld(seed: number): World {
  const types = new Types();
  const kit = makeKit(types, seed);
  const contracts = [
    buildContract0(kit),
    buildQx(kit),
    buildQuottery(kit),
    buildRandom(kit),
    buildQutil(kit),
    buildMlm(kit),
    buildGqmprop(kit),
    buildSwatch(kit),
    buildCcf(kit),
    buildQearn(kit),
  ];
  return {
    seed,
    types,
    contracts,
    byIndex: (i) => contracts.find((c) => c.index === i),
    typeInfo: (id) => {
      const n = types.nodes[id];
      return n ? types.info(n) : undefined;
    },
  };
}
