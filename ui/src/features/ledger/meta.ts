// Visual vocabulary of the ledgers: glyph + wording per ledger kind, and the chips of the three universe record types.
import { CoinsIcon, GemIcon, type LucideIcon } from "lucide-react";
import type { LedgerKind } from "@/rpc/contract";

export interface LedgerMeta {
  kind: LedgerKind;
  label: string;
  Icon: LucideIcon;
  /** One line under the name in the sidebar / empty states. */
  blurb: string;
  /** What one live record is. */
  record: string;
  records: string;
}

export const LEDGER_META: Record<LedgerKind, LedgerMeta> = {
  spectrum: { kind: "spectrum", label: "Spectrum", Icon: CoinsIcon, blurb: "entities and balances", record: "entity", records: "entities" },
  universe: { kind: "universe", label: "Universe", Icon: GemIcon, blurb: "assets: issuances, ownerships, possessions", record: "record", records: "records" },
};

export const LEDGER_ORDER: readonly LedgerKind[] = ["spectrum", "universe"];

/** Universe record types (AssetRecord.type): 1 issuance, 2 ownership, 3 possession. Colours from the type palette. */
export interface AssetTypeMeta {
  type: 1 | 2 | 3;
  label: string;
  plural: string;
  /** Text colour class (full literal so Tailwind sees it); chips tint from it. */
  text: string;
  view: "issuances" | "ownerships" | "possessions";
}

export const ASSET_TYPES: Record<1 | 2 | 3, AssetTypeMeta> = {
  1: { type: 1, label: "issuance", plural: "issuances", text: "text-t-asset", view: "issuances" },
  2: { type: 2, label: "ownership", plural: "ownerships", text: "text-t-int", view: "ownerships" },
  3: { type: 3, label: "possession", plural: "possessions", text: "text-t-enum", view: "possessions" },
};

export function assetTypeOf(v: unknown): AssetTypeMeta | undefined {
  const n = typeof v === "number" ? v : typeof v === "string" ? Number(v) : NaN;
  return n === 1 || n === 2 || n === 3 ? ASSET_TYPES[n] : undefined;
}

/** Type of a record from a type chip name ("ISSUANCE") or number. */
export function assetTypeFromName(name: string | undefined): AssetTypeMeta | undefined {
  switch ((name ?? "").toLowerCase()) {
    case "issuance":
      return ASSET_TYPES[1];
    case "ownership":
      return ASSET_TYPES[2];
    case "possession":
      return ASSET_TYPES[3];
    default:
      return undefined;
  }
}
