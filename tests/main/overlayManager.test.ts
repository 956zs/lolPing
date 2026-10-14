import { EventEmitter } from 'node:events';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { HyprMonitor, HyprRule } from '../../src/main/hyprRules';
import type { EmoteArt } from '../../src/shared/emotes';
import { OVERLAY_EMOTED, OVERLAY_PINGED } from '../../src/shared/ipc';
import { DEFAULT_SETTINGS, overlaySettings } from '../../src/shared/settings';

type Rect = { x: number; y: number; width: number; height: number };
type FakeDisplay = { id: number; bounds: Rect; scaleFactor: number };

const twoDisplays = (): FakeDisplay[] => [
  { id: 1, bounds: { x: 0, y: 0, width: 1920, height: 1080 }, scaleFactor: 1 },
  { id: 2, bounds: { x: 1920, y: 0, width: 1280, height: 1024 }, scaleFactor: 1 },
];

const h = vi.hoisted(() => ({
  windows: [] as Array<{
    x: number;
    bounds: Rect;
    title?: string;
    destroyed: boolean;
    emit: (event: string, ...args: unknown[]) => boolean;
    webContents: { send: ReturnType<typeof vi.fn> };
  }>,
  handlers: new Map<string, (event: unknown, raw: unknown) => void>(),
  isMac: false,
  isLinux: false,
  displays: [] as FakeDisplay[],
  /** What happened, in order: rules added and windows made. */
  log: [] as string[],
}));

vi.mock('electron', async () => {
  const { EventEmitter: Emitter } = await import('node:events');
  class FakeWindow extends Emitter {
    static fromWebContents(wc: unknown): FakeWindow | null {
      return (h.windows as unknown as FakeWindow[]).find((w) => w.webContents === wc) ?? null;
    }
    x: number;
    bounds: Rect;
    title?: string;
    destroyed = false;
    webContents = Object.assign(new Emitter(), { send: vi.fn() });
    constructor(opts: Rect & { title?: string }) {
      super();
      this.x = opts.x;
      this.bounds = { x: opts.x, y: opts.y, width: opts.width, height: opts.height };
      this.title = opts.title;
      h.windows.push(this);
      h.log.push(`window ${opts.title ?? ''}`);
    }
    setIgnoreMouseEvents(): void {}
    setAlwaysOnTop(): void {}
    setVisibleOnAllWorkspaces(): void {}
    setBounds(r: Rect): void {
      this.bounds = r;
    }
    showInactive(): void {}
    isDestroyed(): boolean {
      return this.destroyed;
    }
    destroy(): void {
      this.destroyed = true;
    }
  }
  const screen = Object.assign(new Emitter(), {
    getAllDisplays: () => h.displays,
    getPrimaryDisplay: () => h.displays[0],
    dipToScreenRect: (_win: unknown, r: unknown) => r,
  });
  screen.setMaxListeners(0); // every test starts a manager on this one fake screen
  const ipcMain = { on: (channel: string, fn: (event: unknown, raw: unknown) => void) => h.handlers.set(channel, fn) };
  return { BrowserWindow: FakeWindow, ipcMain, screen };
});

vi.mock('../../src/main/paths', () => ({
  get IS_MAC() {
    return h.isMac;
  },
  get IS_LINUX() {
    return h.isLinux;
  },
  loadPage: () => Promise.resolve(),
  preloadPath: () => 'preload.js',
}));

type Manager = InstanceType<typeof import('../../src/main/overlayManager').OverlayManager>;
let mgr: Manager;
let resolveArt: ReturnType<typeof vi.fn<(ref: string, remote: boolean) => EmoteArt>>;

const art = (ref: string, remote: boolean): EmoteArt => ({ kind: 'bundled', slug: `${ref}${remote ? '-remote' : ''}` });
/** What display 1 (index 0) or display 2 (index 1) was told, minus the settings it gets on load. */
const sent = (i: number): Array<[string, unknown]> =>
  h.windows[i].webContents.send.mock.calls.filter((c) => c[0] !== 'overlay:settings') as Array<[string, unknown]>;

beforeEach(async () => {
  h.windows.length = 0;
  h.handlers.clear();
  h.isMac = false;
  h.isLinux = false;
  h.displays = twoDisplays();
  h.log.length = 0;
  vi.resetModules();
  const { OverlayManager } = await import('../../src/main/overlayManager');
  resolveArt = vi.fn(art);
  mgr = new OverlayManager(overlaySettings({ ...DEFAULT_SETTINGS, clickEmoteId: 'okay' }), resolveArt);
  mgr.start();
});

