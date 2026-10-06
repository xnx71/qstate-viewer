import { useAtom, useAtomValue } from "jotai";
import { CornerDownRightIcon } from "lucide-react";
import { memo, useEffect, useMemo, useRef, useState } from "react";
import { toast } from "sonner";
import { RpcErrorView } from "@/components/common/RpcErrorView";
import { Button } from "@/components/ui/button";
import { asciiOf, fmtCount, fmtHexOffset, hexToBytes, parseOffset } from "@/lib/format";
import { rowPx } from "@/lib/sizes";
import { byteOfHexChar, HEX_CHARS, hexHeader } from "@/lib/hexLayout";
import { useRowSlots } from "@/lib/useRowSlots";
import { useScaledVirtualizer } from "@/lib/useScaledVirtualizer";
import { uiSizeAtom } from "@/store/prefs";
import { cn } from "@/lib/utils";
import { describeError } from "@/rpc/errors";
import { BYTES_BLOCK, bytesQ, useContractVersion, type BytesBlock } from "@/store/data";
import { hexMenu } from "@/features/contextmenu/builders/hex";
import { useContextMenu } from "@/features/contextmenu/useContextMenu";
import { bytesBase, fetchBytesBlock, readRange, useBytesBlock as query } from "./hexQuery";
import { usePageLoader, type LoaderPage } from "@/lib/usePageLoader";
import { hexJumpAtom, selectedByteAtom } from "@/store/hex";
import { gotoOffset } from "@/store/search";

const COLS = 16;
const BLOCK_ROWS = BYTES_BLOCK / COLS;

// A row is TWO text runs, not 32 one-character elements: the hex column and the ASCII column are strings, split into a
// <span> only where the look changes (zero byte / other byte / selection). 35 elements per row and 40 rows were rebuilt on
// every jump of the scrollbar; the DOM garbage that produced (Blink's heap collects lazily) made the hex view the phase with
// the biggest renderer peak (docs/MEMORY.md). Monospace font: the hover maps x to a byte with plain arithmetic.
const HEX2: string[] = Array.from({ length: 256 }, (_, n) => n.toString(16).padStart(2, "0"));
const MISSING = 0;
const ZERO = 1;
const OTHER = 2;
const SELECTED = 3;
const EDGE = 4;
const HEX_CLASS = ["text-fg-subtle/40", "text-fg-subtle", "text-fg", "bg-sel text-fg shadow-[inset_0_-2px_0_var(--sel-edge)]", "rounded-l-sm bg-sel-edge text-brand-fg"];
const ASCII_CLASS = ["", "text-fg-subtle", "text-t-bytes", "bg-sel text-fg", "bg-sel text-fg"];

function kindOf(b: number | undefined, inSel: boolean, edge: boolean): number {
  return edge ? EDGE : inSel ? SELECTED : b === undefined ? MISSING : b === 0 ? ZERO : OTHER;
}

/** Consecutive bytes of the same look share one span. */
function runs(kinds: number[], texts: string[], classes: string[]): React.ReactNode[] {
  const out: React.ReactNode[] = [];
  let i = 0;
  while (i < kinds.length) {
    let j = i + 1;
    let text = texts[i];
    while (j < kinds.length && kinds[j] === kinds[i] && kinds[i] !== EDGE) text += texts[j++];
    out.push(
      <span key={out.length} className={classes[kinds[i]] || undefined}>
        {text}
      </span>,
    );
    i = j;
  }
  return out;
}

const bytesCache = new WeakMap<BytesBlock, Uint8Array>();
function blockBytes(b: BytesBlock): Uint8Array {
  let u = bytesCache.get(b);
  if (!u) bytesCache.set(b, (u = hexToBytes(b.hex)));
  return u;
}

interface RowProps {
  contract: number;
  row: number;
  range: { start: number; end: number } | null;
  onHover: (offset: number | null) => void;
}

