import { invoke } from "@/rpc/client";
import { useQuery, type QueryResult } from "@/store/query";
import { BYTES_BLOCK, bytesQ, type BytesBlock } from "@/store/data";
import { hexToBytes } from "@/lib/format";

export const bytesBase = (contract: number, block: number) => `b|${contract}|${block}`;

export function fetchBytesBlock(contract: number, block: number, version: string): Promise<BytesBlock> {
  return bytesQ.fetch(bytesBase(contract, block), version, () => loadBytesBlock(contract, block));
}

async function loadBytesBlock(contract: number, block: number): Promise<BytesBlock> {
  const res = await invoke("state.bytes", { contract, offset: block * BYTES_BLOCK, length: BYTES_BLOCK });
  return { offset: res.offset, hex: res.hex, fileSize: res.fileSize };
}

/** One 2 KiB block of the state file (cached per generation). `fetch = false`: read the cache only (see useQuery). */
export function useBytesBlock(contract: number, block: number, version: string, fetch = true): QueryResult<BytesBlock> {
  return useQuery(bytesQ, bytesBase(contract, block), version, () => loadBytesBlock(contract, block), { fetch });
}

/** Bytes [start, start + length) of the state file, from cached blocks (missing blocks are fetched). */
export async function readRange(contract: number, version: string, start: number, length: number): Promise<Uint8Array> {
  const out = new Uint8Array(length);
  let written = 0;
  for (let block = Math.floor(start / BYTES_BLOCK); written < length; block++) {
    const b = await fetchBytesBlock(contract, block, version);
    const bytes = hexToBytes(b.hex);
    const from = Math.max(0, start + written - block * BYTES_BLOCK);
    const slice = bytes.subarray(from, Math.min(bytes.length, from + (length - written)));
    if (slice.length === 0) break;
    out.set(slice, written);
    written += slice.length;
  }
  return written === length ? out : out.subarray(0, written);
}
