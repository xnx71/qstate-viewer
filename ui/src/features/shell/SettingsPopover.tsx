import { useAtomValue } from "jotai";
import { MonitorIcon, MoonIcon, SlidersHorizontalIcon, SunIcon } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Popover, PopoverContent, PopoverTrigger } from "@/components/ui/popover";
import { Switch } from "@/components/ui/switch";
import { UI_SIZES, UI_SIZE_ORDER, type UiSize } from "@/lib/sizes";
import { cn } from "@/lib/utils";
import type { Theme } from "@/store/prefs";
import { prefsAtom, setTheme, setUiSize, themeAtom, uiSizeAtom, updatePrefs } from "@/store/prefs";

function Segmented<T extends string>({ value, options, onChange, label }: { value: T; options: { value: T; label: React.ReactNode; title?: string }[]; onChange: (v: T) => void; label: string }) {
  return (
    <div role="radiogroup" aria-label={label} className="grid auto-cols-fr grid-flow-col gap-0.5 rounded-lg bg-surface-3 p-0.5">
      {options.map((o) => (
        <button
          key={o.value}
          type="button"
          role="radio"
          aria-checked={o.value === value}
          title={o.title}
          onClick={() => onChange(o.value)}
          className={cn(
            "flex h-8 items-center justify-center gap-1.5 rounded-md px-2 text-data font-medium transition-colors outline-none focus-visible:ring-2 focus-visible:ring-ring",
            o.value === value ? "bg-surface-2 text-fg shadow-soft" : "text-fg-muted hover:text-fg",
          )}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}

const THEMES: { value: Theme; label: React.ReactNode }[] = [
  { value: "dark", label: <><MoonIcon className="size-4" /> Dark</> },
  { value: "light", label: <><SunIcon className="size-4" /> Light</> },
  { value: "system", label: <><MonitorIcon className="size-4" /> Auto</> },
];

/** Small settings popover: UI size (Compact / Comfortable / Large), theme and two display toggles. */
export function SettingsPopover() {
  const size = useAtomValue(uiSizeAtom);
  const theme = useAtomValue(themeAtom);
  const prefs = useAtomValue(prefsAtom);
  return (
    <Popover>
      <PopoverTrigger
        render={
          <Button variant="ghost" size="icon-sm" aria-label="Settings" title="Settings">
            <SlidersHorizontalIcon />
          </Button>
        }
      />
      <PopoverContent align="end" className="w-80 gap-4 p-4" aria-label="Settings">
        <div className="space-y-2">
          <div className="flex items-baseline justify-between">
            <h3 className="text-data font-semibold">UI size</h3>
            <span className="text-meta text-fg-muted">{UI_SIZES[size].hint}</span>
          </div>
          <Segmented<UiSize>
            label="UI size"
            value={size}
            onChange={setUiSize}
            options={UI_SIZE_ORDER.map((s) => ({ value: s, label: UI_SIZES[s].label, title: `${UI_SIZES[s].label} (${Math.round(UI_SIZES[s].scale * 100)}%)` }))}
          />
        </div>
        <div className="space-y-2">
          <h3 className="text-data font-semibold">Theme</h3>
          <Segmented<Theme> label="Theme" value={theme} onChange={setTheme} options={THEMES} />
        </div>
        <div className="space-y-2.5">
          <label className="flex items-center justify-between gap-3 text-data">
            <span>
              Change animations
              <span className="block text-meta text-fg-muted">Flash and count when a value changes live</span>
            </span>
            <Switch checked={prefs.animations} onCheckedChange={(v) => updatePrefs({ animations: v })} aria-label="Change animations" />
          </label>
          <label className="flex items-center justify-between gap-3 text-data">
            <span>
              Byte offsets in the tree
              <span className="block text-meta text-fg-muted">Show the offset column</span>
            </span>
            <Switch checked={prefs.showOffsets} onCheckedChange={(v) => updatePrefs({ showOffsets: v })} aria-label="Byte offsets" />
          </label>
        </div>
      </PopoverContent>
    </Popover>
  );
}
