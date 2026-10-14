import { mkdtempSync, rmSync } from 'node:fs';
import { createServer, type Server, type Socket } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { Hyprland, Retry } from '../../src/main/hyprland';
import { overlayRule, SETTINGS_RULE } from '../../src/main/hyprRules';

/** A stand-in for hyprctl: answers by the request, and records every call. */
function fakeHyprctl(answers: { status?: string | null; monitors?: string | null; add?: string | null } = {}) {
  const calls: string[][] = [];
  const run = vi.fn((args: readonly string[]): Promise<string | null> => {
    calls.push([...args]);
    if (args[1] === 'status') return Promise.resolve(answers.status === undefined ? '{"configProvider":"lua"}' : answers.status);
    if (args[1] === 'monitors') return Promise.resolve(answers.monitors === undefined ? '[]' : answers.monitors);
    return Promise.resolve(answers.add === undefined ? 'ok' : answers.add);
  });
  return { run, calls, adds: () => calls.filter((c) => c[0] === 'eval' || c[0] === 'keyword') };
}

let dir: string;
let server: Server | null = null;
let clients: Socket[] = [];
let hypr: Hyprland | null = null;

beforeEach(() => {
  dir = mkdtempSync(join(tmpdir(), 'lolping-hypr-'));
  clients = [];
});

afterEach(async () => {
  hypr?.stop();
  hypr = null;
  for (const c of clients) c.destroy();
  if (server) await new Promise<void>((resolve) => server?.close(() => resolve()));
  server = null;
  rmSync(dir, { recursive: true, force: true });
});

/** Hyprland's event socket in the temp folder; resolves once the event socket is listening. */
function eventSocket(): Promise<void> {
  server = createServer((socket) => clients.push(socket));
  return new Promise((resolve) => server?.listen(join(dir, '.socket2.sock'), resolve));
}

const send = (line: string): void => {
  for (const c of clients) c.write(`${line}\n`);
};

describe('Hyprland', () => {
  it('reads the monitors, and none when hyprctl fails or prints something else', async () => {
    const json = '[{"name":"eDP-1","x":3840,"y":0,"width":1920,"height":1200,"scale":1.00,"transform":0}]';
    expect(await new Hyprland({ socketDir: dir, hyprctl: fakeHyprctl({ monitors: json }).run }).monitors())
      .toEqual([{ name: 'eDP-1', rect: { x: 3840, y: 0, width: 1920, height: 1200 } }]);
    expect(await new Hyprland({ socketDir: dir, hyprctl: fakeHyprctl({ monitors: null }).run }).monitors()).toEqual([]);
    expect(await new Hyprland({ socketDir: dir, hyprctl: fakeHyprctl({ monitors: 'oops' }).run }).monitors()).toEqual([]);
  });

  it('adds rules with eval on a Lua config, once each', async () => {
    const h = fakeHyprctl();
    hypr = new Hyprland({ socketDir: dir, hyprctl: h.run });
    expect(await hypr.addRules([SETTINGS_RULE, overlayRule('eDP-1')])).toBe(true);
    expect(await hypr.addRules([overlayRule('eDP-1')])).toBe(true);
    expect(h.adds()).toEqual([['eval', SETTINGS_RULE.lua], ['eval', overlayRule('eDP-1').lua]]);
    expect(h.calls.filter((c) => c[1] === 'status')).toHaveLength(1); // the config language is asked once
  });

  it('adds rules with keyword on a hyprlang config, and reports a rule it can\'t write there', async () => {
    const h = fakeHyprctl({ status: 'unknown request' });
    hypr = new Hyprland({ socketDir: dir, hyprctl: h.run });
    expect(await hypr.addRules([overlayRule('DP-1'), overlayRule('a,b')])).toBe(false);
    expect(h.adds()).toEqual([['keyword', 'windowrule', overlayRule('DP-1').hyprlang]]);
  });

  it('runs one add at a time, so rules asked for twice at once are added once', async () => {
    const h = fakeHyprctl();
    hypr = new Hyprland({ socketDir: dir, hyprctl: h.run });
    const both = await Promise.all([hypr.addRules([overlayRule('DP-1')]), hypr.addRules([overlayRule('DP-1')])]);
    expect(both).toEqual([true, true]);
    expect(h.adds()).toHaveLength(1);
  });

  it('tries a rule again when hyprctl failed, and asks for the config language again when Hyprland was away', async () => {
    let up = false;
    const calls: string[][] = [];
    hypr = new Hyprland({
      socketDir: dir,
      hyprctl: (args) => {
        calls.push([...args]);
        if (!up) return Promise.resolve(null);
        return Promise.resolve(args[1] === 'status' ? '{"configProvider":"lua"}' : 'ok');
      },
    });
    expect(await hypr.addRules([overlayRule('DP-1')])).toBe(false);
    up = true;
    expect(await hypr.addRules([overlayRule('DP-1')])).toBe(true);
    expect(calls).toEqual([['-j', 'status'], ['-j', 'status'], ['eval', overlayRule('DP-1').lua]]);
  });

  it('adds the rules again after Hyprland reloads its config, and reports monitor changes', async () => {
    await eventSocket();
    let now = 0;
    const h = fakeHyprctl();
    hypr = new Hyprland({ socketDir: dir, hyprctl: h.run, now: () => now });
    const reloaded = vi.fn();
    const monitors = vi.fn();
    hypr.on('reloaded', reloaded);
    hypr.on('monitors', monitors);
    hypr.watch();
    await vi.waitFor(() => expect(clients).toHaveLength(1));
    await hypr.addRules([SETTINGS_RULE]);
    now = 5000;
    send('activewindow>>kitty,~');
    send('monitoradded>>DP-2');
    send('monitoraddedv2>>1,DP-2,desc');
    send('configreloaded>>');
    await vi.waitFor(() => expect(reloaded).toHaveBeenCalledOnce());
    expect(monitors).toHaveBeenCalledOnce();
    await hypr.addRules([SETTINGS_RULE]);
    expect(h.adds()).toHaveLength(2);
  });

  it('stops adding the rules again when every add is followed by a reload, until a reload comes on its own', async () => {
    await eventSocket();
    let now = 0;
    const h = fakeHyprctl();
    hypr = new Hyprland({ socketDir: dir, hyprctl: h.run, now: () => now });
    const reloaded = vi.fn();
    hypr.on('reloaded', reloaded);
    hypr.watch();
    await vi.waitFor(() => expect(clients).toHaveLength(1));
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    try {
      for (let i = 1; i <= 4; i++) {
        await hypr.addRules([SETTINGS_RULE]);
        now += 100; // the reload comes right after the add
        send('configreloaded>>');
        await vi.waitFor(() => expect(reloaded.mock.calls.length + warn.mock.calls.length).toBe(i));
      }
      expect(reloaded).toHaveBeenCalledTimes(3);
      expect(warn).toHaveBeenCalledOnce();
      expect(h.adds()).toHaveLength(4);
      now += 60_000; // the user reloads the config much later
      send('configreloaded>>');
      await vi.waitFor(() => expect(reloaded).toHaveBeenCalledTimes(4));
    } finally {
      warn.mockRestore();
    }
  });

  it('connects again when the event socket closes, and not after stop()', async () => {
    await eventSocket();
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    try {
      hypr = new Hyprland({ socketDir: dir, hyprctl: fakeHyprctl().run });
      hypr.watch();
      await vi.waitFor(() => expect(clients).toHaveLength(1));
      clients[0].destroy();
      await vi.waitFor(() => expect(vi.getTimerCount()).toBe(1)); // the reconnect is scheduled once the close is seen
      vi.runOnlyPendingTimers();
      await vi.waitFor(() => expect(clients).toHaveLength(2));
      hypr.stop();
      expect(vi.getTimerCount()).toBe(0);
    } finally {
      vi.useRealTimers();
    }
  });
});

