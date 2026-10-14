/** Safety ceiling: more overlapping sounds than this are skipped (the ping still shows). */
export const MAX_VOICES = 8;

/** How long the context stays running after the last sound: a running one renders silence about 100 times a second. */
export const IDLE_SUSPEND_MS = 5000;

/** The parts of AudioContext the bank uses, so tests can pass a fake. */
export type SoundContext = Pick<
  AudioContext, 'state' | 'resume' | 'suspend' | 'decodeAudioData' | 'createBufferSource' | 'createGain' | 'destination'
>;

/** Pre-decoded Web Audio buffers: HTMLAudioElement per play added audible latency in the prototype. */
export class SoundBank {
  private readonly buffers = new Map<string, AudioBuffer>();
  private voices = 0;
  private idleTimer: ReturnType<typeof setTimeout> | null = null;

  /** `idleSuspendMs`: suspend the context after that long without a sound (null: keep it running). */
  constructor(
    private readonly idleSuspendMs: number | null = null,
    private readonly ctx: SoundContext = new AudioContext({ latencyHint: 'interactive' }),
  ) {}

  /** Decodes every sound up front, each under its key. Resolves with the URLs that failed to load. */
  async load(entries: readonly { key: string; url: string }[]): Promise<string[]> {
    const results = await Promise.all(
      entries.map(async ({ key, url }) => {
        try {
          const res = await fetch(url);
          if (!res.ok) throw new Error(String(res.status));
          this.buffers.set(key, await this.ctx.decodeAudioData(await res.arrayBuffer()));
          return null;
        } catch {
          return url;
        }
      }),
    );
    this.suspendWhenIdle();
    return results.filter((r): r is string => r !== null);
  }

  play(key: string, volume01: number): void {
    const buffer = this.buffers.get(key);
    if (!buffer || volume01 <= 0 || this.voices >= MAX_VOICES) return;
    if (this.idleTimer) clearTimeout(this.idleTimer);
    this.idleTimer = null;
    if (this.ctx.state !== 'running') void this.ctx.resume(); // about 10 ms; start() below plays once it has
    const src = this.ctx.createBufferSource();
    const gain = this.ctx.createGain();
    src.buffer = buffer;
    gain.gain.value = volume01;
    src.connect(gain).connect(this.ctx.destination);
    this.voices++;
    src.onended = () => {
      this.voices--;
      if (this.voices === 0) this.suspendWhenIdle();
    };
    src.start();
  }

  private suspendWhenIdle(): void {
    if (this.idleSuspendMs === null) return;
    if (this.idleTimer) clearTimeout(this.idleTimer);
    this.idleTimer = setTimeout(() => {
      this.idleTimer = null;
      if (this.voices === 0) void this.ctx.suspend();
    }, this.idleSuspendMs);
  }
}
