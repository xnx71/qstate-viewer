// Summarise a V8 heap snapshot (.heapsnapshot from `memory.mjs --snapshots=...` or DevTools): top constructors by shallow
// size and by RETAINED size (dominator tree), plus the biggest single retainers.
//
//   node scripts/heap-top.mjs <file.heapsnapshot> [--top=25]
import { readFileSync } from "node:fs";

const file = process.argv[2];
const top = Number((process.argv.find((a) => a.startsWith("--top=")) || "--top=25").slice(6));
if (!file) {
  console.error("usage: node scripts/heap-top.mjs <file.heapsnapshot> [--top=25]");
  process.exit(2);
}
const snap = JSON.parse(readFileSync(file, "utf8"));
const { node_fields: NF, node_types: NT, edge_fields: EF, edge_types: ET } = snap.snapshot.meta;
const nodes = snap.nodes;
const edges = snap.edges;
const strings = snap.strings;
const NS = NF.length;
const ES = EF.length;
const iType = NF.indexOf("type");
const iName = NF.indexOf("name");
const iSelf = NF.indexOf("self_size");
const iEdgeCount = NF.indexOf("edge_count");
const eType = EF.indexOf("type");
const eTo = EF.indexOf("to_node");
const nodeTypeNames = NT[0];
const edgeTypeNames = ET[0];
const N = nodes.length / NS;
const WEAK = edgeTypeNames.indexOf("weak");
const SHORTCUT = edgeTypeNames.indexOf("shortcut");

// first edge index per node
const firstEdge = new Uint32Array(N + 1);
for (let i = 0, e = 0; i < N; i++) {
  firstEdge[i] = e;
  e += nodes[i * NS + iEdgeCount];
}
firstEdge[N] = edges.length / ES;

// iterative DFS: postorder over essential edges
const post = new Uint32Array(N);
const order = new Int32Array(N).fill(-1);
let postCount = 0;
{
  const stack = new Uint32Array(N);
  const cursor = new Uint32Array(N);
  const seen = new Uint8Array(N);
  let sp = 0;
  stack[sp++] = 0;
  seen[0] = 1;
  cursor[0] = firstEdge[0];
  while (sp > 0) {
    const v = stack[sp - 1];
    if (cursor[v] < firstEdge[v + 1]) {
      const e = cursor[v]++;
      const t = edges[e * ES + eType];
      if (t === WEAK || t === SHORTCUT) continue;
      const w = edges[e * ES + eTo] / NS;
      if (!seen[w]) {
        seen[w] = 1;
        cursor[w] = firstEdge[w];
        stack[sp++] = w;
      }
    } else {
      order[v] = postCount;
      post[postCount++] = v;
      sp--;
    }
  }
}
// predecessors (essential edges, reachable nodes only)
const predCount = new Uint32Array(N + 1);
for (let v = 0; v < N; v++) {
  if (order[v] < 0) continue;
  for (let e = firstEdge[v]; e < firstEdge[v + 1]; e++) {
    const t = edges[e * ES + eType];
    if (t === WEAK || t === SHORTCUT) continue;
    predCount[edges[e * ES + eTo] / NS + 1]++;
  }
}
for (let i = 0; i < N; i++) predCount[i + 1] += predCount[i];
const preds = new Uint32Array(predCount[N]);
const fill = new Uint32Array(N);
for (let v = 0; v < N; v++) {
  if (order[v] < 0) continue;
  for (let e = firstEdge[v]; e < firstEdge[v + 1]; e++) {
    const t = edges[e * ES + eType];
    if (t === WEAK || t === SHORTCUT) continue;
    const w = edges[e * ES + eTo] / NS;
    preds[predCount[w] + fill[w]++] = v;
  }
}
// Cooper-Harvey-Kennedy dominators on postorder numbers
const idom = new Int32Array(N).fill(-1);
idom[0] = 0;
const intersect = (a, b) => {
  while (a !== b) {
    while (order[a] < order[b]) a = idom[a];
    while (order[b] < order[a]) b = idom[b];
  }
  return a;
};
for (let changed = true; changed; ) {
  changed = false;
  for (let i = postCount - 2; i >= 0; i--) {
    const v = post[i];
    let nd = -1;
    for (let p = predCount[v]; p < predCount[v + 1]; p++) {
      const u = preds[p];
      if (idom[u] < 0) continue;
      nd = nd < 0 ? u : intersect(u, nd);
    }
    if (nd >= 0 && idom[v] !== nd) {
      idom[v] = nd;
      changed = true;
    }
  }
}
const retained = new Float64Array(N);
for (let i = 0; i < postCount; i++) retained[post[i]] = nodes[post[i] * NS + iSelf];
for (let i = 0; i < postCount - 1; i++) {
  const v = post[i];
  if (idom[v] >= 0) retained[idom[v]] += retained[v];
}

