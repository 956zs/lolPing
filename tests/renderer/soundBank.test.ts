import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { IDLE_SUSPEND_MS, MAX_VOICES, SoundBank, type SoundContext } from '../../src/renderer/overlay/soundBank';

interface FakeSource {
  buffer: unknown;
  onended: (() => void) | null;
  started: boolean;
}

/** Just enough of an AudioContext: state, suspend/resume calls and the sources it hands out. */
function fakeContext() {
  const sources: FakeSource[] = [];
  const calls = { suspend: 0, resume: 0 };
  const node = { connect: (next: unknown) => next };
  const ctx = {
    state: 'running' as AudioContextState,
    destination: {},
    suspend: async () => {
      calls.suspend++;
      ctx.state = 'suspended';
    },
    resume: async () => {
      calls.resume++;
      ctx.state = 'running';
    },
    decodeAudioData: async () => ({}),
    createGain: () => ({ ...node, gain: { value: 1 } }),
    createBufferSource: () => {
      const src: FakeSource & typeof node & { start: () => void } = {
        ...node, buffer: null, onended: null, started: false, start: () => { src.started = true; },
      };
      sources.push(src);
      return src;
    },
  };
  return { ctx: ctx as unknown as SoundContext, state: () => ctx.state, calls, sources };
}

async function loadedBank(idleMs: number | null) {
  const fake = fakeContext();
  const bank = new SoundBank(idleMs, fake.ctx);
  await bank.load([{ key: 'ping', url: 'ping.wav' }]);
  return { bank, ...fake };
}

describe('SoundBank', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal('fetch', async () => ({ ok: true, arrayBuffer: async () => new ArrayBuffer(8) }));
  });
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  it('suspends the context once no sound has played for the idle time', async () => {
    const { calls, state } = await loadedBank(IDLE_SUSPEND_MS);
    vi.advanceTimersByTime(IDLE_SUSPEND_MS - 1);
    expect(calls.suspend).toBe(0);
    vi.advanceTimersByTime(1);
    expect(calls.suspend).toBe(1);
    expect(state()).toBe('suspended');
  });

  it('never suspends when idle suspension is off', async () => {
    const { bank, calls, sources } = await loadedBank(null);
    bank.play('ping', 1);
    sources[0].onended?.();
    vi.advanceTimersByTime(IDLE_SUSPEND_MS * 10);
    expect(calls.suspend).toBe(0);
  });

  it('resumes a suspended context to play, then suspends again after the sound ends', async () => {
    const { bank, calls, sources, state } = await loadedBank(IDLE_SUSPEND_MS);
    vi.advanceTimersByTime(IDLE_SUSPEND_MS);
    expect(state()).toBe('suspended');
    bank.play('ping', 0.5);
    expect(calls.resume).toBe(1);
    expect(sources[0].started).toBe(true);
    await Promise.resolve();
    expect(state()).toBe('running');
    sources[0].onended?.();
    vi.advanceTimersByTime(IDLE_SUSPEND_MS);
    expect(calls.suspend).toBe(2);
  });

  it('keeps the context running while a sound is still playing', async () => {
    const { bank, calls } = await loadedBank(IDLE_SUSPEND_MS);
    bank.play('ping', 1);
    vi.advanceTimersByTime(IDLE_SUSPEND_MS * 3);
    expect(calls.suspend).toBe(0);
  });

  it('waits for the last of several overlapping sounds', async () => {
    const { bank, calls, sources } = await loadedBank(IDLE_SUSPEND_MS);
    bank.play('ping', 1);
    bank.play('ping', 1);
    sources[0].onended?.();
    vi.advanceTimersByTime(IDLE_SUSPEND_MS);
    expect(calls.suspend).toBe(0);
    sources[1].onended?.();
    vi.advanceTimersByTime(IDLE_SUSPEND_MS);
    expect(calls.suspend).toBe(1);
  });

  it('starts the idle time again when a sound plays just before it runs out', async () => {
    const { bank, calls, sources } = await loadedBank(IDLE_SUSPEND_MS);
    vi.advanceTimersByTime(IDLE_SUSPEND_MS - 1);
    bank.play('ping', 1);
    vi.advanceTimersByTime(1);
    expect(calls.suspend).toBe(0);
    sources[0].onended?.();
    vi.advanceTimersByTime(IDLE_SUSPEND_MS - 1);
    expect(calls.suspend).toBe(0);
    vi.advanceTimersByTime(1);
    expect(calls.suspend).toBe(1);
  });

  it('plays nothing and leaves a suspended context alone for silence, unknown sounds or too many voices', async () => {
    const { bank, calls, sources } = await loadedBank(IDLE_SUSPEND_MS);
    vi.advanceTimersByTime(IDLE_SUSPEND_MS);
    bank.play('ping', 0);
    bank.play('missing', 1);
    expect(sources).toHaveLength(0);
    expect(calls.resume).toBe(0);
    for (let i = 0; i < MAX_VOICES + 2; i++) bank.play('ping', 1);
    expect(sources).toHaveLength(MAX_VOICES);
  });

  it('reports the sounds that failed to load and still suspends when idle', async () => {
    vi.stubGlobal('fetch', async (url: string) => ({ ok: url !== 'bad.wav', arrayBuffer: async () => new ArrayBuffer(8) }));
    const fake = fakeContext();
    const bank = new SoundBank(IDLE_SUSPEND_MS, fake.ctx);
    const missing = await bank.load([{ key: 'ok', url: 'ok.wav' }, { key: 'bad', url: 'bad.wav' }]);
    expect(missing).toEqual(['bad.wav']);
    vi.advanceTimersByTime(IDLE_SUSPEND_MS);
    expect(fake.calls.suspend).toBe(1);
  });
});
