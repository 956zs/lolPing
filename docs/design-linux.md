# lolPing for Linux (Hyprland) — Design Spec

**Date:** 2026-10-14
**Status:** Proposed. A throwaway spike on Hyprland 0.56.1 checked the two risky parts: the overlay windows and the mouse grab (§8)

## 1. Purpose and constraints

Bring lolPing to Linux on the Hyprland compositor with the same behaviour as on Windows: hold a key, drag, release on a slice, and the ping appears on any display and in whole-screen capture. Rooms and emotes work unchanged.

| Topic | Decision |
|---|---|
| Target | Hyprland on Wayland, x86_64. Other compositors and X11 sessions are out of scope for now (§7). |
| Testing | The developer's own machine: Arch, Hyprland 0.56.1 with the Lua config, end4 dots. CI runs typecheck and tests on Ubuntu. |
| Codebase | One codebase, developed on the `linux` branch. |
| Default trigger | Alt + drag. Toggle Ctrl+Alt+P. |
| Trigger choices | Alt, Ctrl, Shift, Super, Mouse 4, Mouse 5. No Caps Lock and no custom key, because keyboards are only read (§2.3). |
| Permissions | The `input` group for `/dev/input/event*`, and write access to `/dev/uinput` (§2.7). |
| Updates | No update check on Linux. There are no Linux release files to update to. |
| Packaging | Run from source, or an AppImage built with `npm run dist:linux`. Not part of the release workflow. |

## 2. Hook helper

The helper stays one C++ program. `decision.cpp`, `commands`, `json`, `output`, `simulate` and `main_posix.cpp` are shared. Only the OS layer is new:

| File | Platform |
|---|---|
| `hooks_linux.cpp` | Linux: devices, grab and forward, keyboard watch, Hyprland cursor queries, watchdog |
| `linuxinput.{h,cpp}` | Pure logic used by `hooks_linux.cpp`, no Linux headers, so it is unit-tested on every platform |

`build.sh` checks `uname`. On Linux it compiles with `c++ -std=c++20`, links `libevdev` (through `pkg-config`) and `-pthread`. On macOS it does what it does today.

### 2.1 Devices

- The helper opens every `/dev/input/event*` and sorts it with libevdev:
  - Mouse: `REL_X`, `REL_Y` and `BTN_LEFT`, no `EV_ABS`.
  - Keyboard: has letter keys (`KEY_A`) or modifier keys (`KEY_LEFTALT`).
  - Anything else, touchpads included, is closed again.
- Virtual devices are skipped: those whose sysfs node (`/sys/class/input/eventN`) resolves under `/devices/virtual/`. That covers the helper's own clones, `ydotoold` and Sunshine. Checking `BUS_VIRTUAL` is not enough: Sunshine's "Mouse passthrough" reports a real bus.
- Hotplug: an inotify watch on `/dev/input` (`IN_CREATE`, and `IN_ATTRIB` because udev sets the permissions after the node appears). A read that fails with `ENODEV` drops the device.

### 2.2 Mice: grab and forward

- While lolPing is on, the helper grabs each mouse (`EVIOCGRAB`) and makes a clone with `libevdev_uinput_create_from_device`. The clone has the same name, IDs and capabilities, so libinput and Hyprland treat it like the real mouse.
- Events that aren't swallowed are written to the clone unchanged, frame by frame, `SYN_REPORT` included.
- **Swallow:** the event isn't written.
- **Replay:** Decision's button down and up are written to the clone of the mouse the gesture came from, each followed by `SYN_REPORT`.
- The clone comes first and the grab 250 ms later, once the compositor has opened the clone, so no event is lost in between.
- A mouse is grabbed only when none of its buttons is held. Until then the helper watches it without a grab, so libinput never misses a release.
- **Paused:** the helper drops its grabs and destroys the clones. A paused lolPing is not in the input path at all. During key capture (`suspend`) the grabs stay and Decision passes everything through.
- **Exit** (stdin EOF, `shutdown`, crash): the kernel drops a grab when its file closes, and the clones disappear with the process. The spike checked this.
- Buttons map as `BTN_LEFT`/`BTN_RIGHT`/`BTN_MIDDLE`, `BTN_SIDE` → X1 (Mouse 4), `BTN_EXTRA` → X2 (Mouse 5).

### 2.3 Keyboards: read only

- Keyboards are opened without a grab. The helper sees every key but can't stop one.
- Why: a grabbed keyboard would send every keystroke through lolPing, and a stalled helper would freeze typing. Windows removes a slow hook by itself; Linux has no such safety net.
- What follows from that:
  - Caps Lock and a custom key need their key swallowed, so Linux doesn't offer them. `normalizeSettings(raw, 'linux')` maps them to the default, as it maps Caps Lock on macOS.
  - The toggle hotkey works, but the focused app also receives it.
  - Esc cancels the wheel, and the app also receives it.
