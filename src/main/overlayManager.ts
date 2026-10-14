import { BrowserWindow, ipcMain, screen, type Display, type Rectangle, type WebContents } from 'electron';
import { EventEmitter } from 'node:events';
import { isEmoteRefShape, type EmoteArt, type EmoteRef } from '../shared/emotes';
import {
  OVERLAY_ASSETS, OVERLAY_EMOTED, OVERLAY_PINGED, type OverlayChannel, type OverlayEvents, type PingTag,
} from '../shared/ipc';
import { isPingId, type PingId } from '../shared/pings';
import type { HelperEvent, WheelKind } from '../shared/protocol';
import type { OverlaySettings } from '../shared/settings';
import { physicalToLocal, type DisplayMap } from './coords';
import { numberDisplays, type NumberedDisplay } from './displayNumbers';
import { Retry } from './hyprland';
import { matchMonitors, overlayRule, overlayTitle, type HyprMonitor, type HyprRule } from './hyprRules';
import { IS_LINUX, IS_MAC, loadPage, preloadPath } from './paths';

/** A ping the user just placed, in overlay-local CSS px: the room shares these. */
export interface SharedPing {
  id: PingId;
  displayId: number;
  x: number;
  y: number;
}

/** An emote the user just placed, in overlay-local CSS px: the room shares these. */
export interface SharedEmote {
  ref: EmoteRef;
  displayId: number;
  x: number;
  y: number;
}

const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);

/**
 * Where a display's overlay window goes. Windows takes a shown window that covers a whole display for a full-screen app:
 * the taskbar drops behind it and an auto-hidden one won't slide up. One DIP short of the bottom edge, it doesn't.
 */
export const overlayBounds = (d: Display): Rectangle =>
  IS_MAC || IS_LINUX ? d.bounds : { ...d.bounds, height: d.bounds.height - 1 };

/** What the Linux overlays need from Hyprland (hyprland.ts): monitors, rules added before a window shows, events. */
export interface OverlayHost {
  monitors(): Promise<HyprMonitor[]>;
  addRules(rules: readonly HyprRule[]): Promise<boolean>;
  /** 'reloaded': the rules are gone and have to be added again. 'monitors': a monitor came or went. */
  on(event: 'reloaded' | 'monitors', listener: () => void): unknown;
}

/**
 * One transparent, click-through, always-on-top window per display.
 * Emits 'shared' (SharedPing) for every wheel or trigger+click ping and 'sharedEmote' (SharedEmote) for every wheel or
 * trigger+click emote, never for settings previews.
 */
export class OverlayManager extends EventEmitter {
  readonly missingAssets = new Set<string>();
  private readonly windows = new Map<number, BrowserWindow>();
  private maps: DisplayMap[] = [];
  private numberedList: NumberedDisplay[] = [];
  private wheelDisplay: number | null = null;
  private wheelKind: WheelKind = 'ping';
  /** Linux: the monitor and rect each window was made for. Wayland can't move or resize windows: a change remakes it. */
  private readonly shapes = new Map<number, string>();
  private hyprQueue: Promise<unknown> = Promise.resolve();
  /** Linux: the next placement remakes every window, because a config reload dropped the rules they were made with. */
  private remakeAll = false;
  /** Linux: places the windows again, a little later each time, until every display has one. */
  private readonly placement = new Retry(() => this.queuePlacement());
  private closed = false;

  /**
   * `resolveArt` says what an emote looks like: `remote` is true for a room member's, whose files may be missing here.
   * `hypr` places the windows on Linux; without it there are no overlays there.
   */
  constructor(
    private settings: OverlaySettings,
    private readonly resolveArt: (ref: EmoteRef, remote: boolean) => EmoteArt,
    private readonly hypr: OverlayHost | null = null,
  ) {
    super();
  }

  start(): void {
    ipcMain.on(OVERLAY_ASSETS, (_event, missing: unknown) => {
      if (!Array.isArray(missing)) return;
      for (const m of missing) if (typeof m === 'string') this.missingAssets.add(m);
    });
    ipcMain.on(OVERLAY_PINGED, (event, raw: unknown) => {
      const p = raw as { id?: unknown; x?: unknown; y?: unknown } | null;
      if (!p || !isPingId(p.id) || !isNum(p.x) || !isNum(p.y)) return;
      const displayId = this.displayOf(event.sender);
      if (displayId !== undefined) this.emit('shared', { id: p.id, displayId, x: p.x, y: p.y } satisfies SharedPing);
    });
    ipcMain.on(OVERLAY_EMOTED, (event, raw: unknown) => {
      const p = raw as { ref?: unknown; x?: unknown; y?: unknown } | null;
      if (!p || !isEmoteRefShape(p.ref) || !isNum(p.x) || !isNum(p.y)) return;
      const displayId = this.displayOf(event.sender);
      if (displayId === undefined) return;
      this.emit('sharedEmote', { ref: p.ref, displayId, x: p.x, y: p.y } satisfies SharedEmote);
      // The wheel's own overlay already shows it. Another display may still hold an earlier one of ours.
      this.clearElsewhere(displayId, 'self');
    });
    this.rebuild();
    screen.on('display-added', () => this.rebuild());
    screen.on('display-removed', () => this.rebuild());
    screen.on('display-metrics-changed', () => this.rebuild());
    this.hypr?.on('monitors', () => this.rebuild());
    this.hypr?.on('reloaded', () => {
      this.remakeAll = true;
      this.rebuild();
    });
  }