const HexRow = memo(function HexRow({ contract, row, range, onHover }: RowProps) {
  const block = Math.floor(row / BLOCK_ROWS);
  const version = useContractVersion(contract);
  const q = query(contract, block, version, false);
  const bytes = q.data ? blockBytes(q.data) : undefined;
  const base = row * COLS;
  const local = base - block * BYTES_BLOCK;
  const kinds: number[] = [];
  const hexTexts: string[] = [];
  const asciiKinds: number[] = [];
  const asciiTexts: string[] = [];
  const placeholder = q.loading || q.data === undefined ? "··" : "  ";
  for (let i = 0; i < COLS; i++) {
    const off = base + i;
    const b = bytes && local + i < bytes.length ? bytes[local + i] : undefined;
    const inSel = range !== null && off >= range.start && off < range.end;
    kinds.push(kindOf(b, inSel, inSel && off === range.start));
    hexTexts.push((b === undefined ? placeholder : HEX2[b]) + (i === 7 ? "  " : i < COLS - 1 ? " " : ""));
    asciiKinds.push(inSel ? SELECTED : b === undefined ? MISSING : b === 0 ? ZERO : OTHER);
    asciiTexts.push(b === undefined ? " " : asciiOf(b));
  }
  return (
    <div
      className="flex h-full items-center gap-4 px-3 font-mono text-hex whitespace-pre hover:bg-hover"
      onMouseMove={(e) => {
        const el = e.target as HTMLElement;
        const col = el.closest<HTMLElement>("[data-col]");
        if (!col) return onHover(null);
        const rect = col.getBoundingClientRect();
        const isHex = col.dataset["col"] === "hex";
        const ch = rect.width / (isHex ? HEX_CHARS : COLS);
        const c = Math.floor((e.clientX - rect.left) / ch);
        onHover(base + (isHex ? byteOfHexChar(c) : Math.max(0, Math.min(COLS - 1, c))));
      }}
      onMouseLeave={() => onHover(null)}
    >
      <span className="w-[8ch] shrink-0 text-fg-muted">{base.toString(16).padStart(8, "0")}</span>
      <span className="shrink-0" data-col="hex">
        {runs(kinds, hexTexts, HEX_CLASS)}
      </span>
      <span className="shrink-0" data-col="ascii">
        {runs(asciiKinds, asciiTexts, ASCII_CLASS)}
      </span>
    </div>
  );
});

/** Interpretation of the bytes at `offset` (little endian), shown while hovering. */
function interpret(contract: number, version: string, offset: number): string {
  const get = (o: number): number | undefined => {
    const block = Math.floor(o / BYTES_BLOCK);
    const b = bytesQ.peek(`b|${contract}|${block}`, version);
    if (!b) return undefined;
    const u = blockBytes(b);
    const i = o - block * BYTES_BLOCK;
    return i < u.length ? u[i] : undefined;
  };
  const bytes: number[] = [];
  for (let i = 0; i < 8; i++) {
    const v = get(offset + i);
    if (v === undefined) break;
    bytes.push(v);
  }
  if (!bytes.length) return "";
  const le = (n: number) => {
    if (bytes.length < n) return undefined;
    let v = 0n;
    for (let i = n - 1; i >= 0; i--) v = (v << 8n) | BigInt(bytes[i]);
    return v;
  };
  const parts = [`u8 ${bytes[0]}`, `i8 ${bytes[0] > 127 ? bytes[0] - 256 : bytes[0]}`];
  const u16 = le(2);
  if (u16 !== undefined) parts.push(`u16 ${u16}`);
  const u32 = le(4);
  if (u32 !== undefined) parts.push(`u32 ${u32}`);
  const u64 = le(8);
  if (u64 !== undefined) parts.push(`u64 ${u64}`);
  return parts.join(" · ");
}

interface Props {
  contract: number;
  /** Highlighted byte range (node extent or search match). */
  range: { offset: number; length: number } | null;
  className?: string;
}