const groupOf = (v) => {
  const t = nodeTypeNames[nodes[v * NS + iType]];
  const name = strings[nodes[v * NS + iName]];
  if (t === "object") return name;
  if (t === "string" || t === "concatenated string" || t === "sliced string") return "(string)";
  if (t === "closure") return `(closure) ${name.slice(0, 40)}`;
  if (t === "array") return `(array) ${name}`;
  if (t === "hidden") return "(system)";
  return `(${t}) ${name.slice(0, 40)}`;
};
const groups = new Map();
let total = 0;
for (let v = 0; v < N; v++) {
  if (order[v] < 0) continue;
  const g = groupOf(v);
  const self = nodes[v * NS + iSelf];
  total += self;
  let r = groups.get(g);
  if (!r) groups.set(g, (r = { name: g, count: 0, self: 0, retained: 0 }));
  r.count++;
  r.self += self;
  // retained size is attributed once per dominator chain of the same group
  const d = idom[v];
  if (v === 0 || d < 0 || groupOf(d) !== g) r.retained += retained[v];
}
const mb = (b) => (b / 1048576).toFixed(2).padStart(8);
console.log(`${file}\nreachable heap ${(total / 1048576).toFixed(1)} MB, ${postCount} nodes`);
const rows = [...groups.values()];
console.log(`\nTop ${top} by shallow size:\n  ${"constructor".padEnd(46)} ${"count".padStart(9)}  shallow MB  retained MB`);
for (const r of rows.sort((a, b) => b.self - a.self).slice(0, top)) console.log(`  ${r.name.slice(0, 46).padEnd(46)} ${String(r.count).padStart(9)} ${mb(r.self)}   ${mb(r.retained)}`);
console.log(`\nTop ${top} by retained size:\n  ${"constructor".padEnd(46)} ${"count".padStart(9)}  shallow MB  retained MB`);
for (const r of rows.sort((a, b) => b.retained - a.retained).slice(0, top)) console.log(`  ${r.name.slice(0, 46).padEnd(46)} ${String(r.count).padStart(9)} ${mb(r.self)}   ${mb(r.retained)}`);

// biggest individual retainers (excluding the root and plain system nodes)
const ids = [];
for (let v = 1; v < N; v++) if (order[v] >= 0) ids.push(v);
ids.sort((a, b) => retained[b] - retained[a]);
console.log(`\nBiggest single objects by retained size:`);
let shown = 0;
for (const v of ids) {
  const t = nodeTypeNames[nodes[v * NS + iType]];
  if (t === "synthetic" || t === "hidden") continue;
  console.log(`  ${mb(retained[v])} MB  ${t.padEnd(9)} ${strings[nodes[v * NS + iName]].slice(0, 70)}`);
  if (++shown >= 15) break;
}

// duplicated strings: identical contents stored many times (candidates for interning / compact storage)
{
  const byValue = new Map();
  let strBytes = 0;
  let wasted = 0;
  for (let v = 1; v < N; v++) {
    if (order[v] < 0) continue;
    const t = nodeTypeNames[nodes[v * NS + iType]];
    if (t !== "string") continue;
    const val = strings[nodes[v * NS + iName]];
    const self = nodes[v * NS + iSelf];
    strBytes += self;
    const r = byValue.get(val);
    if (r) {
      r.count++;
      r.bytes += self;
    } else byValue.set(val, { count: 1, bytes: self });
  }
  const list = [...byValue.entries()].map(([val, r]) => ({ val, ...r, waste: r.bytes - r.bytes / r.count }));
  for (const r of list) wasted += r.waste;
  console.log(`\nStrings: ${(strBytes / 1048576).toFixed(1)} MB in total, ${(wasted / 1048576).toFixed(1)} MB of it duplicates of an identical string. Top duplicated strings:`);
  for (const r of list.sort((a, b) => b.waste - a.waste).slice(0, 12)) console.log(`  ${(r.waste / 1048576).toFixed(2).padStart(7)} MB wasted  x${String(r.count).padStart(7)}  ${JSON.stringify(r.val.slice(0, 60))}`);
}