  updateSettings(s: OverlaySettings): void {
    this.settings = s;
    for (const id of this.windows.keys()) this.send(id, 'overlay:settings', s);
  }

  handle(ev: HelperEvent): void {
    switch (ev.type) {
      case 'wheelOpen': {
        const p = physicalToLocal(ev.x, ev.y, this.maps);
        if (!p) return;
        this.wheelDisplay = p.displayId;
        this.wheelKind = ev.wheel;
        this.send(p.displayId, 'wheel:open', { x: p.x, y: p.y, wheel: ev.wheel });
        break;
      }
      case 'wheelMove':
      case 'wheelRelease': {
        if (this.wheelDisplay === null) return;
        const p = physicalToLocal(ev.x, ev.y, this.maps, this.wheelDisplay);
        if (!p) return;
        this.send(p.displayId, ev.type === 'wheelMove' ? 'wheel:move' : 'wheel:release', { x: p.x, y: p.y, wheel: this.wheelKind });
        if (ev.type === 'wheelRelease') this.wheelDisplay = null;
        break;
      }
      case 'click': {
        const p = physicalToLocal(ev.x, ev.y, this.maps);
        if (!p) break;
        if (ev.wheel === 'emote') {
          const ref = this.settings.clickEmoteId;
          this.send(p.displayId, 'emote:spawn', { owner: 'self', art: this.resolveArt(ref, false), x: p.x, y: p.y });
          this.clearElsewhere(p.displayId, 'self');
          this.emit('sharedEmote', { ref, displayId: p.displayId, x: p.x, y: p.y } satisfies SharedEmote);
          break;
        }
        this.send(p.displayId, 'ping:spawn', { id: this.settings.clickPingId, x: p.x, y: p.y });
        this.emit('shared', { id: this.settings.clickPingId, displayId: p.displayId, x: p.x, y: p.y } satisfies SharedPing);
        break;
      }
      case 'cancel':
        this.cancelWheel();
        break;
      default:
        break;
    }
  }

  previewPing(id: PingId): void {
    const d = screen.getPrimaryDisplay();
    this.send(d.id, 'ping:spawn', { id, x: d.bounds.width / 2, y: d.bounds.height / 2 });
  }

  /** A room member's ping, already mapped onto one of our displays. */
  spawnRemote(p: { displayId: number; x: number; y: number; id: PingId; tag: PingTag }): void {
    this.send(p.displayId, 'ping:spawn', { id: p.id, x: p.x, y: p.y, tag: p.tag });
  }

  /** Shows an emote in the middle of the main display, without sharing it (the settings page's preview). */
  previewEmote(ref: EmoteRef): void {
    const d = screen.getPrimaryDisplay();
    this.send(d.id, 'emote:spawn', { owner: 'preview', art: this.resolveArt(ref, false), x: d.bounds.width / 2, y: d.bounds.height / 2 });
  }

  /** A room member's emote, already mapped onto one of our displays. It replaces their earlier one, whichever display that was on. */
  spawnRemoteEmote(p: { displayId: number; x: number; y: number; ref: EmoteRef; peer: string; tag: PingTag }): void {
    this.send(p.displayId, 'emote:spawn', { owner: p.peer, art: this.resolveArt(p.ref, true), x: p.x, y: p.y, tag: p.tag });
    this.clearElsewhere(p.displayId, p.peer);
  }

  /** This machine's displays with their room numbers (#1 = primary). */
  numbered(): NumberedDisplay[] {
    return this.numberedList;
  }

  toast(title: string, body: string): void {
    this.send(screen.getPrimaryDisplay().id, 'toast:show', { title, body });
  }

  destroy(): void {
    this.closed = true;
    this.placement.stop();
    for (const w of this.windows.values()) if (!w.isDestroyed()) w.destroy();
    this.windows.clear();
  }

  /** Dismisses a wheel that is on screen, if any. Also used when the helper stops: its cancel would never arrive. */
  cancelWheel(): void {
    if (this.wheelDisplay === null) return;
    this.send(this.wheelDisplay, 'wheel:cancel', null);
    this.wheelDisplay = null;
  }

  private displayOf(sender: WebContents): number | undefined {
    const win = BrowserWindow.fromWebContents(sender);
    return [...this.windows].find(([, w]) => w === win)?.[0];
  }

  /** Each owner has one emote on screen: clear theirs on every display but the one that just showed their new one. */
  private clearElsewhere(displayId: number, owner: string): void {
    for (const id of this.windows.keys()) if (id !== displayId) this.send(id, 'emote:clear', { owner });
  }