- A key counts as down while any keyboard holds it. `linuxinput` turns per-keyboard changes into the VK key-downs and key-ups Decision expects. Keys map by physical position (evdev code → Windows VK), as `macinput` does: Alt → `VK_LMENU`/`VK_RMENU`, Ctrl → `VK_LCONTROL`/`VK_RCONTROL`, Shift → `VK_LSHIFT`/`VK_RSHIFT`, Super → `VK_LWIN`/`VK_RWIN`.
- After `SYN_DROPPED`, libevdev resyncs the device and the helper calls `Decision::syncKeyStates` with the current key state.

### 2.4 Lone modifier taps

- Firefox and GTK apps show their menu bar on a lone Alt tap. end4 opens its overview on a lone Super tap.
- On Windows the helper injects a mask key just before the modifier's key-up. On Linux the key-up reaches the compositor before the helper reads it, so Linux calls `setMaskMenuKeys(false)`.
- Instead, as soon as a mouse down is swallowed while Alt or Super is held, the helper presses and releases a key that does nothing (`KEY_F24`) on its own uinput keyboard. The modifier is then no longer a lone tap.
- This needs checking with Firefox and with the end4 Super bind. If it causes trouble in the focused app, it is dropped and the menu bar goes into the limitations.

### 2.5 Cursor position

- evdev only reports relative motion. Screen positions come from Hyprland's socket `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock` with the request `j/cursorpos`. They are global layout coordinates, the same space as Electron's DIPs, as on macOS.
- A round trip takes 0.05–3 ms. That is too slow for the event path: in the spike one forwarded event waited 2 ms. So the work is split:
  - The event thread feeds Decision a position summed from the raw motion. Only the drag threshold uses it.
  - A cursor thread owns the socket. Decision's emits reach it in order, and it fills in real positions before writing them out:
    - Wheel open and click use the position from the press.
    - Wheel release uses a query made at the release.
    - While a gesture is open, it polls every 8 ms and sends `wheelMove` when the position changed. Decision's per-event moves are dropped.
- If `HYPRLAND_INSTANCE_SIGNATURE` is unset or the socket doesn't answer, the helper prints `{"type":"error","code":"noHyprland",…}` and exits with code 3, like a missing permission (§2.7).
- `gameCheck` has no fullscreen probe on Linux, as on macOS.

### 2.6 Watchdog

- The mouse passes through the helper, so a stalled event loop would freeze the pointer.
- A watchdog thread checks every 250 ms that the event loop is still advancing. When a grabbed mouse has unread input and the loop hasn't moved for 1 s, the watchdog calls `_exit(5)`. The kernel drops the grabs, the pointer works again, and `InputBridge` restarts the helper with its usual backoff.

### 2.7 Permission

- `/dev/input/event*` needs the `input` group.
- `/dev/uinput` needs a udev rule, for example `KERNEL=="uinput", SUBSYSTEM=="misc", TAG+="uaccess", OPTIONS+="static_node=uinput"`. Some packages, such as ydotool and Sunshine, already install one.
- When `/dev/uinput` can't be opened, or no mouse can be read and at least one input node was refused, the helper prints `{"type":"error","code":"noAccess",…}` and exits with code 3, as on macOS. With access but no mouse plugged in, it keeps running and waits for one. `InputBridge` already treats code 3 as "no access", not as a crash.
- The settings window shows a Linux permission card with the commands to run and a Try again button. It uses the error's `code` to choose between the permission text and the Hyprland text.

## 3. Electron main process

- **Platform:** `Platform` becomes `'win' | 'mac' | 'linux'`, and `platformOf('linux')` returns `'linux'`. Every `Record<Platform, …>` gets a Linux entry, so TypeScript points out each place that needs one.
- **Wayland:** on Linux with `WAYLAND_DISPLAY` set, main appends `--ozone-platform=wayland` before the app is ready. The overlays need native Wayland windows for the window rules below.
- **Helper path:** `hook-helper`, as on macOS.
- **Overlays:** a new `src/main/hyprland.ts` talks to Hyprland. The rule text and the monitor matching are pure functions in their own module, with tests.
  - There is one window per display, titled `lolPing overlay <monitor>`.
  - Before creating the windows, main adds one rule per monitor: float, pin, no initial focus, no focus, the monitor, position `0 0`, size `monitor_w monitor_h`, no animation, blur, shadow, border or rounding. A Lua config (Hyprland 0.56.1 here) takes it through `hyprctl eval 'hl.window_rule({…})'`. A hyprlang config takes `hyprctl keyword windowrule …`. The spike only checked the Lua form.
  - Electron displays match Hyprland monitors by layout rect: `x`, `y`, and `width`/`height` divided by `scale` from `hyprctl -j monitors`.
  - Wayland ignores `setBounds`, `setPosition` and `setAlwaysOnTop`. A display whose rect changes gets a new window instead of `setBounds`.
  - `setIgnoreMouseEvents(true)` works on Wayland: in the spike, clicks, scrolling and hover all reached the windows below.
  - Rules added at runtime are lost when Hyprland reloads its config. Main listens on `.socket2.sock` for `configreloaded` and adds them again. `monitoradded` and `monitorremoved` also trigger a rebuild.
  - Coordinate maps use `phys = dip` and `scale = 1`, as on macOS.
