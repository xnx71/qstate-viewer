// Textual paths of tree nodes.

/** ["state", "_assetOrders", "[3]", "entity"] -> "state._assetOrders[3].entity" (array indices attach without a dot). */
export function formatPath(labels: readonly string[]): string {
  let out = "";
  for (const raw of labels) {
    const l = raw || "";
    if (!l) continue;
    out += out === "" ? l : l.startsWith("[") ? l : `.${l}`;
  }
  return out;
}
