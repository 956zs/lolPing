import type { Settings } from './settings';

/** Which wheel a gesture belongs to: the ping trigger's or the emote key's. */
export type WheelKind = 'ping' | 'emote';

/**
 * Messages the hook helper writes to stdout, one JSON object per line.
 * Coordinates are physical pixels on Windows, global points (Electron DIPs) on macOS and Hyprland's layout coordinates
 * (also DIPs) on Linux.
 */
export type HelperEvent =
  | { type: 'ready'; version: number }
  | { type: 'wheelOpen' | 'wheelMove' | 'wheelRelease' | 'click'; x: number; y: number; wheel: WheelKind }
  | { type: 'cancel' }
  | { type: 'toggled'; enabled: boolean }
  | { type: 'error'; message: string; code?: string }
  | { type: 'sim'; swallow: boolean; inject: string };

/**
 * `noAccess`: the helper can't run without something only the user can give it: Accessibility access on macOS, or on
 * Linux the input devices or a Hyprland session.
 */
export type HelperStatus = 'starting' | 'running' | 'failed' | 'noAccess';

/**
 * The helper's exit code when it can't read input: its event tap fails (macOS Accessibility access missing), or on Linux
 * it can't open the input devices or reach Hyprland.
 */
export const HELPER_EXIT_NO_ACCESS = 3;

/** Linux: what a helper that exited with HELPER_EXIT_NO_ACCESS is missing, the input devices or a Hyprland session. */
export type AccessProblem = 'input' | 'hyprland';

/** Reads the `code` of the error the Linux helper prints before it exits for lack of access. */
export const accessProblemOf = (code: string | undefined): AccessProblem => (code === 'noHyprland' ? 'hyprland' : 'input');

type PointType = 'wheelOpen' | 'wheelMove' | 'wheelRelease' | 'click';
const POINT_TYPES = new Set<string>(['wheelOpen', 'wheelMove', 'wheelRelease', 'click']);
const isInt = (x: unknown): x is number => Number.isInteger(x);

export function parseHelperLine(line: string): HelperEvent | null {
  let v: unknown;
  try {
    v = JSON.parse(line);
  } catch {
    return null;
  }
  if (typeof v !== 'object' || v === null || Array.isArray(v)) return null;
  const o = v as Record<string, unknown>;
  switch (o.type) {
    case 'ready':
      return isInt(o.version) ? { type: 'ready', version: o.version } : null;
    case 'cancel':
      return { type: 'cancel' };
    case 'toggled':
      return typeof o.enabled === 'boolean' ? { type: 'toggled', enabled: o.enabled } : null;
    case 'error':
      if (typeof o.message !== 'string') return null;
      return typeof o.code === 'string' ? { type: 'error', message: o.message, code: o.code } : { type: 'error', message: o.message };
    case 'sim':
      return typeof o.swallow === 'boolean' && typeof o.inject === 'string'
        ? { type: 'sim', swallow: o.swallow, inject: o.inject }
        : null;
    default:
      if (typeof o.type === 'string' && POINT_TYPES.has(o.type) && isInt(o.x) && isInt(o.y)) {
        // Anything but "emote" (including no field, from a version 1 helper) is the ping wheel.
        return { type: o.type as PointType, x: o.x, y: o.y, wheel: o.wheel === 'emote' ? 'emote' : 'ping' };
      }
      return null;
  }
}

export type HelperTrigger = 'alt' | 'ctrl' | 'shift' | 'win' | 'capslock' | 'mouse4' | 'mouse5' | 'vk';

/** Messages Electron writes to the helper's stdin. Flat objects only: the C++ parser has no nesting. */
export type HelperCommand =
  | {
      type: 'config';
      trigger: HelperTrigger;
      triggerVk: number;
      clickPing: boolean;
      /** Fullscreen apps keep every click: no wheel gestures while one is in front. */
      gameCheck: boolean;
      /** 'off': no emote key, so no emote gestures. */
      emoteTrigger: HelperTrigger | 'off';
      emoteTriggerVk: number;
      emoteClick: boolean;
      dragThresholdPx: number;
      toggleMods: number;
      toggleVk: number;
      enabled: boolean;
      /** Linux: a window that locks the pointer moves its own cursor this far per mouse count. */
      lockedSensitivity: number;
      /** Linux: the screens as "x,y,width,height;…" in layout pixels, to keep that cursor on one. */
      screens?: string;
    }
  | { type: 'setEnabled'; enabled: boolean }
  | { type: 'suspend'; on: boolean }
  | { type: 'shutdown' };

export interface ScreenRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export function configCommand(s: Settings, enabled: boolean, screens?: readonly ScreenRect[]): HelperCommand {
  const t = s.trigger;
  const e = s.emoteTrigger;
  return {
    type: 'config',
    trigger: typeof t === 'object' ? 'vk' : t,
    triggerVk: typeof t === 'object' ? t.vk : 0,
    clickPing: s.clickPing,
    gameCheck: s.gameCheck,
    emoteTrigger: typeof e === 'object' ? 'vk' : e,
    emoteTriggerVk: typeof e === 'object' ? e.vk : 0,
    emoteClick: s.emoteClick,
    dragThresholdPx: s.dragThresholdPx,
    toggleMods: s.toggleHotkey.mods,
    toggleVk: s.toggleHotkey.vk,
    enabled,
    lockedSensitivity: s.lockedPointerSensitivity,
    ...(screens ? { screens: screens.map((r) => [r.x, r.y, r.width, r.height].map(Math.round).join(',')).join(';') } : {}),
  };
}

export const serializeCommand = (c: HelperCommand): string => `${JSON.stringify(c)}\n`;
