// Colour maths for the design-token contrast checks: oklch -> sRGB, WCAG relative luminance and contrast ratio,
// and the same `color-mix(in oklab, a p%, b)` the CSS uses for tints, so a tested colour is the colour on screen.

export interface Oklab {
  L: number;
  a: number;
  b: number;
}

export interface Rgb {
  r: number;
  g: number;
  b: number;
}

/** Parse `oklch(L C H)` (L as 0..1 or percent; alpha is not supported: tested colours must be opaque). */
export function parseOklch(css: string): Oklab {
  const m = /^oklch\(\s*([\d.]+)(%?)\s+([\d.]+)\s+([\d.]+)(?:deg)?\s*\)$/.exec(css.trim());
  if (!m) throw new Error(`not an opaque oklch() colour: ${css}`);
  const L = m[2] ? Number(m[1]) / 100 : Number(m[1]);
  const C = Number(m[3]);
  const h = (Number(m[4]) * Math.PI) / 180;
  return { L, a: C * Math.cos(h), b: C * Math.sin(h) };
}

/** `color-mix(in oklab, x p%, y)`: linear interpolation of Lab coordinates. */
export function mixOklab(x: Oklab, pct: number, y: Oklab): Oklab {
  const t = pct / 100;
  return { L: x.L * t + y.L * (1 - t), a: x.a * t + y.a * (1 - t), b: x.b * t + y.b * (1 - t) };
}

const lin2srgb = (c: number) => (c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055);

/** Linear sRGB of an oklab colour, clipped to the gamut the way browsers do for display. */
export function oklabToLinearRgb({ L, a, b }: Oklab): Rgb {
  const l_ = L + 0.3963377774 * a + 0.2158037573 * b;
  const m_ = L - 0.1055613458 * a - 0.0638541728 * b;
  const s_ = L - 0.0894841775 * a - 1.291485548 * b;
  const l = l_ ** 3;
  const m = m_ ** 3;
  const s = s_ ** 3;
  const clip = (v: number) => Math.min(1, Math.max(0, v));
  return {
    r: clip(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
    g: clip(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
    b: clip(-0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s),
  };
}

export function toSrgb8(c: Oklab): [number, number, number] {
  const { r, g, b } = oklabToLinearRgb(c);
  return [Math.round(lin2srgb(r) * 255), Math.round(lin2srgb(g) * 255), Math.round(lin2srgb(b) * 255)];
}

/** WCAG 2.x relative luminance of the (gamut clipped) colour. */
export function luminance(c: Oklab): number {
  const [r, g, b] = toSrgb8(c).map((v) => {
    const s = v / 255;
    return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrastRatio(a: Oklab, b: Oklab): number {
  const la = luminance(a);
  const lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

export function toHex(c: Oklab): string {
  return `#${toSrgb8(c)
    .map((v) => v.toString(16).padStart(2, "0"))
    .join("")}`;
}
