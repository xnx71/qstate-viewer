import type { RpcEventName, RpcEvents } from "./contract";

type AnyHandler = (payload: unknown) => void;

/** Tiny typed event hub shared by all transports. */
export class EventHub {
  private handlers = new Map<string, Set<AnyHandler>>();

  on<E extends RpcEventName>(event: E, handler: (payload: RpcEvents[E]) => void): () => void {
    let set = this.handlers.get(event);
    if (!set) this.handlers.set(event, (set = new Set()));
    const h = handler as AnyHandler;
    set.add(h);
    return () => {
      set.delete(h);
    };
  }

  emit(event: string, payload: unknown): void {
    const set = this.handlers.get(event);
    if (!set) return;
    for (const h of [...set]) {
      try {
        h(payload);
      } catch (e) {
        console.error(`event handler for ${event} failed`, e);
      }
    }
  }
}
