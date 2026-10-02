import { Component, type ErrorInfo, type ReactNode } from "react";
import { RpcErrorView } from "@/components/common/RpcErrorView";
import { isRpcErrorLike } from "@/rpc/errors";

interface Props {
  children: ReactNode;
  /** Changing this value resets the boundary (e.g. selected contract). */
  resetKey?: unknown;
  label?: string;
}
interface State {
  error: unknown;
}

/** Catches render errors; RpcErrors get a mapped, readable message. */
export class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null };
  static getDerivedStateFromError(error: unknown): State {
    return { error };
  }
  componentDidCatch(error: unknown, info: ErrorInfo): void {
    console.error("UI error", error, info.componentStack);
  }
  componentDidUpdate(prev: Props): void {
    if (prev.resetKey !== this.props.resetKey && this.state.error) this.setState({ error: null });
  }
  render() {
    if (this.state.error === null) return this.props.children;
    const e = this.state.error;
    return (
      <RpcErrorView
        error={isRpcErrorLike(e) ? e : { code: "internal", message: `${this.props.label ?? "This view"} crashed: ${e instanceof Error ? e.message : String(e)}` }}
        onRetry={() => this.setState({ error: null })}
      />
    );
  }
}