describe('Retry', () => {
  beforeEach(() => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  /** Lets the timers run for `ms`, then lets an attempt they started finish. */
  const advance = async (ms: number): Promise<void> => {
    await vi.advanceTimersByTimeAsync(ms);
    await new Promise<void>((resolve) => setImmediate(resolve));
  };

  it('tries again after 1 s, 2 s and then every 5 s until the attempt works', async () => {
    let ok = false;
    const attempt = vi.fn(() => Promise.resolve(ok));
    const retry = new Retry(attempt);
    expect(await retry.run()).toBe(false);
    await advance(999);
    expect(attempt).toHaveBeenCalledTimes(1);
    await advance(1);
    expect(attempt).toHaveBeenCalledTimes(2);
    await advance(2000);
    expect(attempt).toHaveBeenCalledTimes(3);
    await advance(5000);
    expect(attempt).toHaveBeenCalledTimes(4);
    await advance(5000);
    expect(attempt).toHaveBeenCalledTimes(5);
    ok = true;
    await advance(5000);
    expect(attempt).toHaveBeenCalledTimes(6);
    await advance(60_000);
    expect(attempt).toHaveBeenCalledTimes(6);
    expect(vi.getTimerCount()).toBe(0);
  });

  it('starts the delays over after a success', async () => {
    let ok = false;
    const attempt = vi.fn(() => Promise.resolve(ok));
    const retry = new Retry(attempt);
    await retry.run();
    await advance(1000);
    await advance(2000);
    expect(attempt).toHaveBeenCalledTimes(3);
    ok = true;
    expect(await retry.run()).toBe(true);
    expect(vi.getTimerCount()).toBe(0);
    ok = false;
    await retry.run();
    await advance(1000);
    expect(attempt).toHaveBeenCalledTimes(6);
  });

  it('tries now in place of a waiting try, and keeps making the wait longer', async () => {
    const attempt = vi.fn(() => Promise.resolve(false));
    const retry = new Retry(attempt);
    await retry.run();
    await advance(500);
    await retry.run(); // a new reason to try, half way through the first wait
    expect(attempt).toHaveBeenCalledTimes(2);
    expect(vi.getTimerCount()).toBe(1);
    await advance(1999);
    expect(attempt).toHaveBeenCalledTimes(2);
    await advance(1);
    expect(attempt).toHaveBeenCalledTimes(3);
  });

  it('counts an attempt that throws as one that failed', async () => {
    const attempt = vi.fn(() => Promise.reject(new Error('hyprctl went away')));
    const retry = new Retry(attempt);
    expect(await retry.run()).toBe(false);
    await advance(1000);
    expect(attempt).toHaveBeenCalledTimes(2);
  });

  it('does nothing after stop(), not even a try that was waiting', async () => {
    const attempt = vi.fn(() => Promise.resolve(false));
    const retry = new Retry(attempt);
    await retry.run();
    retry.stop();
    expect(vi.getTimerCount()).toBe(0);
    expect(await retry.run()).toBe(false);
    await advance(60_000);
    expect(attempt).toHaveBeenCalledOnce();
  });

  it('schedules nothing when stopped while an attempt was running', async () => {
    let finish: (ok: boolean) => void = () => undefined;
    const retry = new Retry(() => new Promise<boolean>((resolve) => {
      finish = resolve;
    }));
    const running = retry.run();
    retry.stop();
    finish(false);
    await running;
    expect(vi.getTimerCount()).toBe(0);
  });
});
