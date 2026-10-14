import { contextBridge, ipcRenderer } from 'electron';
import type { EmoteRef } from '../shared/emotes';
import {
  OVERLAY_ASSETS, OVERLAY_CHANNELS, OVERLAY_EMOTED, OVERLAY_PINGED, type OverlayChannel, type OverlayEvents,
} from '../shared/ipc';
import type { PingId } from '../shared/pings';
import type { Platform } from '../shared/platform';

// Not platformOf: two sandboxed preloads importing one module get a shared chunk, which a sandboxed preload can't require.
const platform: Platform = process.platform === 'darwin' ? 'mac' : process.platform === 'linux' ? 'linux' : 'win';

const api = {
  platform,
  on<C extends OverlayChannel>(channel: C, cb: (payload: OverlayEvents[C]) => void): void {
    if (!OVERLAY_CHANNELS.includes(channel)) throw new Error(`unknown overlay channel: ${channel}`);
    ipcRenderer.on(channel, (_event, payload: OverlayEvents[C]) => cb(payload));
  },
  reportMissingAssets(names: string[]): void {
    ipcRenderer.send(OVERLAY_ASSETS, names);
  },
  /** A wheel ping was placed here (CSS px in this overlay). */
  reportPing(id: PingId, x: number, y: number): void {
    ipcRenderer.send(OVERLAY_PINGED, { id, x, y });
  },
  /** A wheel emote was placed here (CSS px in this overlay). */
  reportEmote(ref: EmoteRef, x: number, y: number): void {
    ipcRenderer.send(OVERLAY_EMOTED, { ref, x, y });
  },
};

contextBridge.exposeInMainWorld('overlay', api);

export type OverlayApi = typeof api;
