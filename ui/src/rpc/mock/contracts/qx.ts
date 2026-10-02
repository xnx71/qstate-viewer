import { F } from '../types';
import { collection } from '../containers';
import { G, gen, num, writeAscii, write64, writeInt } from '../gens';
import type { LeafGen } from '../gens';
import { mix, below, u01 } from '../prng';
import { contractIdBytes } from '../ids';
import type { ContractDef, Kit } from './kit';
import { ASSET_NAMES, skewedPick } from './kit';

export const QX_CAPACITY = 2097152;
const POV_COUNT = 39873;
const POP_BASE = 1623411;

export function buildQx(k: Kit): ContractDef {
  const { t } = k;
  const entityPool = k.pool('QX', 'entity', 8192, [contractIdBytes(4), contractIdBytes(9), contractIdBytes(8)]);

  // numberOfShares: rows < 64 are volatile (animate after a change), a few rows are huge (> 2^53).
  const shares: LeafGen = {
    write: (out, o, c, salt, row) => {
      if (row % 4099 === 3) {
        write64(out, o, mix(salt, 0xb16), (mix(salt, 0xb17) | 0x00400000) >>> 0);
      } else if (row < 64) {
        writeInt(out, o, 8, 1000 + below(mix(salt, c.gen * 7919 + 13), 90000));
      } else {
        writeInt(out, o, 8, 1 + below(mix(salt, 0x5a), 5_000_000));
      }
    },
  };

  // priority = price (negative for sell side in this mock): signed 64-bit.
  const priority = num(8, (c, salt, row) => {
    const price = 1 + below(mix(salt, 0x9a1), 4_000_000_000);
    const vol = row < 64 ? (c.gen * 17) % 1000 : 0;
    return u01(mix(salt, 0x9a2)) < 0.07 ? -(price + vol) : price + vol;
  });

  // PoV key: first 8 bytes = asset name (ASCII), remaining 24 bytes = prefix of the issuer id.
  const povValue: LeafGen = {
    write: (out, o, _c, _salt, p) => {
      writeAscii(out, o, ASSET_NAMES[p % ASSET_NAMES.length] as string, 7);
      const issuer = Math.floor(p / ASSET_NAMES.length) % 800;
      const tag = k.tag('QX', 'issuer');
      for (let q = 0; q < 3; q++) write64(out, o + 8 + q * 8, mix(tag, issuer * 4 + q), mix(tag ^ 0xff, issuer * 4 + q));
    },
  };

  const order = t.struct(
    'QX::AssetOrder',
    [
      F('entity', t.id, gen(G.poolId(entityPool, skewedPick(entityPool.size, 2)))),
      F('numberOfShares', t.u64, gen(shares)),
    ],
    { source: { file: 'src/contracts/Qx.h', line: 61 } },
  );
  const orders = collection(t, order, QX_CAPACITY, {
    population: (c) => Math.round(POP_BASE * (1 + 0.03 * (c.epoch - 229))),
    povCount: () => POV_COUNT,
    priority,
    povValue,
  });

  const root = t.struct(
    'QX::StateData',
    [
      F('_earnedAmount', t.u64, gen(G.growing(8, 4_812_345_678, 250_000))),
      F('_distributedAmount', t.u64, gen(G.constant(8, 3_100_000_000))),
      F('_burnedAmount', t.u64, gen(G.u64Const(0xeb1f0ad2, 0xab54a98c))), // 12,345,678,901,234,567,890
      F('_assetIssuanceFee', t.u32, gen(G.constant(4, 1_000_000_000))),
      F('_transferFee', t.u32, gen(G.constant(4, 1000))),
      F('_tradeFee', t.u32, gen(G.constant(4, 5_000_000))),
      F('_feeSplitBp', t.u16, gen(G.constant(2, 50))),
      F('_paused', t.bool, gen(G.constant(1, 0))),
      F('_assetOrders', orders),
    ],
    { source: { file: 'src/contracts/Qx.h', line: 88 } },
  );
  return {
    index: 1,
    name: 'QX',
    structName: 'QX',
    stateTypeName: 'QX::StateData',
    headerFile: 'src/contracts/Qx.h',
    constructionEpoch: 66,
    destructionEpoch: 65535,
    root,
    salt: k.tag('QX', 'root'),
  };
}