/** Virtualized hex dump of the whole state file with range highlighting. */
export function HexView({ contract, range, className }: Props) {
  const version = useContractVersion(contract);
  const first = query(contract, 0, version, true);
  const fileSize = first.data?.fileSize ?? 0;
  const rows = Math.ceil(fileSize / COLS);
  const ROW_H = rowPx("hex", useAtomValue(uiSizeAtom));
  const scrollRef = useRef<HTMLDivElement>(null);
  const sv = useScaledVirtualizer({ count: rows, rowHeight: ROW_H, scrollRef, overscan: 6 });
  const slotOf = useRowSlots(sv.rows.length);
  const [hover, setHover] = useState<number | null>(null);
  const [jump, setJump] = useAtom(hexJumpAtom);
  const override = useAtomValue(selectedByteAtom);
  const effective = override ?? range;
  const span = useMemo(
    () => (effective && effective.length > 0 ? { start: effective.offset, end: effective.offset + effective.length } : null),
    [effective],
  );
  const [goto, setGoto] = useState("");
  const ctx = useContextMenu(() => hexMenu({ span, read: (start, length) => readRange(contract, version, start, length) }));

  usePageLoader({
    start: sv.rows[0]?.index ?? 0,
    end: sv.rows.length ? sv.rows[sv.rows.length - 1].index : -1,
    count: rows,
    direction: sv.direction,
    pageRows: BLOCK_ROWS,
    pageOfRow: (row): LoaderPage => {
      const block = Math.floor(row / BLOCK_ROWS);
      const base = bytesBase(contract, block);
      return {
        key: `${base}@${version}`,
        cached: () => bytesQ.has(base, version),
        retain: () => bytesQ.retain(base),
        run: () => fetchBytesBlock(contract, block, version),
      };
    },
    epoch: `${contract}@${version}`,
  });

  // Scroll to the highlighted range when it changes.
  const lastKey = useRef("");
  useEffect(() => {
    if (!span || !rows) return;
    const key = `${contract}:${span.start}`;
    if (key === lastKey.current) return;
    lastKey.current = key;
    requestAnimationFrame(() => sv.scrollToRow(Math.floor(span.start / COLS), "center"));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [span?.start, contract, rows]);

  useEffect(() => {
    if (jump && rows) {
      sv.scrollToRow(Math.floor(jump.offset / COLS), "center");
      setJump(null);
    }
  }, [jump, rows, sv, setJump]);

  if (first.error) return <RpcErrorView error={first.error} onRetry={first.retry} compact />;

  const submitGoto = async () => {
    const off = parseOffset(goto);
    if (off === null) return toast.error("Enter a byte offset like 1234 or 0x4d2");
    if (fileSize && off >= fileSize) return toast.error(`Offset is beyond the end of the file (${fmtCount(fileSize)} bytes)`);
    try {
      await gotoOffset(contract, off);
      sv.scrollToRow(Math.floor(off / COLS), "center");
    } catch (e) {
      toast.error(describeError(e));
    }
  };

  return (
    <div className={cn("flex min-h-0 flex-col", className)}>
      <div className="flex items-center gap-2 border-b px-3 py-2">
        <input
          value={goto}
          onChange={(e) => setGoto(e.target.value)}
          onKeyDown={(e) => e.key === "Enter" && void submitGoto()}
          placeholder="Go to offset (123 or 0x7b)"
          aria-label="Go to byte offset"
          className="h-8 min-w-0 flex-1 rounded-lg border border-line-input bg-surface-1 px-2.5 font-mono text-mono outline-none focus-visible:border-ring focus-visible:ring-2 focus-visible:ring-ring/40"
        />
        <Button variant="outline" size="sm" onClick={() => void submitGoto()}>
          <CornerDownRightIcon /> Go
        </Button>
      </div>
      <div className="border-b px-3 py-1.5 font-mono text-hex whitespace-pre text-fg-muted" aria-hidden>
        <span className="mr-4 inline-block w-[8ch]">offset</span>
        <span>{hexHeader()}</span>
      </div>
      <div ref={scrollRef} className="min-h-0 flex-1 overflow-auto bg-surface-1" role="region" aria-label="Hex dump" tabIndex={0} {...ctx}>
        <div style={{ height: sv.scrollHeight, position: "relative", minWidth: "fit-content" }}>
          {rows === 0 && !first.loading && <div className="p-4 text-center text-fg-muted">Empty file</div>}
          {sv.rows.map((r) => (
            <div key={slotOf(r.index)} style={{ position: "absolute", top: 0, left: 0, right: 0, height: ROW_H, transform: `translateY(${r.y}px)` }}>
              <HexRow contract={contract} row={r.index} range={span} onHover={setHover} />
            </div>
          ))}
        </div>
      </div>
      <div className="flex h-8 shrink-0 items-center gap-3 overflow-hidden border-t px-3 font-mono text-meta whitespace-nowrap text-fg-muted tabular">
        {hover !== null ? (
          <>
            <span className="text-fg">{fmtHexOffset(hover)}</span>
            <span>{fmtCount(hover)}</span>
            <span className="truncate">{interpret(contract, version, hover)}</span>
          </>
        ) : span ? (
          <span>
            {fmtHexOffset(span.start)}..{fmtHexOffset(span.end - 1)} · {fmtCount(span.end - span.start)} bytes
          </span>
        ) : (
          <span>{fileSize ? `${fmtCount(fileSize)} bytes` : ""}</span>
        )}
      </div>
    </div>
  );
}
