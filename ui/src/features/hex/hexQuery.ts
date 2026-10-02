import { invoke } from "@/rpc/client";
import { useQuery, type QueryResult } from "@/store/query";
import { BYTES_BLOCK, bytesQ, type BytesBlock } from "@/store/data";

/** One 2 KiB block of the state file (cached per generation). */
export function useBytesBlock(contract: number, block: number, version: string, enabled: boolean): QueryResult<BytesBlock> {
  return useQuery(bytesQ, enabled ? `b|${contract}|${block}` : null, version, async () => {
    const res = await invoke("state.bytes", { contract, offset: block * BYTES_BLOCK, length: BYTES_BLOCK });
    return { offset: res.offset, hex: res.hex, fileSize: res.fileSize };
  });
}
