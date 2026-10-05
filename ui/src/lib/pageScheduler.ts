// Small request scheduler for page loading: bounded concurrency, priority order, and cancellation of queued work that
// the viewport no longer needs. Requests that already started are left to finish: their result lands in the cache and
// makes scrolling back instant.

export interface PageJob {
  key: string;
  run: () => Promise<unknown>;
}

export class PageScheduler {
  private queue: PageJob[] = [];
  private readonly running = new Set<string>();

  private readonly concurrency: number;

  constructor(concurrency = 3) {
    this.concurrency = concurrency;
  }

  /** Replace the wanted jobs (priority order). Queued jobs that are not in the list any more are dropped. */
  want(jobs: readonly PageJob[]): void {
    const seen = new Set<string>();
    this.queue = jobs.filter((j) => {
      if (this.running.has(j.key) || seen.has(j.key)) return false;
      seen.add(j.key);
      return true;
    });
    this.pump();
  }

  /** Drop everything that has not started. */
  cancelQueued(): void {
    this.queue = [];
  }

  get queued(): number {
    return this.queue.length;
  }

  get active(): number {
    return this.running.size;
  }

  private pump(): void {
    while (this.running.size < this.concurrency && this.queue.length > 0) {
      const job = this.queue.shift() as PageJob;
      this.running.add(job.key);
      let p: Promise<unknown>;
      try {
        p = job.run();
      } catch (e) {
        p = Promise.reject(e);
      }
      void p
        .catch(() => undefined)
        .finally(() => {
          this.running.delete(job.key);
          this.pump();
        });
    }
  }
}