- **Settings window:** an opaque Fluent background (Mica is Windows-only) with Electron's caption buttons (`titleBarOverlay`). A Hyprland rule floats it and centres it.
- **Tray:** Electron's `Tray` is a StatusNotifierItem on Linux, and the end4 bar shows it. It uses a plain PNG; off is dimmed.
- **Startup:** `setLoginItemSettings` does nothing on Linux. "Open at login" writes or removes `~/.config/autostart/lolping.desktop` with `--hidden`. Hyprland only runs those entries when the session starts through uwsm or a similar tool, so the setting's description says so and the README shows the `exec-once` line.
- **Updates:** `updater` is `null` on Linux. There is no update card and no GitHub traffic.
- **Rooms, emotes, LAN and relays** stay as they are.

## 4. Settings UI

- `<html data-platform="linux">`: the Windows Fluent look on an opaque background.
- Labels: Ctrl, Alt, Shift, Super. Hotkeys read "Ctrl + Alt + P".
- `strings(lang, 'linux')` adds Linux wording in English and Chinese: open at login and its hint, tray wording, the permission card, "Show in file manager", and the limitations.
- The trigger picker hides Caps Lock and the custom key on Linux.

## 5. Packaging and CI

- Run from source: `npm ci`, `npm run build:helper`, `npm run dev`. This needs a C++20 compiler and the libevdev headers.
- `npm run dist:linux`: electron-builder `--linux AppImage`, with `hook-helper` as an extra resource. `release.yml` is not touched.
- CI: an `ubuntu-latest` job installs `libevdev-dev` and runs typecheck, the helper tests and the TypeScript tests.

## 6. Testing

- `helper_tests.cpp` covers `linuxinput`:
  - key code → VK
  - device kind from capability bits
  - the key-state union across keyboards
  - when the mask key is typed
  - parsing a `cursorpos` reply
  - how emits get their positions
- Vitest covers:
  - `normalizeSettings` for `'linux'` (Caps Lock and custom key → default)
  - Linux key labels and that the Linux i18n tables are complete
  - the Hyprland rule builder and monitor matching
  - the autostart file
- On Hyprland with `npm run dev`:
  - With lolPing on and off: move, click, scroll, right-click, drag and side buttons behave as without lolPing.
  - Alt+click is replayed. Alt+drag opens the wheel and the release places the ping. A right-click cancels.
  - The toggle hotkey works.
  - Two monitors, fractional scaling, and a fullscreen window.
  - After a config reload the rules come back.
  - Killing the helper gives the pointer back. Unplugging and plugging in a mouse works.
  - Firefox's menu bar and end4's Super overview after an Alt or Super gesture.

## 7. Known Linux limitations

- Hyprland only. Other Wayland compositors and X11 are not supported yet.
- Touchpads are left alone, so a touchpad drag doesn't open the wheel. Use a mouse.
- Keys are not swallowed: the toggle hotkey and Esc also reach the focused app, and Caps Lock or a custom key can't be the trigger.
- The overlay probably doesn't show over a fullscreen window: Hyprland draws fullscreen windows above floating ones. To be checked.
- The `input` group and `/dev/uinput` let a program read and send all input. That is the same access lolPing needs on every platform.
- Single-window screen sharing doesn't include the pings.

## 8. Spike results (2026-10-14)

| Check | Result |
|---|---|
| Overlay window | Electron 43, native Wayland, with a runtime `hl.window_rule`: floating, pinned, exactly the monitor's rect, not focused, transparent. |
| Click-through | `setIgnoreMouseEvents(true)`: clicks, scrolling and hover reached the windows below. The overlay page got no mouse events. |
| Display bounds | `screen.getAllDisplays()` matched Hyprland's layout (3840,0 1920×1200). `getBounds()` on a Wayland window reports 0,0. |
| Mouse grab | 14,580 events forwarded through uinput clones, 7.7 µs each on average. Alt + left press and release swallowed, the cursor kept moving, and clicks and drags were told apart. |
| Cleanup | Clones disappeared and grabs were released when the process exited. |
| Cursor IPC | `j/cursorpos` round trip 0.05–3 ms. |