describe('OverlayManager wheels', () => {
  it('forwards the open wheel\'s kind with every move and the release', () => {
    mgr.handle({ type: 'wheelOpen', x: 100, y: 200, wheel: 'emote' });
    mgr.handle({ type: 'wheelMove', x: 110, y: 210, wheel: 'emote' });
    mgr.handle({ type: 'wheelRelease', x: 120, y: 220, wheel: 'emote' });
    expect(sent(0)).toEqual([
      ['wheel:open', { x: 100, y: 200, wheel: 'emote' }],
      ['wheel:move', { x: 110, y: 210, wheel: 'emote' }],
      ['wheel:release', { x: 120, y: 220, wheel: 'emote' }],
    ]);
  });

  it('keeps the ping wheel a ping wheel after an emote one', () => {
    mgr.handle({ type: 'wheelOpen', x: 1, y: 1, wheel: 'emote' });
    mgr.handle({ type: 'wheelRelease', x: 1, y: 1, wheel: 'emote' });
    mgr.handle({ type: 'wheelOpen', x: 5, y: 5, wheel: 'ping' });
    mgr.handle({ type: 'wheelMove', x: 6, y: 6, wheel: 'ping' });
    expect(sent(0).slice(2)).toEqual([
      ['wheel:open', { x: 5, y: 5, wheel: 'ping' }],
      ['wheel:move', { x: 6, y: 6, wheel: 'ping' }],
    ]);
  });
});

describe('OverlayManager emote clicks', () => {
  it('shows the click emote where it was clicked, clears ours on the other displays and shares it', () => {
    const shared = vi.fn();
    mgr.on('sharedEmote', shared);
    mgr.on('shared', shared);
    mgr.handle({ type: 'click', x: 1920 + 300, y: 400, wheel: 'emote' });
    expect(sent(1)).toEqual([['emote:spawn', { owner: 'self', art: art('okay', false), x: 300, y: 400 }]]);
    expect(sent(0)).toEqual([['emote:clear', { owner: 'self' }]]);
    expect(resolveArt).toHaveBeenCalledWith('okay', false);
    expect(shared).toHaveBeenCalledOnce();
    expect(shared).toHaveBeenCalledWith({ ref: 'okay', displayId: 2, x: 300, y: 400 });
  });

  it('still drops the click ping for a ping click', () => {
    const emoted = vi.fn();
    const pinged = vi.fn();
    mgr.on('sharedEmote', emoted);
    mgr.on('shared', pinged);
    mgr.handle({ type: 'click', x: 50, y: 60, wheel: 'ping' });
    expect(sent(0)).toEqual([['ping:spawn', { id: DEFAULT_SETTINGS.clickPingId, x: 50, y: 60 }]]);
    expect(sent(1)).toEqual([]);
    expect(emoted).not.toHaveBeenCalled();
    expect(pinged).toHaveBeenCalledOnce();
  });
});

describe('OverlayManager emote reports from the overlay', () => {
  const sender = (i: number): unknown => h.windows[i].webContents;

  it('shares a wheel emote from the display that placed it and clears ours elsewhere', () => {
    const shared = vi.fn();
    mgr.on('sharedEmote', shared);
    h.handlers.get(OVERLAY_EMOTED)!({ sender: sender(1) }, { ref: 'c:' + 'a'.repeat(32), x: 10, y: 20 });
    expect(shared).toHaveBeenCalledWith({ ref: 'c:' + 'a'.repeat(32), displayId: 2, x: 10, y: 20 });
    expect(sent(0)).toEqual([['emote:clear', { owner: 'self' }]]);
    expect(sent(1)).toEqual([]);
  });

  it('ignores a malformed report', () => {
    const shared = vi.fn();
    mgr.on('sharedEmote', shared);
    const report = h.handlers.get(OVERLAY_EMOTED)!;
    for (const raw of [
      null, 'x', 5, {}, { ref: 'Bad Ref', x: 1, y: 1 }, { ref: 'c:abc', x: 1, y: 1 }, { ref: 'okay', x: Number.NaN, y: 1 },
      { ref: 'okay', x: 1, y: Infinity }, { ref: 'okay', x: '1', y: 1 }, { ref: 7, x: 1, y: 1 },
    ]) report({ sender: sender(0) }, raw);
    report({ sender: {} }, { ref: 'okay', x: 1, y: 1 }); // not one of our windows
    expect(shared).not.toHaveBeenCalled();
    expect(sent(0)).toEqual([]);
    expect(sent(1)).toEqual([]);
  });

  it('still shares pings', () => {
    const shared = vi.fn();
    mgr.on('shared', shared);
    h.handlers.get(OVERLAY_PINGED)!({ sender: sender(0) }, { id: 'danger', x: 1, y: 2 });
    expect(shared).toHaveBeenCalledWith({ id: 'danger', displayId: 1, x: 1, y: 2 });
  });
});

