import { describe, expect, it } from 'vitest';
import {
  hyprSocketDir, luaString, matchMonitors, overlayRule, overlayTitle, parseConfigProvider, parseEvent, parseMonitors, ruleArgs,
  SETTINGS_RULE,
} from '../../src/main/hyprRules';

/** One entry of `hyprctl -j monitors`, trimmed to the fields lolPing reads plus a few it ignores. */
const monitor = (over: Record<string, unknown> = {}): Record<string, unknown> => ({
  id: 0, name: 'eDP-1', description: 'BOE 0x0A42', width: 1920, height: 1200, refreshRate: 60.003, x: 3840, y: 0,
  scale: 1.0, transform: 0, focused: true, disabled: false, mirrorOf: 'none', ...over,
});

describe('hyprRules', () => {
  describe('hyprSocketDir', () => {
    it('finds the session\'s socket folder under the runtime folder', () => {
      expect(hyprSocketDir({ XDG_RUNTIME_DIR: '/run/user/1000', HYPRLAND_INSTANCE_SIGNATURE: 'abc_123_456' }))
        .toBe('/run/user/1000/hypr/abc_123_456');
      expect(hyprSocketDir({ XDG_RUNTIME_DIR: '/run/user/1000/', HYPRLAND_INSTANCE_SIGNATURE: 'abc' })).toBe('/run/user/1000/hypr/abc');
    });

    it('is null outside Hyprland or with a signature that isn\'t a plain name', () => {
      expect(hyprSocketDir({ XDG_RUNTIME_DIR: '/run/user/1000' })).toBeNull();
      expect(hyprSocketDir({ HYPRLAND_INSTANCE_SIGNATURE: 'abc' })).toBeNull();
      expect(hyprSocketDir({})).toBeNull();
      for (const sig of ['', '../x', 'a/b', 'a b']) {
        expect(hyprSocketDir({ XDG_RUNTIME_DIR: '/run/user/1000', HYPRLAND_INSTANCE_SIGNATURE: sig }), sig).toBeNull();
      }
      expect(hyprSocketDir({ XDG_RUNTIME_DIR: 'run/user', HYPRLAND_INSTANCE_SIGNATURE: 'abc' })).toBeNull();
    });
  });

  describe('parseMonitors', () => {
    it('reads each monitor\'s name and layout rect', () => {
      expect(parseMonitors([monitor()])).toEqual([{ name: 'eDP-1', rect: { x: 3840, y: 0, width: 1920, height: 1200 } }]);
    });

    it('divides the size by the scale, as the layout does', () => {
      expect(parseMonitors([monitor({ width: 2880, height: 1800, scale: 1.5 })])[0].rect)
        .toEqual({ x: 3840, y: 0, width: 1920, height: 1200 });
      // hyprctl prints 4/3 as 1.33: the size comes out a few pixels large, and is rounded.
      expect(parseMonitors([monitor({ width: 2560, height: 1600, scale: 1.33 })])[0].rect)
        .toEqual({ x: 3840, y: 0, width: 1925, height: 1203 });
    });

    it('swaps width and height for a monitor turned a quarter', () => {
      for (const transform of [1, 3, 5, 7]) {
        expect(parseMonitors([monitor({ transform })])[0].rect, `${transform}`).toEqual({ x: 3840, y: 0, width: 1200, height: 1920 });
      }
      for (const transform of [0, 2, 4, 6, 'x', null]) {
        expect(parseMonitors([monitor({ transform })])[0].rect, `${transform}`).toEqual({ x: 3840, y: 0, width: 1920, height: 1200 });
      }
    });

    it('leaves out disabled and mirroring monitors, and a second monitor of the same name', () => {
      expect(parseMonitors([monitor({ disabled: true })])).toEqual([]);
      expect(parseMonitors([monitor({ mirrorOf: 'DP-1' })])).toEqual([]);
      expect(parseMonitors([monitor(), monitor({ x: 0 })])).toHaveLength(1);
      const { mirrorOf: _m, disabled: _d, ...older } = monitor(); // older Hyprland versions print neither field
      expect(parseMonitors([older])).toHaveLength(1);
    });

    it('skips malformed entries and reads anything but an array as no monitors', () => {
      const bad = [
        null, 'eDP-1', 5, [], monitor({ name: '' }), monitor({ name: 7 }), monitor({ x: '0' }), monitor({ y: Number.NaN }),
        monitor({ width: 0 }), monitor({ height: -1 }), monitor({ scale: 0 }), monitor({ scale: null }), monitor({ width: Infinity }),
      ];
      expect(parseMonitors(bad)).toEqual([]);
      expect(parseMonitors([...bad, monitor({ name: 'DP-2' })]).map((m) => m.name)).toEqual(['DP-2']);
      for (const raw of [null, undefined, {}, 'x', 1, { 0: monitor() }]) expect(parseMonitors(raw)).toEqual([]);
    });
  });

  describe('matchMonitors', () => {
    const display = (id: number, x: number, y: number, width: number, height: number) => ({ id, bounds: { x, y, width, height } });
    const mon = (name: string, x: number, y: number, width: number, height: number) => ({ name, rect: { x, y, width, height } });

    it('pairs each display with the monitor at its place in the layout', () => {
      const displays = [display(71, 3840, 0, 1920, 1200), display(72, 0, 0, 2560, 1440)];
      const monitors = [mon('DP-1', 0, 0, 2560, 1440), mon('eDP-1', 3840, 0, 1920, 1200)];
      expect(matchMonitors(displays, monitors)).toEqual(new Map([[71, 'eDP-1'], [72, 'DP-1']]));
    });

    it('allows the few pixels a two-decimal scale is off by, but not a different size', () => {
      expect(matchMonitors([display(1, 0, 0, 1920, 1200)], [mon('DP-1', 0, 0, 1925, 1203)])).toEqual(new Map([[1, 'DP-1']]));
      expect(matchMonitors([display(1, 0, 0, 1920, 1200)], [mon('DP-1', 1, 1, 1920, 1200)])).toEqual(new Map([[1, 'DP-1']]));
      expect(matchMonitors([display(1, 0, 0, 1920, 1200)], [mon('DP-1', 0, 0, 1280, 800)])).toEqual(new Map());
      expect(matchMonitors([display(1, 0, 0, 1920, 1200)], [mon('DP-1', 2, 0, 1920, 1200)])).toEqual(new Map());
      expect(matchMonitors([display(1, 0, 0, 100, 100)], [mon('DP-1', 0, 0, 102, 98)])).toEqual(new Map([[1, 'DP-1']]));
      expect(matchMonitors([display(1, 0, 0, 100, 100)], [mon('DP-1', 0, 0, 103, 100)])).toEqual(new Map());
    });

    it('takes the closest monitor and uses each monitor once', () => {
      const monitors = [mon('A', 0, 0, 1930, 1200), mon('B', 0, 0, 1920, 1200)];
      expect(matchMonitors([display(1, 0, 0, 1920, 1200)], monitors)).toEqual(new Map([[1, 'B']]));
      expect(matchMonitors([display(1, 0, 0, 1920, 1200), display(2, 0, 0, 1920, 1200)], [mon('B', 0, 0, 1920, 1200)]))
        .toEqual(new Map([[1, 'B']]));
    });

    it('matches nothing when either side is empty', () => {
      expect(matchMonitors([], [mon('DP-1', 0, 0, 1920, 1080)])).toEqual(new Map());
      expect(matchMonitors([display(1, 0, 0, 1920, 1080)], [])).toEqual(new Map());
    });
  });

  describe('rules', () => {
    it('writes the overlay rule the spike checked, for the monitor\'s own title', () => {
      expect(overlayTitle('eDP-1')).toBe('lolPing overlay eDP-1');
      expect(overlayRule('eDP-1')).toEqual({
        name: 'lolping-overlay-eDP-1',
        lua: 'hl.window_rule({ name = "lolping-overlay-eDP-1", match = { title = "^lolPing overlay eDP-1$" }, float = true, pin = true, '
          + 'no_initial_focus = true, no_focus = true, monitor = "eDP-1", move = {0, 0}, size = {"monitor_w", "monitor_h"}, '
          + 'no_anim = true, no_blur = true, no_shadow = true, border_size = 0, rounding = 0 })',
        hyprlang: 'match:title ^lolPing overlay eDP-1$, float on, pin on, no_initial_focus on, no_focus on, monitor eDP-1, move 0 0, '
          + 'size monitor_w monitor_h, no_anim on, no_blur on, no_shadow on, border_size 0, rounding 0',
      });
    });

    it('escapes a monitor name for the title pattern and the Lua string', () => {
      const rule = overlayRule('Odd.Mon "1"\\');
      expect(rule.lua).toContain('title = "^lolPing overlay Odd\\\\.Mon \\"1\\"\\\\\\\\$"');
      expect(rule.lua).toContain('monitor = "Odd.Mon \\"1\\"\\\\"');
      expect(rule.hyprlang).toBeNull(); // a hyprlang rule can't hold quotes, spaces or commas
      expect(overlayRule('HEADLESS-2.1').hyprlang).toContain('match:title ^lolPing overlay HEADLESS-2\\.1$,');
      expect(overlayRule('a,b').hyprlang).toBeNull();
      expect(overlayRule('a#b').hyprlang).toBeNull();
    });

    it('writes control characters in a Lua string as three-digit escapes', () => {
      expect(luaString('a\nb')).toBe('"a\\010b"');
      expect(luaString('\u00011')).toBe('"\\0011"');
      expect(luaString('')).toBe('""');
      expect(luaString('新')).toBe('"新"');
    });

    it('floats and centres the settings window under either of its titles', () => {
      expect(SETTINGS_RULE.lua).toBe(
        'hl.window_rule({ name = "lolping-settings", match = { title = "^lolPing( Settings)?$" }, float = true, center = true })',
      );
      expect(SETTINGS_RULE.hyprlang).toBe('match:title ^lolPing( Settings)?$, float on, center on');
      const title = /^lolPing( Settings)?$/;
      expect(['lolPing', 'lolPing Settings'].every((t) => title.test(t))).toBe(true);
      expect(['lolPing overlay eDP-1', 'lolPing - GitHub'].some((t) => title.test(t))).toBe(false);
    });

    it('adds a rule with eval on a Lua config and with keyword on a hyprlang one', () => {
      const rule = overlayRule('DP-1');
      expect(ruleArgs(rule, 'lua')).toEqual(['eval', rule.lua]);
      expect(ruleArgs(rule, 'hyprlang')).toEqual(['keyword', 'windowrule', rule.hyprlang]);
      const odd = overlayRule('a,b');
      expect(ruleArgs(odd, 'lua')).toEqual(['eval', odd.lua]);
      expect(ruleArgs(odd, 'hyprlang')).toBeNull();
    });
  });

  describe('parseConfigProvider', () => {
    it('reads the config language from hyprctl status', () => {
      expect(parseConfigProvider({ configProvider: 'lua', backend: 'drm' })).toBe('lua');
      expect(parseConfigProvider({ configProvider: 'hyprlang' })).toBe('hyprlang');
    });

    it('takes anything else for hyprlang, such as an older Hyprland\'s reply', () => {
      for (const raw of [null, undefined, 'unknown request', {}, { configProvider: 'LUA' }, ['lua']]) {
        expect(parseConfigProvider(raw)).toBe('hyprlang');
      }
    });
  });

  describe('parseEvent', () => {
    it('reports config reloads and monitor changes', () => {
      expect(parseEvent('configreloaded>>')).toBe('configreloaded');
      expect(parseEvent('monitoradded>>DP-2')).toBe('monitors');
      expect(parseEvent('monitorremoved>>DP-2')).toBe('monitors');
    });

    it('ignores the v2 copies of monitor events and everything else', () => {
      for (const line of ['monitoraddedv2>>1,DP-2,desc', 'monitorremovedv2>>1,DP-2,desc', 'activewindow>>kitty,~', 'workspace>>2',
        '', 'configreloaded', '>>configreloaded', 'xconfigreloaded>>']) {
        expect(parseEvent(line), line).toBeNull();
      }
    });
  });
});
