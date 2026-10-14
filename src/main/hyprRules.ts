import type { Rect } from './coords';

/** A Hyprland monitor: its connector name and its rect in Hyprland's layout, which is Electron's DIP space. */
export interface HyprMonitor {
  name: string;
  rect: Rect;
}

/** A window rule written for both of Hyprland's config languages. */
export interface HyprRule {
  /** Names the rule, so it is added once per config load. */
  name: string;
  /** Code for `hyprctl eval`, with a Lua config. */
  lua: string;
  /** The value for `hyprctl keyword windowrule`, with a hyprland.conf; null when the rule can't be written there. */
  hyprlang: string | null;
}

/** Which language Hyprland's config is in, which decides how rules are added at runtime. */
export type ConfigProvider = 'lua' | 'hyprlang';

/** The events lolPing follows: a config reload drops runtime rules, and a monitor change needs new overlays. */
export type HyprEvent = 'configreloaded' | 'monitors';

const isObj = (v: unknown): v is Record<string, unknown> => typeof v === 'object' && v !== null && !Array.isArray(v);
const isNum = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);

/** Hyprland's socket folder for this session, or null outside Hyprland. */
export function hyprSocketDir(env: Readonly<Record<string, string | undefined>>): string | null {
  const runtime = env.XDG_RUNTIME_DIR;
  const signature = env.HYPRLAND_INSTANCE_SIGNATURE;
  if (!runtime || !runtime.startsWith('/') || !signature || !/^[A-Za-z0-9_]+$/.test(signature)) return null;
  return `${runtime.replace(/\/+$/, '')}/hypr/${signature}`;
}

/** The monitors in `hyprctl -j monitors` output. Disabled and mirroring monitors and malformed entries are left out. */
export function parseMonitors(raw: unknown): HyprMonitor[] {
  if (!Array.isArray(raw)) return [];
  const out: HyprMonitor[] = [];
  for (const m of raw) {
    if (!isObj(m) || typeof m.name !== 'string' || m.name === '' || m.disabled === true) continue;
    if (typeof m.mirrorOf === 'string' && m.mirrorOf !== 'none') continue;
    if (out.some((o) => o.name === m.name)) continue;
    const { x, y, width, height, scale } = m;
    if (!isNum(x) || !isNum(y) || !isNum(width) || !isNum(height) || !isNum(scale)) continue;
    if (width <= 0 || height <= 0 || scale <= 0) continue;
    // Transforms 1, 3, 5 and 7 turn the monitor a quarter, which swaps its width and height in the layout.
    const turned = Number.isInteger(m.transform) && (m.transform as number) % 2 === 1;
    const w = Math.round((turned ? height : width) / scale);
    const h = Math.round((turned ? width : height) / scale);
    out.push({ name: m.name, rect: { x, y, width: w, height: h } });
  }
  return out;
}

/** hyprctl prints the scale with two decimals, so a size worked out from it can be off by a few pixels. */
const sizeNear = (a: number, b: number): boolean => Math.abs(a - b) <= Math.max(2, b * 0.01);

/**
 * Pairs Electron displays with the Hyprland monitors at the same layout position and of about the same size.
 * Returns display id → monitor name; a display without a match is left out, and each monitor is used once.
 */
export function matchMonitors(
  displays: readonly { id: number; bounds: Rect }[],
  monitors: readonly HyprMonitor[],
): Map<number, string> {
  const out = new Map<number, string>();
  const used = new Set<string>();
  for (const d of displays) {
    const b = d.bounds;
    let best: HyprMonitor | null = null;
    let bestScore = Infinity;
    for (const m of monitors) {
      const r = m.rect;
      if (used.has(m.name) || Math.abs(r.x - b.x) > 1 || Math.abs(r.y - b.y) > 1) continue;
      if (!sizeNear(r.width, b.width) || !sizeNear(r.height, b.height)) continue;
      const score = Math.abs(r.x - b.x) + Math.abs(r.y - b.y) + Math.abs(r.width - b.width) + Math.abs(r.height - b.height);
      if (score < bestScore) {
        best = m;
        bestScore = score;
      }
    }
    if (best) {
      out.set(d.id, best.name);
      used.add(best.name);
    }
  }
  return out;
}