  private rebuild(): void {
    this.cancelWheel();
    const displays = screen.getAllDisplays();
    // The macOS and Linux helpers already report points in Electron's DIP space; dipToScreenRect only exists on Windows.
    this.maps = displays.map((d) => IS_MAC || IS_LINUX
      ? { id: d.id, dip: d.bounds, phys: d.bounds, scale: 1 }
      : { id: d.id, dip: d.bounds, phys: screen.dipToScreenRect(null, d.bounds), scale: d.scaleFactor });
    this.numberedList = numberDisplays(displays, screen.getPrimaryDisplay().id);
    this.emit('displays', this.numberedList);
    this.dropWindowsNotIn(displays);
    if (IS_LINUX) {
      void this.placement.run();
      return;
    }
    for (const d of displays) {
      const win = this.windows.get(d.id);
      if (win) win.setBounds(overlayBounds(d));
      else this.windows.set(d.id, this.createWindow(d));
    }
  }

  private dropWindowsNotIn(displays: readonly Display[]): void {
    for (const [id, win] of this.windows) {
      if (!displays.some((d) => d.id === id)) {
        win.destroy();
        this.windows.delete(id);
        this.shapes.delete(id);
      }
    }
  }

  /** Linux: one placement at a time; a display that changes meanwhile queues another. */
  private queuePlacement(): Promise<boolean> {
    const run = this.hyprQueue.then(() => this.placeOnHyprland());
    this.hyprQueue = run.catch((err: unknown) => console.warn('[overlay]', err));
    return run;
  }

  /**
   * Linux: Hyprland places each window by a rule matching its title, so the rule is added before the window is made, and
   * a display Hyprland has no monitor for gets no window (without its rule it would be tiled like any other window).
   * A window is only replaced once its replacement's rule is in, so a failed hyprctl call leaves the old one on screen.
   * Resolves with whether every display has an up-to-date window.
   */
  private async placeOnHyprland(): Promise<boolean> {
    const host = this.hypr;
    if (!host || this.closed) return true;
    if (this.remakeAll) {
      this.remakeAll = false;
      this.shapes.clear(); // every window then looks out of date, until it has been remade
    }
    const displays = screen.getAllDisplays();
    this.dropWindowsNotIn(displays);
    if (displays.length === 0) return true;
    const monitors = await host.monitors();
    if (monitors.length === 0) return false; // no answer: keep the windows there are and try again
    const names = matchMonitors(displays, monitors);
    let complete = true;
    for (const d of displays) {
      const monitor = names.get(d.id);
      if (monitor === undefined) {
        complete = false;
        continue;
      }
      const b = d.bounds;
      const shape = `${monitor} ${b.x},${b.y} ${b.width}x${b.height}`;
      if (this.windows.has(d.id) && shape === this.shapes.get(d.id)) continue;
      if (!(await host.addRules([overlayRule(monitor)]))) {
        complete = false;
        continue;
      }
      if (this.closed) return true;
      const old = this.windows.get(d.id);
      if (old) old.destroy();
      this.windows.set(d.id, this.createWindow(d, overlayTitle(monitor)));
      this.shapes.set(d.id, shape);
    }
    return complete;
  }

  /** `title` is set on Linux only, where Hyprland's rule for the window matches it. */
  private createWindow(d: Display, title?: string): BrowserWindow {
    const win = new BrowserWindow({
      ...overlayBounds(d),
      ...(title === undefined ? {} : { title }),
      transparent: true,
      backgroundColor: '#00000000',
      frame: false,
      resizable: false,
      movable: false,
      minimizable: false,
      maximizable: false,
      fullscreenable: false,
      focusable: false,
      skipTaskbar: true,
      hasShadow: false,
      alwaysOnTop: true,
      enableLargerThanScreen: true, // also stops macOS pushing the window below the menu bar
      show: false,
      // A macOS panel can float over full-screen apps. On Linux, Hyprland's rule does what a type would.
      type: IS_MAC ? 'panel' : IS_LINUX ? undefined : 'toolbar',
      webPreferences: {
        preload: preloadPath('overlay'),
        backgroundThrottling: false,
        autoplayPolicy: 'no-user-gesture-required',
        spellcheck: false,
      },
    });
    win.setIgnoreMouseEvents(true);
    win.setAlwaysOnTop(true, 'screen-saver');
    if (IS_MAC) win.setVisibleOnAllWorkspaces(true, { visibleOnFullScreen: true, skipTransformProcessType: true });
    // Hyprland's rule matches the window's title, which the page's own <title> would replace.
    if (title !== undefined) win.on('page-title-updated', (event) => event.preventDefault());
    win.webContents.on('did-finish-load', () => win.webContents.send('overlay:settings', this.settings));
    win.once('ready-to-show', () => {
      win.showInactive();
      // Re-apply: mixed-DPI setups can size the first frame wrong. Wayland ignores it; Hyprland's rule sizes the window.
      if (!IS_LINUX) win.setBounds(overlayBounds(d));
    });
    void loadPage(win, 'overlay');
    return win;
  }

  private send<C extends OverlayChannel>(displayId: number, channel: C, payload: OverlayEvents[C]): void {
    const w = this.windows.get(displayId);
    if (w && !w.isDestroyed()) w.webContents.send(channel, payload);
  }
}
