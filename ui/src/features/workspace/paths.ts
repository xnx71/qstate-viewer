// Path helpers for the folder browser (POSIX and Windows separators).

export type Sep = "/" | "\\";

/** contractNNNN.EEE: a contract state file. */
export const STATE_FILE_RE = /^contract(\d{4})\.(\d+)$/;

export function parseStateFileName(name: string): { index: number; epoch: number } | null {
  const m = STATE_FILE_RE.exec(name);
  return m ? { index: Number(m[1]), epoch: Number(m[2]) } : null;
}

interface Segment {
  label: string;
  path: string;
}

/** Split an absolute path into clickable breadcrumb segments, root first. */
export function pathSegments(path: string, sep: Sep): Segment[] {
  if (!path) return [];
  const parts = path.split(sep === "\\" ? /[\\/]+/ : /\/+/).filter(Boolean);
  const segs: Segment[] = sep === "/" ? [{ label: "/", path: "/" }] : [];
  let acc = "";
  parts.forEach((p, i) => {
    acc = sep === "/" ? `${acc}/${p}` : i === 0 ? `${p}\\` : `${acc.replace(/\\$/, "")}\\${p}`;
    segs.push({ label: p, path: acc });
  });
  return segs;
}

export function parentPath(path: string, sep: Sep): string | null {
  const segs = pathSegments(path, sep);
  return segs.length > 1 ? (segs[segs.length - 2] as Segment).path : null;
}

export function baseName(path: string): string {
  return path.split(/[\\/]+/).filter(Boolean).pop() ?? path;
}

/**
 * Interpret text typed or pasted into the path field: surrounding quotes and a file:// prefix are dropped; a path that
 * names a state file is split into its folder and the file.
 */
export function parseTypedPath(text: string, sep: Sep): { dir: string; file?: string } {
  let p = text.trim().replace(/^(["'])(.*)\1$/, "$2").replace(/^file:\/\//, "");
  if (p.length > 1) p = p.replace(/[\\/]+$/, "");
  if (parseStateFileName(baseName(p))) {
    const dir = parentPath(p, sep);
    if (dir !== null) return { dir, file: p };
  }
  return { dir: p };
}

/** Shorten a path in the middle: "/home/…/qubic/state". */
export function shortenPath(path: string, max = 44): string {
  if (path.length <= max) return path;
  const keep = max - 1;
  const head = Math.ceil(keep * 0.35);
  return `${path.slice(0, head)}…${path.slice(path.length - (keep - head))}`;
}