describe('OverlayManager remote and preview emotes', () => {
  it('shows a room member\'s emote on the target display with their tag, and clears theirs on the others', () => {
    mgr.spawnRemoteEmote({ displayId: 2, x: 5, y: 6, ref: 'gg_heart', peer: 'peer-1', tag: { name: 'Bob', color: 2 } });
    expect(resolveArt).toHaveBeenCalledWith('gg_heart', true);
    expect(sent(1)).toEqual([['emote:spawn', { owner: 'peer-1', art: art('gg_heart', true), x: 5, y: 6, tag: { name: 'Bob', color: 2 } }]]);
    expect(sent(0)).toEqual([['emote:clear', { owner: 'peer-1' }]]);
  });

  it('previews in the middle of the main display without sharing', () => {
    const shared = vi.fn();
    mgr.on('sharedEmote', shared);
    mgr.previewEmote('nice');
    expect(resolveArt).toHaveBeenCalledWith('nice', false);
    expect(sent(0)).toEqual([['emote:spawn', { owner: 'preview', art: art('nice', false), x: 960, y: 540 }]]);
    expect(sent(1)).toEqual([]);
    expect(shared).not.toHaveBeenCalled();
  });
});

describe('OverlayManager window bounds', () => {
  it('leaves the bottom row of each display uncovered on Windows, so the taskbar does not see a full-screen app', () => {
    for (const w of h.windows) w.emit('ready-to-show');
    expect(h.windows.map((w) => w.bounds)).toEqual([
      { x: 0, y: 0, width: 1920, height: 1079 },
      { x: 1920, y: 0, width: 1280, height: 1023 },
    ]);
  });

  it('keeps pings where they were placed, since the window still starts at the display\'s corner', () => {
    mgr.handle({ type: 'click', x: 1920 + 300, y: 400, wheel: 'ping' });
    expect(sent(1)).toEqual([['ping:spawn', { id: DEFAULT_SETTINGS.clickPingId, x: 300, y: 400 }]]);
  });

  it('covers the whole display on macOS', async () => {
    h.windows.length = 0;
    h.isMac = true;
    vi.resetModules();
    const { OverlayManager } = await import('../../src/main/overlayManager');
    new OverlayManager(overlaySettings(DEFAULT_SETTINGS), resolveArt).start();
    for (const w of h.windows) w.emit('ready-to-show');
    expect(h.windows.map((w) => w.bounds)).toEqual([
      { x: 0, y: 0, width: 1920, height: 1080 },
      { x: 1920, y: 0, width: 1280, height: 1024 },
    ]);
  });
});

/** A stand-in for Hyprland: its monitors, and whether adding a rule works. */
class FakeHost extends EventEmitter {
  monitorList: HyprMonitor[] = [
    { name: 'DP-1', rect: { x: 0, y: 0, width: 1920, height: 1080 } },
    { name: 'HDMI-A-1', rect: { x: 1920, y: 0, width: 1280, height: 1024 } },
  ];
  refuse = new Set<string>();
  /** hyprctl fails: no monitors, and no rule is added. */
  down = false;
  monitors(): Promise<HyprMonitor[]> {
    return Promise.resolve(this.down ? [] : this.monitorList);
  }
  addRules(rules: readonly HyprRule[]): Promise<boolean> {
    for (const r of rules) h.log.push(`rule ${r.name}`);
    return Promise.resolve(!this.down && rules.every((r) => !this.refuse.has(r.name)));
  }
}