/** The title of the overlay window on `monitor`, which its window rule matches. */
export const overlayTitle = (monitor: string): string => `lolPing overlay ${monitor}`;

/** A Lua string literal. Control characters become three-digit escapes, so a digit after one can't join it. */
export const luaString = (s: string): string =>
  `"${s.replace(/[\\"]/g, '\\$&').replace(/[\x00-\x1f\x7f]/g, (c) => `\\${String(c.charCodeAt(0)).padStart(3, '0')}`)}"`;

const escapeRegex = (s: string): string => s.replace(/[\\^$.|?*+()[\]{}]/g, '\\$&');

/** Monitor names a hyprlang rule can hold: commas separate its fields, and # starts a comment. */
const HYPRLANG_NAME = /^[A-Za-z0-9._-]+$/;

/**
 * The rule for the overlay on `monitor`: floating and pinned on that monitor, covering it exactly, never focused, with no
 * animation, blur, shadow, border or rounding.
 */
export function overlayRule(monitor: string): HyprRule {
  const name = `lolping-overlay-${monitor}`;
  const title = `^${escapeRegex(overlayTitle(monitor))}$`;
  const lua = `hl.window_rule({ name = ${luaString(name)}, match = { title = ${luaString(title)} }, float = true, pin = true, `
    + `no_initial_focus = true, no_focus = true, monitor = ${luaString(monitor)}, move = {0, 0}, `
    + 'size = {"monitor_w", "monitor_h"}, no_anim = true, no_blur = true, no_shadow = true, border_size = 0, rounding = 0 })';
  const hyprlang = HYPRLANG_NAME.test(monitor)
    ? `match:title ${title}, float on, pin on, no_initial_focus on, no_focus on, monitor ${monitor}, move 0 0, `
      + 'size monitor_w monitor_h, no_anim on, no_blur on, no_shadow on, border_size 0, rounding 0'
    : null;
  return { name, lua, hyprlang };
}

/** The settings window's title is the window's own until its page sets "lolPing Settings". */
const SETTINGS_TITLE = '^lolPing( Settings)?$';

/** Floats the settings window in the middle of the screen instead of tiling it. */
export const SETTINGS_RULE: HyprRule = {
  name: 'lolping-settings',
  lua: `hl.window_rule({ name = "lolping-settings", match = { title = ${luaString(SETTINGS_TITLE)} }, `
    + 'float = true, center = true })',
  hyprlang: `match:title ${SETTINGS_TITLE}, float on, center on`,
};

/** The hyprctl arguments that add `rule` to a config of `provider`'s kind, or null when it can't be added there. */
export function ruleArgs(rule: HyprRule, provider: ConfigProvider): string[] | null {
  if (provider === 'lua') return ['eval', rule.lua];
  return rule.hyprlang === null ? null : ['keyword', 'windowrule', rule.hyprlang];
}

/** Reads `hyprctl -j status`. Anything else, such as the reply of a Hyprland too old to have `status`, means hyprlang. */
export const parseConfigProvider = (raw: unknown): ConfigProvider =>
  (isObj(raw) && raw.configProvider === 'lua' ? 'lua' : 'hyprlang');

/** Reads a line from Hyprland's event socket (`name>>data`). */
export function parseEvent(line: string): HyprEvent | null {
  const sep = line.indexOf('>>');
  if (sep < 0) return null;
  const name = line.slice(0, sep);
  if (name === 'configreloaded') return 'configreloaded';
  // Each of these comes again as a v2 event: following only the first rebuilds once per change.
  return name === 'monitoradded' || name === 'monitorremoved' ? 'monitors' : null;
}
