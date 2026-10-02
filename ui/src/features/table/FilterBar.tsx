import { FilterIcon, PlusIcon, XIcon } from "lucide-react";
import { useMemo, useState } from "react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Popover, PopoverContent, PopoverTrigger } from "@/components/ui/popover";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui/select";
import { describeFilter, FILTER_OP_LABEL, makeFilter, opNeedsValue, opsForKind, validateFilterValue } from "@/lib/filters";
import type { FilterOp, FilterSpec, TableColumn } from "@/rpc/contract";

const PLACEHOLDER: Partial<Record<TableColumn["kind"], string>> = {
  int: "123 or 0x7b",
  u128: "123 or 0x7b",
  id: "60-letter identity or 64 hex chars",
  float: "1.5",
  bool: "true / false",
  bytes: "hex bytes or text",
  datetime: "2024-01-31 12:00:00",
};

function AddFilter({ columns, onAdd }: { columns: TableColumn[]; onAdd: (f: FilterSpec) => void }) {
  const filterable = useMemo(() => columns.filter((c) => c.filterable), [columns]);
  const [open, setOpen] = useState(false);
  const [colId, setColId] = useState<string>(filterable[0]?.id ?? "");
  const col = filterable.find((c) => c.id === colId) ?? filterable[0];
  const ops = col ? opsForKind(col.kind) : [];
  const [op, setOp] = useState<FilterOp>("eq");
  const effectiveOp = ops.includes(op) ? op : (ops[0] ?? "eq");
  const [value, setValue] = useState("");
  const [touched, setTouched] = useState(false);
  const error = col ? validateFilterValue(col.kind, effectiveOp, value) : "No filterable column";

  const submit = () => {
    if (!col) return;
    setTouched(true);
    if (error) return;
    onAdd(makeFilter(col, effectiveOp, value));
    setValue("");
    setTouched(false);
    setOpen(false);
  };

  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger
        render={
          <Button variant="outline" size="xs" disabled={!filterable.length} aria-label="Add filter">
            <PlusIcon /> Filter
          </Button>
        }
      />
      <PopoverContent align="start" className="w-80">
        <div className="space-y-2" onKeyDown={(e) => e.key === "Enter" && submit()}>
          <div className="text-[0.8rem] font-medium text-muted-foreground uppercase">Add filter</div>
          <Select
            value={col?.id ?? ""}
            items={filterable.map((c) => ({ value: c.id, label: c.label }))}
            onValueChange={(v) => v && setColId(v)}
          >
            <SelectTrigger size="sm" className="w-full" aria-label="Filter column">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {filterable.map((c) => (
                <SelectItem key={c.id} value={c.id}>
                  {c.label} <span className="text-muted-foreground">{c.kind}</span>
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Select
            value={effectiveOp}
            items={ops.map((o) => ({ value: o, label: FILTER_OP_LABEL[o] }))}
            onValueChange={(v) => v && setOp(v as FilterOp)}
          >
            <SelectTrigger size="sm" className="w-full" aria-label="Filter operator">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {ops.map((o) => (
                <SelectItem key={o} value={o}>
                  {FILTER_OP_LABEL[o]}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          {opNeedsValue(effectiveOp) && (
            <div>
              <Input
                autoFocus
                value={value}
                onChange={(e) => setValue(e.target.value)}
                placeholder={col ? (PLACEHOLDER[col.kind] ?? "value") : ""}
                aria-label="Filter value"
                aria-invalid={touched && !!error}
                className="h-7 font-mono"
              />
              {touched && error && <p className="mt-1 text-[0.8rem] text-destructive">{error}</p>}
            </div>
          )}
          <div className="flex justify-end gap-1.5 pt-1">
            <Button variant="ghost" size="xs" onClick={() => setOpen(false)}>
              Cancel
            </Button>
            <Button size="xs" onClick={submit}>
              Apply
            </Button>
          </div>
        </div>
      </PopoverContent>
    </Popover>
  );
}

interface Props {
  columns: TableColumn[];
  filters: FilterSpec[];
  onChange: (filters: FilterSpec[]) => void;
}

export function FilterBar({ columns, filters, onChange }: Props) {
  return (
    <div className="flex min-w-0 flex-wrap items-center gap-1.5" role="group" aria-label="Filters">
      <FilterIcon className="size-3.5 text-muted-foreground" aria-hidden />
      {filters.map((f, i) => (
        <span
          key={`${f.column}:${f.op}:${f.value ?? ""}:${i}`}
          data-testid="filter-chip"
          className="inline-flex max-w-[28rem] items-center gap-1 rounded-md bg-primary/12 py-0.5 pr-0.5 pl-2 font-mono text-[0.85rem] text-primary"
        >
          <span className="truncate">{describeFilter(f, columns)}</span>
          <button
            type="button"
            aria-label={`Remove filter ${describeFilter(f, columns)}`}
            className="rounded p-0.5 hover:bg-primary/20"
            onClick={() => onChange(filters.filter((_, j) => j !== i))}
          >
            <XIcon className="size-3" />
          </button>
        </span>
      ))}
      <AddFilter columns={columns} onAdd={(f) => onChange([...filters, f])} />
      {filters.length > 0 && (
        <Button variant="ghost" size="xs" onClick={() => onChange([])}>
          Clear
        </Button>
      )}
    </div>
  );
}