describe('OverlayManager on Hyprland', () => {
  const live = () => h.windows.filter((w) => !w.destroyed);
  /** Settles the manager's queue of Hyprland work, for checks that nothing (more) happens. */
  const settle = () => new Promise<void>((resolve) => setImmediate(resolve));

  const started: Manager[] = [];
  // Every manager listens to the one fake screen: one left from an earlier test would make windows of its own.
  afterEach(() => {
    for (const m of started) m.destroy();
    started.length = 0;
  });

  async function startLinux(host: FakeHost | null): Promise<Manager> {
    h.windows.length = 0;
    h.log.length = 0;
    h.isLinux = true;
    vi.resetModules();
    const { OverlayManager } = await import('../../src/main/overlayManager');
    const m = new OverlayManager(overlaySettings(DEFAULT_SETTINGS), resolveArt, host);
    started.push(m);
    m.start();
    return m;
  }

  it('adds each monitor\'s rule before making its window, and names the window after the monitor', async () => {
    await startLinux(new FakeHost());
    await vi.waitFor(() => expect(live()).toHaveLength(2));
    expect(h.log).toEqual([
      'rule lolping-overlay-DP-1', 'window lolPing overlay DP-1', 'rule lolping-overlay-HDMI-A-1', 'window lolPing overlay HDMI-A-1',
    ]);
  });

  it('keeps the title the rule matches when the page sets its own', async () => {
    await startLinux(new FakeHost());
    await vi.waitFor(() => expect(live()).toHaveLength(2));
    const event = { preventDefault: vi.fn() };
    h.windows[0].emit('page-title-updated', event, 'lolPing overlay');
    expect(event.preventDefault).toHaveBeenCalled();
  });

  it('covers the whole display and takes helper points as DIPs, whatever the scale', async () => {
    h.displays[1].scaleFactor = 2;
    const m = await startLinux(new FakeHost());
    await vi.waitFor(() => expect(live()).toHaveLength(2));
    for (const w of h.windows) w.emit('ready-to-show');
    expect(h.windows.map((w) => w.bounds)).toEqual([h.displays[0].bounds, h.displays[1].bounds]);
    m.handle({ type: 'click', x: 1920 + 300, y: 400, wheel: 'ping' });
    expect(sent(1)).toEqual([['ping:spawn', { id: DEFAULT_SETTINGS.clickPingId, x: 300, y: 400 }]]);
  });

  it('makes a new window for a display whose rect changed, and leaves the others alone', async () => {
    const host = new FakeHost();
    await startLinux(host);
    await vi.waitFor(() => expect(live()).toHaveLength(2));
    const { screen } = await import('electron');
    h.displays[1] = { ...h.displays[1], bounds: { x: 1920, y: 0, width: 1440, height: 900 } };
    host.monitorList[1] = { name: 'HDMI-A-1', rect: { x: 1920, y: 0, width: 1440, height: 900 } };
    screen.emit('display-metrics-changed');
    await vi.waitFor(() => expect(h.windows).toHaveLength(3));
    expect(h.windows[0].destroyed).toBe(false);
    expect(h.windows[1].destroyed).toBe(true);
    expect(h.windows[2]).toMatchObject({ title: 'lolPing overlay HDMI-A-1', bounds: { x: 1920, y: 0, width: 1440, height: 900 } });
  });

  it('keeps every window when the displays report a change that moves nothing', async () => {
    await startLinux(new FakeHost());
    await vi.waitFor(() => expect(live()).toHaveLength(2));
    const { screen } = await import('electron');
    screen.emit('display-metrics-changed');
    await settle();
    expect(h.windows).toHaveLength(2);
    expect(live()).toHaveLength(2);
  });

  it('remakes every window with its rule added again after a config reload', async () => {
    const host = new FakeHost();
    await startLinux(host);
    await vi.waitFor(() => expect(live()).toHaveLength(2));
    h.log.length = 0;
    host.emit('reloaded');
    await vi.waitFor(() => expect(h.windows).toHaveLength(4));
    expect(h.windows.slice(0, 2).every((w) => w.destroyed)).toBe(true);
    expect(h.log).toEqual([
      'rule lolping-overlay-DP-1', 'window lolPing overlay DP-1', 'rule lolping-overlay-HDMI-A-1', 'window lolPing overlay HDMI-A-1',
    ]);
  });

  it('makes a window for a monitor that Hyprland reports later', async () => {
    const host = new FakeHost();
    const second = host.monitorList[1];
    host.monitorList = [host.monitorList[0]];
    await startLinux(host);
    await vi.waitFor(() => expect(live()).toHaveLength(1));
    host.monitorList.push(second);
    host.emit('monitors');
    await vi.waitFor(() => expect(live()).toHaveLength(2));
    expect(h.windows[1].title).toBe('lolPing overlay HDMI-A-1');
  });

  it('makes no window for a display without a Hyprland monitor or whose rule could not be added', async () => {
    const host = new FakeHost();
    host.refuse.add('lolping-overlay-HDMI-A-1');
    host.monitorList.push({ name: 'DP-9', rect: { x: 9000, y: 0, width: 800, height: 600 } }); // no display there
    await startLinux(host);
    await vi.waitFor(() => expect(h.log).toContain('rule lolping-overlay-HDMI-A-1'));
    await settle();
    expect(live().map((w) => w.title)).toEqual(['lolPing overlay DP-1']);
  });

  describe('when Hyprland fails', () => {
    beforeEach(() => {
      vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] }); // settle() still runs on setImmediate
    });
    afterEach(() => {
      for (const m of started) m.destroy();
      started.length = 0;
      vi.useRealTimers();
    });

    /** Display 2 becomes 1440 × 900, on Electron's side and on Hyprland's. */
    async function resizeSecond(host: FakeHost): Promise<void> {
      const { screen } = await import('electron');
      h.displays[1] = { ...h.displays[1], bounds: { x: 1920, y: 0, width: 1440, height: 900 } };
      host.monitorList[1] = { name: 'HDMI-A-1', rect: { x: 1920, y: 0, width: 1440, height: 900 } };
      screen.emit('display-metrics-changed');
      await settle();
    }

    const advance = async (ms: number): Promise<void> => {
      await vi.advanceTimersByTimeAsync(ms);
      await settle();
    };

    it('keeps the overlays it has when Hyprland doesn\'t answer, and replaces them once it does', async () => {
      const host = new FakeHost();
      await startLinux(host);
      await settle();
      expect(live()).toHaveLength(2);
      host.down = true;
      await resizeSecond(host);
      expect(h.windows).toHaveLength(2);
      expect(live()).toHaveLength(2);
      host.down = false;
      await advance(1000);
      expect(h.windows).toHaveLength(3);
      expect(h.windows[1].destroyed).toBe(true);
      expect(h.windows[2]).toMatchObject({ destroyed: false, title: 'lolPing overlay HDMI-A-1', bounds: { width: 1440, height: 900 } });
    });

    it('keeps the old window until the rule for its replacement is in', async () => {
      const host = new FakeHost();
      await startLinux(host);
      await settle();
      host.refuse.add('lolping-overlay-HDMI-A-1');
      await resizeSecond(host);
      expect(live().map((w) => w.title)).toEqual(['lolPing overlay DP-1', 'lolPing overlay HDMI-A-1']);
      expect(h.windows).toHaveLength(2);
      host.refuse.clear();
      await advance(1000);
      expect(h.windows[1].destroyed).toBe(true);
      expect(live().map((w) => w.bounds.width)).toEqual([1920, 1440]);
    });

    it('keeps the old windows when a config reload\'s rules can\'t be added, and remakes them later', async () => {
      const host = new FakeHost();
      await startLinux(host);
      await settle();
      host.down = true;
      host.emit('reloaded');
      await settle();
      expect(live()).toHaveLength(2);
      expect(h.windows).toHaveLength(2);
      host.down = false;
      await advance(1000);
      expect(h.windows).toHaveLength(4);
      expect(live()).toEqual(h.windows.slice(2));
    });

    it('tries again after 1 s, 2 s and then every 5 s until every display has an overlay', async () => {
      const host = new FakeHost();
      host.refuse.add('lolping-overlay-HDMI-A-1');
      await startLinux(host);
      await settle();
      const tries = () => h.log.filter((l) => l === 'rule lolping-overlay-HDMI-A-1').length;
      expect(tries()).toBe(1);
      await advance(999);
      expect(tries()).toBe(1);
      await advance(1);
      expect(tries()).toBe(2);
      await advance(2000);
      expect(tries()).toBe(3);
      await advance(5000);
      expect(tries()).toBe(4);
      await advance(5000);
      expect(tries()).toBe(5);
      host.refuse.clear();
      await advance(5000);
      expect(tries()).toBe(6);
      expect(live().map((w) => w.title)).toEqual(['lolPing overlay DP-1', 'lolPing overlay HDMI-A-1']);
      await advance(60_000);
      expect(tries()).toBe(6);
    });

    it('stops trying once destroyed', async () => {
      const host = new FakeHost();
      host.down = true;
      const m = await startLinux(host);
      await settle();
      m.destroy();
      host.down = false;
      await advance(60_000);
      expect(h.windows).toEqual([]);
    });
  });

  it('makes no windows without Hyprland', async () => {
    await startLinux(null);
    await settle();
    expect(h.windows).toEqual([]);
  });

  it('makes no window once it has been destroyed', async () => {
    const m = await startLinux(new FakeHost());
    m.destroy();
    await settle();
    expect(live()).toEqual([]);
  });
});
