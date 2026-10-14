import { execFile } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { createConnection, type Socket } from 'node:net';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import {
  parseConfigProvider, parseEvent, parseMonitors, ruleArgs, type ConfigProvider, type HyprMonitor, type HyprRule,
} from './hyprRules';

/** Runs hyprctl with `args`: what it printed, or null when it failed. */
export type Hyprctl = (args: readonly string[]) => Promise<string | null>;

const runHyprctl: Hyprctl = (args) => new Promise((resolve) => {
  execFile('hyprctl', args, { timeout: 2000 }, (err, stdout) => resolve(err ? null : stdout));
});

const EVENTS_RETRY_MS = 3000;
/** A config reload this soon after one of our own adds could be Hyprland answering that add. */
const OWN_RELOAD_MS = 1000;
/** After this many of those in a row, adding the rules again is a loop, so lolPing stops doing it. */
const MAX_OWN_RELOADS = 3;

export interface HyprlandOptions {
  /** From hyprSocketDir. */
  socketDir: string;
  now?: () => number;
  hyprctl?: Hyprctl;
}

/**
 * Talks to Hyprland: its monitors, window rules and events. Rules added at runtime are dropped when Hyprland reloads its
 * config, so each one is added once per config load.
 * Events: 'reloaded' after a config reload (the rules have to be added again), 'monitors' when a monitor comes or goes.
 */
export class Hyprland extends EventEmitter {
  private readonly added = new Set<string>();
  private provider: ConfigProvider | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  private lastAdd = -Infinity;
  private ownReloads = 0;
  private socket: Socket | null = null;
  private retryTimer: NodeJS.Timeout | null = null;
  private stopped = false;
  private readonly now: () => number;
  private readonly hyprctl: Hyprctl;

  constructor(private readonly opts: HyprlandOptions) {
    super();
    this.now = opts.now ?? Date.now;
    this.hyprctl = opts.hyprctl ?? runHyprctl;
  }

  /** The active monitors; none when Hyprland can't be reached. */
  async monitors(): Promise<HyprMonitor[]> {
    const out = await this.hyprctl(['-j', 'monitors']);
    if (out === null) return [];
    try {
      return parseMonitors(JSON.parse(out));
    } catch {
      return [];
    }
  }

  /** Adds the rules that weren't added since the last config load, one call at a time. True when all of them are in place. */
  addRules(rules: readonly HyprRule[]): Promise<boolean> {
    const run = this.queue.then(() => this.addMissing(rules));
    this.queue = run.catch(() => undefined);
    return run;
  }

  /** Follows Hyprland's events until stop(), connecting again whenever the socket closes. */
  watch(): void {
    if (this.stopped || this.socket || this.retryTimer) return;
    const socket = createConnection(join(this.opts.socketDir, '.socket2.sock'));
    this.socket = socket;
    createInterface({ input: socket }).on('line', (line) => this.onEvent(line));
    socket.on('error', () => undefined); // 'close' follows
    socket.on('close', () => {
      if (this.socket === socket) this.socket = null;
      if (this.stopped) return;
      this.retryTimer = setTimeout(() => {
        this.retryTimer = null;
        this.watch();
      }, EVENTS_RETRY_MS);
    });
  }

  stop(): void {
    this.stopped = true;
    if (this.retryTimer) clearTimeout(this.retryTimer);
    this.retryTimer = null;
    this.socket?.destroy();
    this.socket = null;
  }

  private async addMissing(rules: readonly HyprRule[]): Promise<boolean> {
    let ok = true;
    for (const rule of rules) {
      if (this.added.has(rule.name)) continue;
      const provider = await this.configProvider();
      const args = provider === null ? null : ruleArgs(rule, provider);
      const out = args === null ? null : await this.hyprctl(args);
      this.lastAdd = this.now();
      if (out === null) {
        console.warn('[hyprland] could not add the window rule', rule.name);
        ok = false;
        continue;
      }
      if (out.trim() !== 'ok' && out.trim() !== '') console.log('[hyprland]', rule.name, out.trim());
      this.added.add(rule.name);
    }
    return ok;
  }

  /** Asked once; null while Hyprland can't be reached, so it is asked again next time. */
  private async configProvider(): Promise<ConfigProvider | null> {
    if (this.provider) return this.provider;
    const out = await this.hyprctl(['-j', 'status']);
    if (out === null) return null;
    let raw: unknown = null;
    try {
      raw = JSON.parse(out);
    } catch {
      // a Hyprland without the status request
    }
    this.provider = parseConfigProvider(raw);
    return this.provider;
  }

  private onEvent(line: string): void {
    const ev = parseEvent(line);
    if (ev === 'monitors') {
      this.emit('monitors');
    } else if (ev === 'configreloaded') {
      this.ownReloads = this.now() - this.lastAdd < OWN_RELOAD_MS ? this.ownReloads + 1 : 0;
      if (this.ownReloads > MAX_OWN_RELOADS) {
        if (this.ownReloads === MAX_OWN_RELOADS + 1) console.warn('[hyprland] adding rules reloads the config: not adding them again');
        return;
      }
      this.added.clear();
      this.emit('reloaded');
    }
  }
}
