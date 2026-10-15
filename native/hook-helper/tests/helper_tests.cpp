// Unit tests for the hook helper's pure logic (no OS hooks).
// Build and run: npm run test:helper
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "../src/commands.h"
#include "../src/decision.h"
#include "../src/json.h"
#include "../src/linuxinput.h"
#include "../src/macinput.h"
#include "../src/output.h"

using namespace lp;

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (cond) {                                                   \
      ++g_pass;                                                   \
    } else {                                                      \
      ++g_fail;                                                   \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

namespace {
constexpr uint32_t ALT = 0xA4, CTRL = 0xA2, KEY_P = 0x50, ESC = 0x1B, CAPS = 0x14, KEY_V = 0x56, KEY_T = 0x54, TAB = 0x09;
using S = Decision::State;

InputEvent key(EvKind k, uint32_t vk) {
  InputEvent e;
  e.kind = k;
  e.vk = vk;
  return e;
}
InputEvent kd(uint32_t vk) { return key(EvKind::KeyDown, vk); }
InputEvent ku(uint32_t vk) { return key(EvKind::KeyUp, vk); }
InputEvent mouse(EvKind k, Btn b, int x, int y, uint64_t t) {
  InputEvent e;
  e.kind = k;
  e.btn = b;
  e.x = x;
  e.y = y;
  e.timeMs = t;
  return e;
}
InputEvent md(Btn b, int x, int y, uint64_t t = 0) { return mouse(EvKind::MouseDown, b, x, y, t); }
InputEvent mu(Btn b, int x, int y, uint64_t t = 0) { return mouse(EvKind::MouseUp, b, x, y, t); }
InputEvent mv(int x, int y, uint64_t t = 0) { return mouse(EvKind::MouseMove, Btn::None, x, y, t); }

const Emit* findEmit(const Result& r, Emit::Kind k) {
  for (const auto& e : r.emits) {
    if (e.kind == k) return &e;
  }
  return nullptr;
}
bool injectIs(const Inject& i, InjectKind k, Btn b, uint32_t vk = 0) { return i.kind == k && i.btn == b && i.vk == vk; }
Decision withTrigger(Trigger t, uint32_t vk = 0, bool clickPing = false) {
  Config c;
  c.trigger = t;
  c.triggerVk = vk;
  c.clickPing = clickPing;
  Decision d;
  d.setConfig(c);
  return d;
}
// Pings on Alt (the default), emotes on `emote`.
Decision withEmote(Trigger emote = Trigger::Ctrl, uint32_t vk = 0, bool emoteClick = false) {
  Config c;
  c.emoteTrigger = emote;
  c.emoteTriggerVk = vk;
  c.emoteClick = emoteClick;
  Decision d;
  d.setConfig(c);
  return d;
}
// Alt held, left pressed at (100,100), dragged to (130,100): the wheel is open.
void openWheel(Decision& d) {
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 100, 100));
  d.onEvent(mv(130, 100));
}
// The same drag with Ctrl: the emote wheel is open (with withEmote()).
void openEmoteWheel(Decision& d) {
  d.onEvent(kd(CTRL));
  d.onEvent(md(Btn::Left, 100, 100));
  d.onEvent(mv(130, 100));
}
bool emitIs(const Emit* e, WheelKind w) { return e != nullptr && e->wheel == w; }
}  // namespace

static void test_alt_drag_opens_wheel_and_releases() {
  Decision d;
  CHECK(!d.onEvent(kd(ALT)).swallow);
  Result r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(r.swallow);
  CHECK(d.state() == S::Pending);
  r = d.onEvent(mv(104, 104));  // 5.7 px, below the 8 px threshold
  CHECK(!r.swallow && r.emits.empty());
  r = d.onEvent(mv(120, 100));
  CHECK(!r.swallow);
  CHECK(d.state() == S::Wheel);
  CHECK(r.emits.size() == 2);
  CHECK(r.emits[0].kind == Emit::Kind::WheelOpen && r.emits[0].x == 100 && r.emits[0].y == 100);
  CHECK(r.emits[1].kind == Emit::Kind::WheelMove && r.emits[1].x == 120);
  r = d.onEvent(mv(125, 90));
  CHECK(findEmit(r, Emit::Kind::WheelMove) != nullptr);
  r = d.onEvent(mu(Btn::Left, 130, 90));
  CHECK(r.swallow);
  const Emit* rel = findEmit(r, Emit::Kind::WheelRelease);
  CHECK(rel != nullptr && rel->x == 130 && rel->y == 90);
  CHECK(d.state() == S::Idle);
}

static void test_plain_click_passes_through() {
  Decision d;
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);
  CHECK(!d.onEvent(mu(Btn::Left, 1, 1)).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_alt_click_replays_when_click_ping_off() {
  Decision d;
  d.onEvent(kd(ALT));
  CHECK(d.onEvent(md(Btn::Left, 50, 60)).swallow);
  d.onEvent(mv(53, 62));  // tiny jitter, still a click
  Result r = d.onEvent(mu(Btn::Left, 53, 62));
  CHECK(r.swallow);
  CHECK(r.emits.empty());
  CHECK(r.injects.size() == 2);
  CHECK(injectIs(r.injects[0], InjectKind::ButtonDown, Btn::Left));
  CHECK(injectIs(r.injects[1], InjectKind::ButtonUp, Btn::Left));
  CHECK(d.state() == S::Idle);
}

static void test_alt_click_pings_when_click_ping_on() {
  Decision d = withTrigger(Trigger::Alt, 0, true);
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 50, 60));
  Result r = d.onEvent(mu(Btn::Left, 51, 60));
  CHECK(r.swallow);
  CHECK(r.injects.empty());
  const Emit* c = findEmit(r, Emit::Kind::Click);
  CHECK(c != nullptr && c->x == 50 && c->y == 60);
}

static void test_self_injected_events_are_ignored() {
  Decision d;
  d.onEvent(kd(ALT));
  InputEvent e = md(Btn::Left, 5, 5);
  e.selfInjected = true;
  CHECK(!d.onEvent(e).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_right_click_cancels_wheel() {
  Decision d;
  openWheel(d);
  Result r = d.onEvent(md(Btn::Right, 130, 100));
  CHECK(r.swallow);
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.onEvent(mu(Btn::Right, 130, 100)).swallow);
  r = d.onEvent(mu(Btn::Left, 130, 100));
  CHECK(r.swallow);
  CHECK(findEmit(r, Emit::Kind::WheelRelease) == nullptr);
  CHECK(d.state() == S::Idle);
}

static void test_right_click_with_alt_but_no_gesture_passes() {
  Decision d;
  d.onEvent(kd(ALT));
  CHECK(!d.onEvent(md(Btn::Right, 1, 1)).swallow);
  CHECK(!d.onEvent(mu(Btn::Right, 1, 1)).swallow);
}

static void test_escape_cancels_wheel() {
  Decision d;
  openWheel(d);
  Result r = d.onEvent(kd(ESC));
  CHECK(r.swallow);
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.onEvent(ku(ESC)).swallow);
  r = d.onEvent(mu(Btn::Left, 130, 100));
  CHECK(r.swallow && r.emits.empty());
}

// As in League, once the wheel is open only the mouse button decides: letting go of the trigger first
// keeps the wheel (and a trackpad's three-finger drag, whose button-up comes late, still pings).
static void test_trigger_released_during_wheel_keeps_wheel() {
  Decision d;
  openWheel(d);
  Result r = d.onEvent(ku(ALT));
  CHECK(r.emits.empty());
  CHECK(d.state() == S::Wheel);
  CHECK(r.swallow && r.injects.size() == 3);  // the Alt-up is still masked
  r = d.onEvent(mv(140, 90));
  CHECK(findEmit(r, Emit::Kind::WheelMove) != nullptr);
  r = d.onEvent(mu(Btn::Left, 140, 90));
  CHECK(r.swallow);
  const Emit* rel = findEmit(r, Emit::Kind::WheelRelease);
  CHECK(rel != nullptr && rel->x == 140 && rel->y == 90);
  CHECK(d.state() == S::Idle);
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);  // no trigger held: clicks pass again

  Decision e;
  openWheel(e);
  e.onEvent(ku(ALT));
  r = e.onEvent(kd(ESC));  // Esc still cancels
  CHECK(r.swallow && findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(e.onEvent(mu(Btn::Left, 130, 100)).swallow);
  CHECK(e.state() == S::Idle);
}

static void test_trigger_released_during_pending_replays_click_after_mask() {
  Decision d;
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 100, 100));
  Result r = d.onEvent(ku(ALT));
  CHECK(r.swallow);  // the Alt-up is swallowed and re-injected after the mask key
  CHECK(r.injects.size() == 4);
  CHECK(injectIs(r.injects[0], InjectKind::KeyDown, Btn::None, kMaskVk));
  CHECK(injectIs(r.injects[1], InjectKind::KeyUp, Btn::None, kMaskVk));
  CHECK(injectIs(r.injects[2], InjectKind::KeyUp, Btn::None, ALT));
  CHECK(injectIs(r.injects[3], InjectKind::ButtonDown, Btn::Left));
  CHECK(d.state() == S::Idle);
  CHECK(!d.onEvent(mu(Btn::Left, 100, 100)).swallow);  // the real up completes the replayed click
}

static void test_trigger_released_during_pending_pings_when_click_ping_on() {
  Decision d = withTrigger(Trigger::Alt, 0, true);
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 100, 100));
  Result r = d.onEvent(ku(ALT));
  const Emit* c = findEmit(r, Emit::Kind::Click);
  CHECK(c != nullptr && c->x == 100);
  CHECK(d.onEvent(mu(Btn::Left, 100, 100)).swallow);
}

// Alt+Tab belongs to the task switcher: clicks pass through untouched while the chord lasts,
// and after Alt is released Alt+click gestures again.
static void test_alt_tab_clicks_pass_through() {
  Decision d;
  CHECK(!d.onEvent(kd(ALT)).swallow);
  CHECK(!d.onEvent(kd(TAB)).swallow);  // the switcher's key marks the chord as foreign
  CHECK(!d.onEvent(ku(TAB)).swallow);
  Result r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(!r.swallow);  // the click belongs to the switcher, not to a wheel
  CHECK(!findEmit(r, Emit::Kind::Click));
  CHECK(!d.onEvent(mu(Btn::Left, 100, 100)).swallow);
  r = d.onEvent(ku(ALT));  // nothing was swallowed, so the Alt-up needs no mask
  CHECK(!r.swallow);
  CHECK(r.injects.empty());
  d.onEvent(kd(ALT));  // the chord is over: Alt+click gestures again
  r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(r.swallow);
  r = d.onEvent(mu(Btn::Left, 100, 100));
  CHECK(r.swallow);
  CHECK(r.injects.size() == 2);  // clickPing off: the click is replayed
}

// Same rule with clickPing on: Alt+Tab + click must NOT place a ping.
static void test_alt_tab_click_places_no_ping_even_with_click_ping_on() {
  Decision d = withTrigger(Trigger::Alt, 0, true);
  d.onEvent(kd(ALT));
  d.onEvent(kd(TAB));
  Result r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(!r.swallow);
  CHECK(!findEmit(r, Emit::Kind::Click));
  CHECK(!d.onEvent(mu(Btn::Left, 100, 100)).swallow);
  r = d.onEvent(ku(ALT));
  CHECK(!r.swallow);
  CHECK(r.injects.empty());
}

// Win+Tab (task view) under the Win trigger; a stale foreign flag is cleared by key re-sync.
static void test_win_tab_clicks_pass_through_and_sync_clears_flag() {
  Decision d = withTrigger(Trigger::Win);
  d.onEvent(kd(0x5B));  // LWin
  d.onEvent(kd(TAB));
  Result r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(!r.swallow);
  CHECK(!d.onEvent(mu(Btn::Left, 100, 100)).swallow);
  d.syncKeyStates([](uint32_t) { return false; });  // the OS reports every key up (Win+L, UAC...)
  d.onEvent(kd(0x5B));                              // LWin again
  r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(r.swallow);  // gestures again
}

// A fullscreen game owns the screen: trigger+click passes through while the probe says fullscreen,
// and gestures come back on the desktop. (clickPing on: the in-game drop-click case.)
static void test_fullscreen_game_keeps_the_click() {
  Config c;
  c.trigger = Trigger::Alt;
  c.clickPing = true;
  c.gameCheck = true;
  Decision d;
  d.setConfig(c);
  bool fullscreen = true;
  d.setFullscreenProbe([&fullscreen](int, int) { return fullscreen; });
  d.onEvent(kd(ALT));
  Result r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(!r.swallow);  // the game keeps the click
  CHECK(!findEmit(r, Emit::Kind::Click));
  CHECK(!d.onEvent(mu(Btn::Left, 100, 100)).swallow);
  fullscreen = false;  // back on the desktop: gestures again
  r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(r.swallow);
  r = d.onEvent(mu(Btn::Left, 100, 100));
  CHECK(r.swallow);
  CHECK(findEmit(r, Emit::Kind::Click) != nullptr);
  CHECK(d.state() == S::Idle);
}

// gameCheck off: the probe is never consulted, fullscreen or not.
static void test_fullscreen_probe_ignored_when_disabled() {
  Config c;
  c.trigger = Trigger::Alt;
  c.clickPing = true;
  c.gameCheck = false;
  Decision d;
  d.setConfig(c);
  bool consulted = false;
  d.setFullscreenProbe([&consulted](int, int) { consulted = true; return true; });
  d.onEvent(kd(ALT));
  Result r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(r.swallow);  // the gesture starts despite "fullscreen"
  CHECK(!consulted);
  r = d.onEvent(mu(Btn::Left, 100, 100));
  CHECK(r.swallow);
  CHECK(findEmit(r, Emit::Kind::Click) != nullptr);
}

static void test_alt_up_after_gesture_is_masked() {
  Decision d;
  openWheel(d);
  d.onEvent(mu(Btn::Left, 130, 100));
  Result r = d.onEvent(ku(ALT));
  CHECK(r.swallow);
  CHECK(r.injects.size() == 3);
  CHECK(injectIs(r.injects[0], InjectKind::KeyDown, Btn::None, kMaskVk));
  CHECK(injectIs(r.injects[1], InjectKind::KeyUp, Btn::None, kMaskVk));
  CHECK(injectIs(r.injects[2], InjectKind::KeyUp, Btn::None, ALT));
}

static void test_plain_alt_tap_is_not_masked() {
  Decision d;
  d.onEvent(kd(ALT));
  Result r = d.onEvent(ku(ALT));
  CHECK(!r.swallow && r.injects.empty());
}

static void test_toggle_hotkey_disables_and_enables() {
  Decision d;
  d.onEvent(kd(CTRL));
  d.onEvent(kd(ALT));
  Result r = d.onEvent(kd(KEY_P));
  CHECK(r.swallow);
  const Emit* t = findEmit(r, Emit::Kind::Toggled);
  CHECK(t != nullptr && !t->enabled);
  CHECK(!d.enabled());
  CHECK(d.onEvent(ku(KEY_P)).swallow);
  d.onEvent(ku(CTRL));
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);  // disabled: Alt+press passes
  d.onEvent(mu(Btn::Left, 1, 1));
  d.onEvent(kd(CTRL));
  r = d.onEvent(kd(KEY_P));
  t = findEmit(r, Emit::Kind::Toggled);
  CHECK(t != nullptr && t->enabled);
  CHECK(d.enabled());
}

static void test_hotkey_autorepeat_toggles_once() {
  Decision d;
  d.onEvent(kd(CTRL));
  d.onEvent(kd(ALT));
  CHECK(findEmit(d.onEvent(kd(KEY_P)), Emit::Kind::Toggled) != nullptr);
  Result repeat = d.onEvent(kd(KEY_P));
  CHECK(repeat.swallow);
  CHECK(findEmit(repeat, Emit::Kind::Toggled) == nullptr);
  CHECK(!d.enabled());
}

static void test_hotkey_ignored_while_suspended() {
  Decision d;
  d.setSuspended(true);
  d.onEvent(kd(CTRL));
  d.onEvent(kd(ALT));
  Result r = d.onEvent(kd(KEY_P));
  CHECK(!r.swallow && r.emits.empty());
  CHECK(d.enabled());
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);  // suspended: no gestures either
}

static void test_disabling_mid_drag_cancels_and_swallows_up() {
  Decision d;
  openWheel(d);
  Result r = d.setEnabled(false);
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.state() == S::SwallowUp);
  CHECK(d.onEvent(mu(Btn::Left, 130, 100)).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_config_change_mid_drag_cancels() {
  Decision d;
  openWheel(d);
  Result r = d.setConfig(Config{});
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.state() == S::SwallowUp);
}

static void test_lost_up_recovers_on_next_press() {
  Decision d;
  openWheel(d);
  d.setEnabled(false);  // -> SwallowUp, waiting for an up that never comes
  d.setEnabled(true);
  Result r = d.onEvent(md(Btn::Left, 5, 5));  // Alt still held
  CHECK(r.swallow);
  CHECK(d.state() == S::Pending);
}

static void test_lost_right_up_does_not_swallow_next_click() {
  Decision d;
  openWheel(d);
  CHECK(d.onEvent(md(Btn::Right, 130, 100)).swallow);  // cancels the wheel, arms the right-up swallow
  // The right-up never arrives (secure desktop, Win+L, UAC).
  d.onEvent(mu(Btn::Left, 130, 100));
  CHECK(d.state() == S::Idle);
  CHECK(!d.onEvent(md(Btn::Right, 5, 5)).swallow);  // a new right-down means the old up is gone
  CHECK(!d.onEvent(mu(Btn::Right, 5, 5)).swallow);
}

static void test_lost_key_up_does_not_swallow_next_press() {
  Decision d;
  openWheel(d);
  CHECK(d.onEvent(kd(ESC)).swallow);  // cancels the wheel, arms the Esc-up swallow
  // The Esc-up never arrives.
  d.onEvent(mu(Btn::Left, 130, 100));
  d.onEvent(ku(ALT));
  CHECK(d.state() == S::Idle);
  CHECK(!d.onEvent(kd(ESC)).swallow);  // a later press that passes clears the stale entry
  CHECK(!d.onEvent(ku(ESC)).swallow);
}

static void test_button_held_before_trigger_passes_up() {
  Decision d;
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);
  d.onEvent(kd(ALT));
  CHECK(!d.onEvent(mv(50, 50)).swallow);
  CHECK(!d.onEvent(mu(Btn::Left, 50, 50)).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_watchdog_cancels_stale_gesture() {
  Decision d;
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 100, 100, 1000));
  d.onEvent(mv(130, 100, 1100));
  CHECK(d.tick(5000).emits.empty());
  Result r = d.tick(1100 + kWatchdogMs + 1);
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.state() == S::SwallowUp);  // the paired up must still be swallowed
  CHECK(d.onEvent(mu(Btn::Left, 130, 100)).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_watchdog_from_pending_swallows_later_up() {
  Decision d;
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 100, 100, 1000));  // never moves past the threshold
  CHECK(d.state() == S::Pending);
  CHECK(d.tick(1000 + kWatchdogMs).emits.empty());  // exactly at the limit: not yet stale
  Result r = d.tick(1000 + kWatchdogMs + 1);
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.state() == S::SwallowUp);
  r = d.onEvent(mu(Btn::Left, 100, 100));
  CHECK(r.swallow && r.emits.empty() && r.injects.empty());
  CHECK(d.state() == S::Idle);
}

static void test_sync_clears_stale_trigger() {
  Decision d;
  d.onEvent(kd(ALT));  // the matching Alt-up is never seen (Win+L, UAC, hook reinstall...)
  d.syncKeyStates([](uint32_t) { return false; });
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_sync_keeps_keys_that_are_still_down() {
  Decision d;
  d.onEvent(kd(ALT));
  d.syncKeyStates([](uint32_t vk) { return vk == ALT; });
  CHECK(d.onEvent(md(Btn::Left, 1, 1)).swallow);
  CHECK(d.state() == S::Pending);
}

static void test_sync_never_sets_keys() {
  Decision d;
  d.syncKeyStates([](uint32_t) { return true; });  // OS says everything is down; we never saw a key-down
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_sync_keeps_swallowed_trigger_key() {
  Decision d = withTrigger(Trigger::CapsLock);
  CHECK(d.onEvent(kd(CAPS)).swallow);
  d.syncKeyStates([](uint32_t) { return false; });  // OS never saw Caps go down
  CHECK(d.onEvent(md(Btn::Left, 0, 0)).swallow);
  CHECK(d.state() == S::Pending);
}

static void test_ctrl_trigger_ignores_alt() {
  Decision d = withTrigger(Trigger::Ctrl);
  d.onEvent(kd(ALT));
  CHECK(!d.onEvent(md(Btn::Left, 1, 1)).swallow);
  d.onEvent(mu(Btn::Left, 1, 1));
  d.onEvent(ku(ALT));
  d.onEvent(kd(CTRL));
  CHECK(d.onEvent(md(Btn::Left, 1, 1)).swallow);
}

static void test_capslock_trigger_swallows_key() {
  Decision d = withTrigger(Trigger::CapsLock);
  CHECK(d.onEvent(kd(CAPS)).swallow);
  CHECK(d.onEvent(md(Btn::Left, 0, 0)).swallow);
  CHECK(findEmit(d.onEvent(mv(0, 40)), Emit::Kind::WheelOpen) != nullptr);
  CHECK(findEmit(d.onEvent(mu(Btn::Left, 0, 40)), Emit::Kind::WheelRelease) != nullptr);
  Result r = d.onEvent(ku(CAPS));
  CHECK(r.swallow && r.injects.empty());
}

static void test_custom_key_trigger() {
  Decision d = withTrigger(Trigger::CustomVk, KEY_V);
  CHECK(d.onEvent(kd(KEY_V)).swallow);
  CHECK(d.onEvent(md(Btn::Left, 0, 0)).swallow);
  CHECK(d.onEvent(ku(KEY_V)).swallow);
  d.setEnabled(false);
  CHECK(!d.onEvent(kd(KEY_V)).swallow);  // disabled: typing V works again
}

// Tap-through: a lone tap of a custom trigger key types it, replayed on release.
static void test_custom_key_tap_types_the_key() {
  constexpr uint32_t KEY_G = 0x47;
  Decision d = withTrigger(Trigger::CustomVk, KEY_G);
  Result r = d.onEvent(kd(KEY_G));
  CHECK(r.swallow && r.injects.empty());  // held back until we know what it is for
  CHECK(d.onEvent(kd(KEY_G)).swallow);    // auto-repeat while held back: still waiting
  r = d.onEvent(ku(KEY_G));
  CHECK(r.swallow && r.injects.size() == 2);
  CHECK(injectIs(r.injects[0], InjectKind::KeyDown, Btn::None, KEY_G));
  CHECK(injectIs(r.injects[1], InjectKind::KeyUp, Btn::None, KEY_G));
  CHECK(d.state() == S::Idle);
}

// Tap-through: typing "gh" fast (H down before G up) keeps the order: G is flushed, then H is replayed behind it.
static void test_custom_key_rollover_keeps_typed_order() {
  constexpr uint32_t KEY_G = 0x47, KEY_H = 0x48;
  Decision d = withTrigger(Trigger::CustomVk, KEY_G);
  CHECK(d.onEvent(kd(KEY_G)).swallow);
  Result r = d.onEvent(kd(KEY_H));
  CHECK(r.swallow && r.injects.size() == 2);
  CHECK(injectIs(r.injects[0], InjectKind::KeyDown, Btn::None, KEY_G));
  CHECK(injectIs(r.injects[1], InjectKind::KeyDown, Btn::None, KEY_H));
  CHECK(!d.onEvent(kd(KEY_G)).swallow);  // flushed: G's auto-repeat types
  r = d.onEvent(ku(KEY_G));
  CHECK(!r.swallow && r.injects.empty());  // the OS saw G down, so it must see the real up
  CHECK(!d.onEvent(ku(KEY_H)).swallow);
  // While G was typing, a click is not a gesture: it belongs to the text the user is in.
  CHECK(d.onEvent(kd(KEY_G)).swallow);  // a fresh press is held back again
  d.onEvent(kd(KEY_H));
  CHECK(!d.onEvent(md(Btn::Left, 0, 0)).swallow);
  d.onEvent(mu(Btn::Left, 0, 0));
  d.onEvent(ku(KEY_H));
  CHECK(!d.onEvent(ku(KEY_G)).swallow);
}

// Tap-through: Shift pressed while G is held sends g first, so the earlier letter isn't capitalised.
static void test_custom_key_flushes_before_modifier() {
  constexpr uint32_t KEY_G = 0x47, LSHIFT = 0xA0;
  Decision d = withTrigger(Trigger::CustomVk, KEY_G);
  d.onEvent(kd(KEY_G));
  Result r = d.onEvent(kd(LSHIFT));
  CHECK(r.swallow && r.injects.size() == 2);
  CHECK(injectIs(r.injects[0], InjectKind::KeyDown, Btn::None, KEY_G));
  CHECK(injectIs(r.injects[1], InjectKind::KeyDown, Btn::None, LSHIFT));
  CHECK(!d.onEvent(ku(KEY_G)).swallow);
}

// Tap-through: a drag consumes the held-back key, so nothing is typed on release.
static void test_custom_key_drag_types_nothing() {
  constexpr uint32_t KEY_G = 0x47;
  Decision d = withTrigger(Trigger::CustomVk, KEY_G);
  d.onEvent(kd(KEY_G));
  CHECK(d.onEvent(md(Btn::Left, 100, 100)).swallow);
  CHECK(findEmit(d.onEvent(mv(140, 100)), Emit::Kind::WheelOpen) != nullptr);
  CHECK(findEmit(d.onEvent(mu(Btn::Left, 140, 100)), Emit::Kind::WheelRelease) != nullptr);
  Result r = d.onEvent(ku(KEY_G));
  CHECK(r.swallow && r.injects.empty());
}

// Caps Lock is not held back: a replayed tap would toggle it.
static void test_caps_lock_is_not_tapped_through() {
  Decision d = withTrigger(Trigger::CapsLock);
  CHECK(d.onEvent(kd(CAPS)).swallow);
  Result r = d.onEvent(ku(CAPS));
  CHECK(r.swallow && r.injects.empty());
}

static void test_mouse4_trigger_drags_with_side_button() {
  Decision d = withTrigger(Trigger::Mouse4);
  CHECK(!d.onEvent(md(Btn::Left, 0, 0)).swallow);
  d.onEvent(mu(Btn::Left, 0, 0));
  CHECK(d.onEvent(md(Btn::X1, 10, 10)).swallow);
  Result r = d.onEvent(mu(Btn::X1, 10, 10));  // click without drag: replay so browser Back still works
  CHECK(r.swallow && r.injects.size() == 2);
  CHECK(injectIs(r.injects[0], InjectKind::ButtonDown, Btn::X1));
  CHECK(injectIs(r.injects[1], InjectKind::ButtonUp, Btn::X1));
  d.onEvent(md(Btn::X1, 10, 10));
  CHECK(findEmit(d.onEvent(mv(40, 10)), Emit::Kind::WheelOpen) != nullptr);
  CHECK(findEmit(d.onEvent(mu(Btn::X1, 40, 10)), Emit::Kind::WheelRelease) != nullptr);
}

static void test_ctrl_drag_opens_emote_wheel() {
  Decision d = withEmote();
  CHECK(!d.onEvent(kd(CTRL)).swallow);
  CHECK(d.onEvent(md(Btn::Left, 100, 100)).swallow);
  CHECK(d.state() == S::Pending);
  Result r = d.onEvent(mv(130, 100));
  const Emit* open = findEmit(r, Emit::Kind::WheelOpen);
  CHECK(emitIs(open, WheelKind::Emote) && open->x == 100 && open->y == 100);
  CHECK(emitIs(findEmit(r, Emit::Kind::WheelMove), WheelKind::Emote));
  CHECK(emitIs(findEmit(d.onEvent(mv(135, 90)), Emit::Kind::WheelMove), WheelKind::Emote));
  r = d.onEvent(mu(Btn::Left, 140, 90));
  CHECK(r.swallow);
  const Emit* rel = findEmit(r, Emit::Kind::WheelRelease);
  CHECK(emitIs(rel, WheelKind::Emote) && rel->x == 140 && rel->y == 90);
  CHECK(d.state() == S::Idle);
}

static void test_alt_drag_is_still_the_ping_wheel() {
  Decision d = withEmote();
  CHECK(!d.onEvent(kd(ALT)).swallow);
  CHECK(d.onEvent(md(Btn::Left, 100, 100)).swallow);
  Result r = d.onEvent(mv(130, 100));
  CHECK(emitIs(findEmit(r, Emit::Kind::WheelOpen), WheelKind::Ping));
  CHECK(emitIs(findEmit(r, Emit::Kind::WheelMove), WheelKind::Ping));
  r = d.onEvent(mu(Btn::Left, 130, 100));
  CHECK(r.swallow);
  CHECK(emitIs(findEmit(r, Emit::Kind::WheelRelease), WheelKind::Ping));
  CHECK(d.state() == S::Idle);
}

static void test_both_triggers_held_pass_through() {
  Decision d = withEmote();
  d.onEvent(kd(ALT));
  d.onEvent(kd(CTRL));
  Result r = d.onEvent(md(Btn::Left, 100, 100));
  CHECK(!r.swallow && r.emits.empty() && r.injects.empty());
  CHECK(d.state() == S::Idle);
  r = d.onEvent(mv(150, 100));
  CHECK(!r.swallow && r.emits.empty());
  r = d.onEvent(mu(Btn::Left, 150, 100));
  CHECK(!r.swallow && r.emits.empty() && r.injects.empty());
  r = d.onEvent(ku(ALT));  // nothing was swallowed, so the Alt-up needs no mask
  CHECK(!r.swallow && r.injects.empty());
}

static void test_emote_click_on_and_off() {
  {
    Decision d = withEmote(Trigger::Ctrl, 0, true);
    d.onEvent(kd(CTRL));
    CHECK(d.onEvent(md(Btn::Left, 50, 60)).swallow);
    Result r = d.onEvent(mu(Btn::Left, 51, 60));
    CHECK(r.swallow && r.injects.empty());
    const Emit* c = findEmit(r, Emit::Kind::Click);
    CHECK(emitIs(c, WheelKind::Emote) && c->x == 50 && c->y == 60);
  }
  {
    Decision d = withEmote();
    d.onEvent(kd(CTRL));
    CHECK(d.onEvent(md(Btn::Left, 50, 60)).swallow);
    Result r = d.onEvent(mu(Btn::Left, 50, 60));
    CHECK(r.swallow && r.emits.empty());
    CHECK(r.injects.size() == 2);
    CHECK(injectIs(r.injects[0], InjectKind::ButtonDown, Btn::Left));
    CHECK(injectIs(r.injects[1], InjectKind::ButtonUp, Btn::Left));
  }
  {
    // Each wheel follows its own click setting: click pings on, emote click off.
    Config c;
    c.clickPing = true;
    c.emoteTrigger = Trigger::Ctrl;
    Decision d;
    d.setConfig(c);
    d.onEvent(kd(CTRL));
    d.onEvent(md(Btn::Left, 50, 60));
    Result r = d.onEvent(mu(Btn::Left, 50, 60));
    CHECK(r.emits.empty() && r.injects.size() == 2);
    d.onEvent(ku(CTRL));
    d.onEvent(kd(ALT));
    d.onEvent(md(Btn::Left, 50, 60));
    r = d.onEvent(mu(Btn::Left, 50, 60));
    CHECK(emitIs(findEmit(r, Emit::Kind::Click), WheelKind::Ping) && r.injects.empty());
  }
  {
    // Ctrl released before the button: the emote click is placed, or the click replayed.
    Decision on = withEmote(Trigger::Ctrl, 0, true);
    on.onEvent(kd(CTRL));
    on.onEvent(md(Btn::Left, 100, 100));
    const Result up = on.onEvent(ku(CTRL));  // keep the Result alive: c points into it
    const Emit* c = findEmit(up, Emit::Kind::Click);
    CHECK(emitIs(c, WheelKind::Emote) && c->x == 100);
    CHECK(on.onEvent(mu(Btn::Left, 100, 100)).swallow);
    Decision off = withEmote();
    off.onEvent(kd(CTRL));
    off.onEvent(md(Btn::Left, 100, 100));
    Result r = off.onEvent(ku(CTRL));
    CHECK(r.injects.size() == 1 && injectIs(r.injects[0], InjectKind::ButtonDown, Btn::Left));
    CHECK(off.state() == S::Idle);
    CHECK(!off.onEvent(mu(Btn::Left, 100, 100)).swallow);
  }
}

static void test_escape_and_right_click_cancel_emote_gesture() {
  Decision d = withEmote();
  openEmoteWheel(d);
  Result r = d.onEvent(kd(ESC));
  CHECK(r.swallow);
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.onEvent(ku(ESC)).swallow);
  r = d.onEvent(mu(Btn::Left, 130, 100));
  CHECK(r.swallow && r.emits.empty());
  CHECK(d.state() == S::Idle);

  d.onEvent(md(Btn::Left, 100, 100));  // Ctrl still held
  CHECK(d.state() == S::Pending);
  r = d.onEvent(md(Btn::Right, 100, 100));
  CHECK(r.swallow);
  CHECK(findEmit(r, Emit::Kind::Cancel) != nullptr);
  CHECK(d.onEvent(mu(Btn::Right, 100, 100)).swallow);
  r = d.onEvent(mu(Btn::Left, 100, 100));
  CHECK(r.swallow && r.emits.empty() && r.injects.empty());
  CHECK(d.state() == S::Idle);
}

static void test_emote_trigger_none_ignores_ctrl_drag() {
  Decision d = withEmote(Trigger::None);
  d.onEvent(kd(CTRL));
  CHECK(!d.onEvent(md(Btn::Left, 100, 100)).swallow);
  CHECK(d.onEvent(mv(150, 100)).emits.empty());
  CHECK(!d.onEvent(mu(Btn::Left, 150, 100)).swallow);
  CHECK(d.state() == S::Idle);
}

static void test_custom_emote_key_is_swallowed() {
  Decision d = withEmote(Trigger::CustomVk, KEY_T);
  CHECK(d.onEvent(kd(KEY_T)).swallow);
  CHECK(d.onEvent(md(Btn::Left, 0, 0)).swallow);
  CHECK(emitIs(findEmit(d.onEvent(mv(0, 40)), Emit::Kind::WheelOpen), WheelKind::Emote));
  CHECK(emitIs(findEmit(d.onEvent(mu(Btn::Left, 0, 40)), Emit::Kind::WheelRelease), WheelKind::Emote));
  CHECK(d.onEvent(ku(KEY_T)).swallow);
  CHECK(!d.onEvent(kd(KEY_V)).swallow);  // other keys still type
  d.onEvent(ku(KEY_V));
  d.setEnabled(false);
  CHECK(!d.onEvent(kd(KEY_T)).swallow);  // disabled: typing T works again
  CHECK(!d.onEvent(ku(KEY_T)).swallow);
}

static void test_mouse5_emote_beside_alt_ping() {
  Decision d = withEmote(Trigger::Mouse5);
  CHECK(!d.onEvent(md(Btn::Left, 0, 0)).swallow);  // no key held: a plain click
  d.onEvent(mu(Btn::Left, 0, 0));
  CHECK(d.onEvent(md(Btn::X2, 10, 10)).swallow);
  CHECK(emitIs(findEmit(d.onEvent(mv(40, 10)), Emit::Kind::WheelOpen), WheelKind::Emote));
  Result r = d.onEvent(mu(Btn::X2, 40, 10));
  CHECK(r.swallow && emitIs(findEmit(r, Emit::Kind::WheelRelease), WheelKind::Emote));

  d.onEvent(kd(ALT));
  CHECK(d.onEvent(md(Btn::Left, 100, 100)).swallow);
  CHECK(emitIs(findEmit(d.onEvent(mv(130, 100)), Emit::Kind::WheelOpen), WheelKind::Ping));
  CHECK(emitIs(findEmit(d.onEvent(mu(Btn::Left, 130, 100)), Emit::Kind::WheelRelease), WheelKind::Ping));

  // With Alt still held, the side button is only the emote trigger.
  CHECK(d.onEvent(md(Btn::X2, 10, 10)).swallow);
  CHECK(emitIs(findEmit(d.onEvent(mv(40, 10)), Emit::Kind::WheelOpen), WheelKind::Emote));
  d.onEvent(mu(Btn::X2, 40, 10));
  CHECK(d.state() == S::Idle);

  // A side-button click without a drag is replayed (emote click off), so browser Forward still works.
  d.onEvent(md(Btn::X2, 10, 10));
  r = d.onEvent(mu(Btn::X2, 10, 10));
  CHECK(r.swallow && r.injects.size() == 2);
  CHECK(injectIs(r.injects[0], InjectKind::ButtonDown, Btn::X2));
  CHECK(injectIs(r.injects[1], InjectKind::ButtonUp, Btn::X2));
}

static void test_pause_stops_both_wheels() {
  Decision d = withEmote();
  d.onEvent(kd(CTRL));
  d.onEvent(kd(ALT));
  CHECK(findEmit(d.onEvent(kd(KEY_P)), Emit::Kind::Toggled) != nullptr);
  CHECK(!d.enabled());
  d.onEvent(ku(KEY_P));
  d.onEvent(ku(ALT));  // Ctrl alone is still held
  CHECK(!d.onEvent(md(Btn::Left, 100, 100)).swallow);
  CHECK(d.onEvent(mv(150, 100)).emits.empty());
  CHECK(!d.onEvent(mu(Btn::Left, 150, 100)).swallow);
  d.onEvent(ku(CTRL));
  d.onEvent(kd(ALT));
  CHECK(!d.onEvent(md(Btn::Left, 100, 100)).swallow);
  CHECK(!d.onEvent(mu(Btn::Left, 100, 100)).swallow);
}

static void test_releasing_emote_trigger_mid_wheel_keeps_wheel() {
  Decision d = withEmote();
  openEmoteWheel(d);
  d.onEvent(kd(ALT));  // the other wheel's trigger doesn't touch this gesture
  CHECK(d.onEvent(ku(ALT)).emits.empty());
  CHECK(d.state() == S::Wheel);
  CHECK(d.onEvent(ku(CTRL)).emits.empty());
  CHECK(d.state() == S::Wheel);
  Result r = d.onEvent(mu(Btn::Left, 130, 100));
  CHECK(r.swallow && emitIs(findEmit(r, Emit::Kind::WheelRelease), WheelKind::Emote));
  CHECK(d.state() == S::Idle);
}

static void test_parse_config_emote_fields() {
  const Config cur;
  Command c = parseCommand(R"({"type":"config","emoteTrigger":"off"})", cur);
  CHECK(c.kind == Command::Kind::Config && c.config.emoteTrigger == Trigger::None);
  c = parseCommand(R"({"type":"config","emoteTrigger":"mouse5","emoteClick":true})", cur);
  CHECK(c.kind == Command::Kind::Config && c.config.emoteTrigger == Trigger::Mouse5 && c.config.emoteClick);
  c = parseCommand(R"({"type":"config","emoteTrigger":"vk","emoteTriggerVk":84,"emoteClick":false})", cur);
  CHECK(c.config.emoteTrigger == Trigger::CustomVk && c.config.emoteTriggerVk == 84 && !c.config.emoteClick);
  c = parseCommand(R"({"type":"config","trigger":"alt","emoteTrigger":"ctrl"})", cur);
  CHECK(c.config.trigger == Trigger::Alt && c.config.emoteTrigger == Trigger::Ctrl);
  Config set;
  set.emoteTrigger = Trigger::Shift;
  set.emoteTriggerVk = 7;
  set.emoteClick = true;
  c = parseCommand(R"({"type":"config"})", set);  // missing fields keep their current values
  CHECK(c.config.emoteTrigger == Trigger::Shift && c.config.emoteTriggerVk == 7 && c.config.emoteClick);
  CHECK(parseCommand(R"({"type":"config","emoteTrigger":"bogus"})", cur).kind == Command::Kind::Invalid);
  CHECK(parseCommand(R"({"type":"config","trigger":"off"})", cur).kind == Command::Kind::Invalid);  // pings always have a trigger
}

static void test_json_parses_flat_objects() {
  std::map<std::string, std::string> m;
  CHECK(parseFlatJson(R"({"type":"config","enabled":true,"dragThresholdPx":8,"trigger":"alt"})", m));
  CHECK(m["type"] == "config" && m["enabled"] == "true" && m["dragThresholdPx"] == "8" && m["trigger"] == "alt");
  CHECK(parseFlatJson(R"( { "msg" : "a\"b\\c" } )", m) && m["msg"] == "a\"b\\c");
  CHECK(parseFlatJson("{}", m) && m.empty());
}

static void test_json_rejects_bad_input() {
  std::map<std::string, std::string> m;
  CHECK(!parseFlatJson("", m));
  CHECK(!parseFlatJson("not json", m));
  CHECK(!parseFlatJson(R"({"a":{"b":1}})", m));
  CHECK(!parseFlatJson(R"({"a":[1]})", m));
  CHECK(!parseFlatJson(R"({"a":1)", m));
  CHECK(!parseFlatJson(R"({"a":1} trailing)", m));
}

static void test_json_escape() { CHECK(jsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n"); }

static void test_format_emit() {
  Emit open{Emit::Kind::WheelOpen};
  open.x = 1;
  open.y = -2;
  CHECK(formatEmit(open) == R"({"type":"wheelOpen","x":1,"y":-2,"wheel":"ping"})");
  Emit toggled{Emit::Kind::Toggled};
  toggled.enabled = true;
  CHECK(formatEmit(toggled) == R"({"type":"toggled","enabled":true})");
  CHECK(formatEmit(Emit{Emit::Kind::Cancel}) == R"({"type":"cancel"})");
}

static void test_format_emit_includes_wheel() {
  Emit open{Emit::Kind::WheelOpen};
  open.x = 1;
  open.y = 2;
  open.wheel = WheelKind::Emote;
  CHECK(formatEmit(open) == R"({"type":"wheelOpen","x":1,"y":2,"wheel":"emote"})");
  Emit click{Emit::Kind::Click};
  click.x = 3;
  click.y = 4;
  CHECK(formatEmit(click) == R"({"type":"click","x":3,"y":4,"wheel":"ping"})");
  click.wheel = WheelKind::Emote;
  CHECK(formatEmit(click) == R"({"type":"click","x":3,"y":4,"wheel":"emote"})");
  for (const auto k : {Emit::Kind::WheelMove, Emit::Kind::WheelRelease}) {
    Emit e{k};
    e.wheel = WheelKind::Emote;
    CHECK(formatEmit(e).find(R"(,"wheel":"emote"})") != std::string::npos);
  }
}

// Review focus: a frozen Electron must never stall the hook thread.
static void test_output_never_blocks_producer() {
  std::mutex m;
  std::condition_variable cv;
  bool release = false;
  size_t written = 0;
  Output out(
      [&](const std::string&) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return release; });
        ++written;
        return true;
      },
      100);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 10000; ++i) out.line("x");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  CHECK(ms < 500);
  CHECK(out.dropped() >= 10000 - 101);
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
  }
  cv.notify_all();
  out.stop();
  CHECK(written + out.dropped() == 10000);
}

static void test_mask_menu_keys_off_leaves_alt_up_alone() {
  Decision d;
  d.setMaskMenuKeys(false);
  openWheel(d);
  d.onEvent(mu(Btn::Left, 130, 100));
  Result r = d.onEvent(ku(ALT));
  CHECK(!r.swallow && r.injects.empty());
}

static void test_mac_keycodes_map_to_vks() {
  CHECK(mac::keyCodeToVk(0x23) == KEY_P);
  CHECK(mac::keyCodeToVk(0x00) == 'A');
  CHECK(mac::keyCodeToVk(0x1D) == '0');
  CHECK(mac::keyCodeToVk(0x35) == ESC);
  CHECK(mac::keyCodeToVk(0x7A) == 0x70);  // F1
  CHECK(mac::keyCodeToVk(0x5A) == 0x83);  // F20
  CHECK(mac::keyCodeToVk(0x7B) == 0x25);  // left arrow
  CHECK(mac::keyCodeToVk(0x3F) == 0);     // fn has no VK
  CHECK(mac::vkToKeyCode(KEY_P) == 0x23);
  CHECK(mac::vkToKeyCode(0x0D) == 0x24);  // Return, not keypad Enter
  CHECK(mac::vkToKeyCode(ALT) == 0x3A);
  CHECK(mac::vkToKeyCode(0x87) == -1);    // F24 doesn't exist on a Mac
  for (uint16_t code = 0; code < 0x80; ++code) {
    const uint32_t vk = mac::keyCodeToVk(code);
    if (vk != 0 && code != 0x4C) CHECK(mac::vkToKeyCode(vk) == code);  // round trip (keypad Enter shares Return's VK)
  }
}

static void test_mac_modifier_held() {
  using namespace mac;
  CHECK(modifierHeld(kFlagOption | kDevLAlt, 0xA4));
  CHECK(!modifierHeld(kFlagOption | kDevLAlt, 0xA5));
  CHECK(modifierHeld(kFlagOption | kDevRAlt, 0xA5));
  CHECK(modifierHeld(kFlagOption, 0xA4));            // synthetic event without side bits: left
  CHECK(!modifierHeld(kFlagOption, 0xA5));
  CHECK(!modifierHeld(kDevLAlt, 0xA4));              // stale side bit with the group up
  CHECK(modifierHeld(kFlagCommand | kDevRCmd, 0x5C));
  CHECK(modifierHeld(kFlagControl | kDevRCtrl, 0xA3));
  CHECK(!modifierHeld(kFlagShift | kDevLShift, 'A'));
}

static void test_mac_modifier_transitions() {
  using namespace mac;
  auto t = modifierTransitions(0, kFlagOption | kDevLAlt);
  CHECK(t.size() == 1 && t[0].vk == 0xA4 && t[0].down);
  t = modifierTransitions(kFlagOption | kDevLAlt, 0);
  CHECK(t.size() == 1 && t[0].vk == 0xA4 && !t[0].down);
  // Option released and Control pressed in one step: ups come first.
  t = modifierTransitions(kFlagOption | kDevLAlt, kFlagControl | kDevLCtrl);
  CHECK(t.size() == 2 && t[0].vk == 0xA4 && !t[0].down && t[1].vk == 0xA2 && t[1].down);
  // Right Command added to left Command.
  t = modifierTransitions(kFlagCommand | kDevLCmd, kFlagCommand | kDevLCmd | kDevRCmd);
  CHECK(t.size() == 1 && t[0].vk == 0x5C && t[0].down);
  CHECK(modifierTransitions(kFlagShift | kDevLShift, kFlagShift | kDevLShift).empty());
}

static void test_mac_option_drag_through_transitions() {
  // The Mac hook feeds flag transitions to Decision as key events: Option + drag must open the wheel.
  Decision d;
  d.setMaskMenuKeys(false);
  for (const auto& k : mac::modifierTransitions(0, mac::kFlagOption | mac::kDevLAlt)) {
    d.onEvent(k.down ? kd(k.vk) : ku(k.vk));
  }
  CHECK(d.onEvent(md(Btn::Left, 10, 10)).swallow);
  Result r = d.onEvent(mv(40, 10));
  CHECK(findEmit(r, Emit::Kind::WheelOpen) != nullptr);
}

static void test_linux_keycodes_map_to_vks() {
  using namespace lnx;
  CHECK(keyCodeToVk(kKeyA) == 'A');
  CHECK(keyCodeToVk(25) == KEY_P);
  CHECK(keyCodeToVk(2) == '1' && keyCodeToVk(11) == '0');
  CHECK(keyCodeToVk(1) == ESC);
  CHECK(keyCodeToVk(15) == TAB);
  CHECK(keyCodeToVk(58) == CAPS);
  CHECK(keyCodeToVk(59) == 0x70 && keyCodeToVk(88) == 0x7B);   // F1, F12
  CHECK(keyCodeToVk(183) == 0x7C && keyCodeToVk(kKeyF24) == 0x87);  // F13, F24
  CHECK(keyCodeToVk(105) == 0x25 && keyCodeToVk(108) == 0x28);  // left, down
  CHECK(keyCodeToVk(28) == 0x0D && keyCodeToVk(96) == 0x0D);    // Enter and keypad Enter
  CHECK(keyCodeToVk(82) == 0x60 && keyCodeToVk(98) == 0x6F);    // keypad 0, keypad /
  CHECK(keyCodeToVk(39) == 0xBA && keyCodeToVk(41) == 0xC0);    // ; `
  CHECK(keyCodeToVk(kKeyLeftAlt) == ALT && keyCodeToVk(100) == 0xA5);
  CHECK(keyCodeToVk(29) == CTRL && keyCodeToVk(97) == 0xA3);
  CHECK(keyCodeToVk(42) == 0xA0 && keyCodeToVk(54) == 0xA1);
  CHECK(keyCodeToVk(125) == 0x5B && keyCodeToVk(126) == 0x5C);  // Super is Win
  CHECK(keyCodeToVk(0) == 0);                                    // KEY_RESERVED
  CHECK(keyCodeToVk(lnx::kBtnLeft) == 0);                        // buttons are not keys
  CHECK(keyCodeToVk(0xFFFF) == 0);
  // Every VK has one key, except Enter, which keypad Enter shares.
  std::map<uint32_t, int> seen;
  for (uint32_t code = 0; code <= 0x2FF; ++code) {
    const uint32_t vk = keyCodeToVk(static_cast<uint16_t>(code));
    if (vk != 0) ++seen[vk];
  }
  for (const auto& v : seen) CHECK(v.second == 1 || v.first == 0x0D);
}

static void test_linux_buttons_map_both_ways() {
  using namespace lnx;
  CHECK(buttonOf(kBtnLeft) == Btn::Left && buttonOf(kBtnRight) == Btn::Right && buttonOf(kBtnMiddle) == Btn::Middle);
  CHECK(buttonOf(kBtnSide) == Btn::X1 && buttonOf(kBtnExtra) == Btn::X2);  // Mouse 4 and Mouse 5
  CHECK(buttonOf(0x115) == Btn::None);  // BTN_FORWARD
  CHECK(buttonOf(0) == Btn::None && buttonOf(kKeyA) == Btn::None);
  for (const Btn b : {Btn::Left, Btn::Right, Btn::Middle, Btn::X1, Btn::X2}) CHECK(buttonOf(buttonCode(b)) == b);
  CHECK(buttonCode(Btn::None) == 0);
}

namespace {
// The caps of a device that reports exactly these (type, code) pairs.
lnx::DeviceCaps capsOf(std::initializer_list<std::pair<uint16_t, uint16_t>> codes) {
  return lnx::readCaps([codes](uint16_t type, uint16_t code) {
    for (const auto& c : codes) {
      if (c.first == type && c.second == code) return true;
    }
    return false;
  });
}
constexpr uint16_t ABS_Y = 0x01, ABS_VOLUME = 0x20, ABS_MISC = 0x28, REL_WHEEL = 0x08;
}  // namespace

static void test_linux_device_kind_from_caps() {
  using namespace lnx;
  const std::pair<uint16_t, uint16_t> relX{kEvRel, kRelX}, relY{kEvRel, kRelY}, left{kEvKey, kBtnLeft};
  CHECK(deviceKind(capsOf({relX, relY, left})) == DeviceKind::Mouse);
  CHECK(deviceKind(capsOf({relX, relY, left, {kEvRel, REL_WHEEL}, {kEvKey, kBtnSide}})) == DeviceKind::Mouse);
  // Gaming mice and receivers that put a non-pointer axis on the pointer node are still mice.
  CHECK(deviceKind(capsOf({relX, relY, left, {kEvAbs, ABS_MISC}})) == DeviceKind::Mouse);
  CHECK(deviceKind(capsOf({relX, relY, left, {kEvAbs, ABS_VOLUME}})) == DeviceKind::Mouse);
  CHECK(!capsOf({{kEvAbs, ABS_MISC}, {kEvAbs, ABS_Y}}).absPointer);
  // Absolute pointers are left alone, even when they also report relative motion.
  CHECK(deviceKind(capsOf({left, {kEvAbs, kAbsX}, {kEvAbs, ABS_Y}, {kEvAbs, kAbsMtPositionX}})) == DeviceKind::Other);
  CHECK(deviceKind(capsOf({relX, relY, left, {kEvAbs, kAbsX}})) == DeviceKind::Other);
  CHECK(deviceKind(capsOf({relX, relY, left, {kEvAbs, kAbsMtPositionX}})) == DeviceKind::Other);
  CHECK(deviceKind(capsOf({relX, relY})) == DeviceKind::Other);  // no button
  CHECK(deviceKind(capsOf({relX, left})) == DeviceKind::Other);  // one axis
  CHECK(deviceKind(capsOf({{kEvKey, kKeyA}})) == DeviceKind::Keyboard);
  CHECK(deviceKind(capsOf({{kEvKey, kKeyLeftAlt}})) == DeviceKind::Keyboard);
  CHECK(deviceKind(capsOf({{kEvKey, kKeyA}, {kEvAbs, ABS_VOLUME}})) == DeviceKind::Keyboard);
  CHECK(deviceKind(capsOf({{kEvKey, kKeyA}, {kEvAbs, kAbsX}})) == DeviceKind::Keyboard);
  // A mouse node that also has keys is grabbed as a mouse.
  CHECK(deviceKind(capsOf({relX, relY, left, {kEvKey, kKeyA}})) == DeviceKind::Mouse);
  CHECK(deviceKind(capsOf({{kEvKey, kKeyA}})) == DeviceKind::Keyboard);
  CHECK(deviceKind(capsOf({})) == DeviceKind::Other);
  CHECK(deviceKind(DeviceCaps{}) == DeviceKind::Other);
  // Codes of the wrong type don't count: REL_X's number as a key, BTN_LEFT's as an axis.
  CHECK(deviceKind(capsOf({{kEvKey, kRelX}, {kEvKey, kRelY}, {kEvRel, kBtnLeft}})) == DeviceKind::Other);
}

static void test_linux_virtual_devices_by_sysfs_path() {
  using namespace lnx;
  CHECK(isVirtualDevice("/sys/devices/virtual/input/input42/event17"));  // uinput: clones, ydotoold, Sunshine
  CHECK(!isVirtualDevice("/sys/devices/virtual/misc/uhid/0005:046D:B023.0001/input/input30/event30"));  // BLE
  CHECK(!isVirtualDevice("/sys/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/0003:046D:C52B.0001/input/input5/event5"));
  CHECK(!isVirtualDevice("/sys/devices/platform/i8042/serio0/input/input3/event3"));
  CHECK(!isVirtualDevice(""));
  CHECK(!isVirtualDevice("/sys/devices/virtual/input"));
  CHECK(!isVirtualDevice("/sys/devices/virtual/inputs/input1/event1"));
  CHECK(!isVirtualDevice("/sys/devices/virtualx/input/input1/event1"));
  CHECK(!isVirtualDevice("devices/virtual/input/input1/event1"));
}

static void test_linux_key_union_holds_a_key_while_any_keyboard_does() {
  using namespace lnx;
  KeyUnion k;
  auto t = k.onKey(1, kKeyLeftAlt, 1);
  CHECK(t.size() == 1 && t[0].vk == ALT && t[0].down);
  CHECK(k.onKey(2, kKeyLeftAlt, 1).empty());  // the second keyboard's Alt: already down
  CHECK(k.onKey(1, kKeyLeftAlt, 0).empty());  // still held on keyboard 2
  CHECK(k.isDown(ALT));
  t = k.onKey(2, kKeyLeftAlt, 0);
  CHECK(t.size() == 1 && t[0].vk == ALT && !t[0].down);
  CHECK(!k.isDown(ALT));
  t = k.onKey(1, 100, 1);  // right Alt is its own VK
  CHECK(t.size() == 1 && t[0].vk == 0xA5 && t[0].down && !k.isDown(ALT));
}

static void test_linux_key_union_repeats_and_strays() {
  using namespace lnx;
  KeyUnion k;
  k.onKey(1, kKeyA, 1);
  auto t = k.onKey(1, kKeyA, 2);  // auto-repeat: a key-down again, as on Windows
  CHECK(t.size() == 1 && t[0].vk == 'A' && t[0].down);
  t = k.onKey(2, 48, 2);  // a repeat whose press was never seen counts as the press
  CHECK(t.size() == 1 && t[0].vk == 'B' && t[0].down);
  CHECK(k.onKey(2, kKeyA, 2).empty());  // keyboard 2's first sight of a key keyboard 1 holds
  CHECK(k.onKey(3, 31, 0).empty());  // an up for a key nobody holds
  CHECK(k.onKey(1, lnx::kBtnLeft, 1).empty());  // no VK
  CHECK(k.onKey(1, 0x2FF, 1).empty());
  // Enter and keypad Enter share a VK: it goes up with the last of them.
  KeyUnion e;
  CHECK(e.onKey(1, 28, 1).size() == 1);
  CHECK(e.onKey(1, 96, 1).empty());
  CHECK(e.onKey(1, 28, 0).empty());
  t = e.onKey(1, 96, 0);
  CHECK(t.size() == 1 && t[0].vk == 0x0D && !t[0].down);
}

static void test_linux_key_union_set_held() {
  using namespace lnx;
  KeyUnion k;
  auto t = k.setHeld(1, {kKeyLeftAlt, kKeyA, lnx::kBtnLeft});  // buttons are ignored
  CHECK(t.size() == 2 && t[0].down && t[1].down && k.isDown(ALT) && k.isDown('A'));
  t = k.setHeld(1, {kKeyA, 29});  // resync: Alt went up, Ctrl went down; ups come first
  CHECK(t.size() == 2 && t[0].vk == ALT && !t[0].down && t[1].vk == CTRL && t[1].down);
  k.onKey(2, 29, 1);
  t = k.setHeld(1, {});  // keyboard 1 unplugged: Ctrl stays down for keyboard 2
  CHECK(t.size() == 1 && t[0].vk == 'A' && !t[0].down && k.isDown(CTRL));
  CHECK(k.setHeld(1, {}).empty());
  CHECK(k.setHeld(3, {}).empty());  // a device that never held anything
}

static void test_linux_key_union_drives_decision() {
  // Alt from the keyboard, a drag from the mouse: the wheel opens. The keyboard vanishing releases Alt.
  using namespace lnx;
  KeyUnion keys;
  Decision d;
  d.setMaskMenuKeys(false);
  for (const auto& t : keys.onKey(7, kKeyLeftAlt, 1)) d.onEvent(t.down ? kd(t.vk) : ku(t.vk));
  CHECK(d.onEvent(md(Btn::Left, 0, 0)).swallow);
  CHECK(findEmit(d.onEvent(mv(20, 0)), Emit::Kind::WheelOpen) != nullptr);
  d.onEvent(mu(Btn::Left, 20, 0));
  for (const auto& t : keys.setHeld(7, {})) d.onEvent(t.down ? kd(t.vk) : ku(t.vk));
  CHECK(!d.onEvent(md(Btn::Left, 0, 0)).swallow);  // Alt is up: a plain click
  d.onEvent(mu(Btn::Left, 0, 0));
  // A key Decision missed going up is cleared by a sync against the union.
  d.onEvent(kd(ALT));
  d.syncKeyStates([&](uint32_t vk) { return keys.isDown(vk); });
  CHECK(!d.onEvent(md(Btn::Left, 0, 0)).swallow);
}

static void test_linux_mask_tap_after_swallowed_press_under_alt_or_super() {
  using namespace lnx;
  KeyUnion keys;
  const InputEvent down = md(Btn::Left, 0, 0);
  CHECK(!shouldTapMask(down, true, keys));  // nothing held
  keys.onKey(1, kKeyLeftAlt, 1);
  CHECK(shouldTapMask(down, true, keys));
  CHECK(!shouldTapMask(down, false, keys));  // passed through: the app saw Alt+click, so no lone tap
  CHECK(!shouldTapMask(mu(Btn::Left, 0, 0), true, keys));
  CHECK(!shouldTapMask(kd('A'), true, keys));
  keys.onKey(1, kKeyLeftAlt, 0);
  keys.onKey(1, 100, 1);  // right Alt
  CHECK(shouldTapMask(down, true, keys));
  keys.onKey(1, 100, 0);
  keys.onKey(1, 125, 1);  // Super
  CHECK(shouldTapMask(md(Btn::X1, 0, 0), true, keys));
  keys.onKey(1, 125, 0);
  keys.onKey(1, 126, 1);
  CHECK(shouldTapMask(down, true, keys));
  keys.onKey(1, 126, 0);
  keys.onKey(1, 29, 1);  // Ctrl has no lone-tap action
  keys.onKey(1, 42, 1);
  CHECK(!shouldTapMask(down, true, keys));
}

static void test_linux_parse_cursor_pos() {
  using namespace lnx;
  int x = 0, y = 0;
  CHECK(parseCursorPos("{\n    \"x\": 4467,\n    \"y\": 885\n}\n", x, y) && x == 4467 && y == 885);
  CHECK(parseCursorPos(R"({"y":2,"x":1})", x, y) && x == 1 && y == 2);
  CHECK(parseCursorPos(R"({"x": -1920, "y": 0})", x, y) && x == -1920 && y == 0);  // a monitor left of the origin
  CHECK(parseCursorPos(R"({"x": 12.6, "y": -3.5})", x, y) && x == 13 && y == -4);
  x = 5;
  y = 6;
  CHECK(!parseCursorPos("", x, y));
  CHECK(!parseCursorPos("unknown request", x, y));
  CHECK(!parseCursorPos(R"({"x": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": "a", "y": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": 1e3, "y": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": 0x10, "y": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": inf, "y": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": 1., "y": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": -, "y": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": 12345678901, "y": 1})", x, y));
  CHECK(!parseCursorPos(R"({"x": {"a": 1}, "y": 1})", x, y));
  CHECK(x == 5 && y == 6);  // a failed parse leaves the outputs alone
}

namespace {
// A compositor that answers with the given positions, one per query, then stops answering, or keeps answering
// `stuck` (a locked pointer) once the list is used up.
struct FakeCursor {
  std::deque<std::pair<int, int>> answers;
  std::optional<std::pair<int, int>> stuck;
  int queries = 0;
  lnx::CursorEmits::Query query() {
    return [this](int& x, int& y) {
      ++queries;
      if (answers.empty()) {
        if (!stuck) return false;
        x = stuck->first;
        y = stuck->second;
        return true;
      }
      x = answers.front().first;
      y = answers.front().second;
      answers.pop_front();
      return true;
    };
  }
};
Emit pointEmit(Emit::Kind k, int x, int y, WheelKind w = WheelKind::Ping) {
  Emit e{k};
  e.x = x;
  e.y = y;
  e.wheel = w;
  return e;
}
}  // namespace

static void test_linux_moved_past_threshold() {
  using lnx::movedPastThreshold;
  CHECK(!movedPastThreshold(100, 100, 108, 100, 8));  // exactly the threshold: not yet, as in Decision
  CHECK(movedPastThreshold(100, 100, 109, 100, 8));
  CHECK(movedPastThreshold(100, 100, 100, 91, 8));
  CHECK(movedPastThreshold(100, 100, 106, 106, 8));   // 8.5 px on the diagonal
  CHECK(!movedPastThreshold(100, 100, 95, 106, 8));   // 7.8 px
  CHECK(movedPastThreshold(-1920, 0, -1930, 0, 8));   // a monitor left of the origin
  CHECK(!movedPastThreshold(5, 5, 5, 5, 0));
  CHECK(movedPastThreshold(5, 5, 6, 5, 0));
}

static void test_linux_pending_press_reports_a_drag_once() {
  FakeCursor c;
  c.answers = {{100, 100}, {104, 104}, {109, 100}, {200, 200}};
  lnx::CursorEmits p(c.query());
  CHECK(!p.polling() && !p.onPoll().dragged && c.queries == 0);  // nothing to watch: no queries
  p.onPress(8);  // at (100, 100)
  CHECK(p.polling() && !p.open());
  lnx::CursorEmits::Poll poll = p.onPoll();
  CHECK(!poll.dragged && !poll.move);  // 5.7 px: still a click
  poll = p.onPoll();
  CHECK(poll.dragged && !poll.move);  // 9 px
  CHECK(!p.polling());                // once: the wheel's open or the press's end comes next
  const int before = c.queries;
  CHECK(!p.onPoll().dragged && c.queries == before);
  // Decision's open emit arrives: placed at the press, not where the drag was noticed.
  const auto e = p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  CHECK(e && e->x == 100 && e->y == 100 && p.open() && p.polling());
  poll = p.onPoll();
  CHECK(!poll.dragged && poll.move && poll.move->x == 200 && poll.move->y == 200);
}

static void test_linux_pending_press_stops_with_click_end_or_cancel() {
  FakeCursor c;
  c.answers = {{10, 10}, {10, 10}, {20, 20}, {30, 30}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);
  CHECK(!p.onPoll().dragged);
  p.onPressEnded();  // a replayed click: Decision went idle without an emit
  CHECK(!p.polling());
  int before = c.queries;
  CHECK(!p.onPoll().dragged && c.queries == before);
  p.onPress(8);  // at (20, 20)
  const auto e = p.onEmit(pointEmit(Emit::Kind::Click, 0, 0));
  CHECK(e && e->x == 20 && e->y == 20 && !p.polling());
  p.onPress(8);  // at (30, 30)
  p.onEmit(Emit{Emit::Kind::Cancel});
  before = c.queries;
  CHECK(!p.polling() && !p.onPoll().dragged && c.queries == before);
}

static void test_linux_pending_press_measures_from_the_first_answer() {
  // The compositor didn't answer at the press: the first answer becomes the press, not a jump from the last one.
  FakeCursor c;
  c.answers = {{0, 0}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);  // answered: (0, 0)
  p.onPressEnded();
  c.answers = {};
  p.onPress(8);  // no answer
  c.answers = {{500, 500}, {505, 500}, {520, 500}};
  CHECK(!p.onPoll().dragged);  // 707 px from the old answer, but this is the baseline now
  CHECK(!p.onPoll().dragged);
  CHECK(p.onPoll().dragged);
  const auto e = p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  CHECK(e && e->x == 500 && e->y == 500);
}

static void test_linux_threshold_move_opens_the_wheel() {
  // The event loop feeds Decision every mouse event at (0, 0) and pastThresholdMove() once the cursor thread
  // reports a drag: raw motion alone never opens a wheel, that move always does.
  for (const int threshold : {1, 8, 40}) {
    Config c;
    c.dragThresholdPx = threshold;
    Decision d;
    d.setMaskMenuKeys(false);
    d.setConfig(c);
    d.onEvent(kd(ALT));
    CHECK(d.onEvent(md(Btn::Left, 0, 0)).swallow);
    for (int i = 0; i < 50; ++i) d.onEvent(mv(0, 0));
    CHECK(d.state() == S::Pending);
    const Result r = d.onEvent(lnx::pastThresholdMove(threshold, 7));
    CHECK(d.state() == S::Wheel && findEmit(r, Emit::Kind::WheelOpen) != nullptr);
  }
}

static void test_linux_ungrabbed_release_ends_a_paused_gesture() {
  // Pings on Alt, emotes on Mouse 4. A pause mid-gesture ungrabs the mouse, so its left release comes through the
  // ungrabbed path. Without it, Decision waits in SwallowUp and the Mouse 4 trigger stays blocked.
  Config c;
  c.emoteTrigger = Trigger::Mouse4;
  Decision blocked;
  blocked.setConfig(c);
  blocked.onEvent(kd(ALT));
  CHECK(blocked.onEvent(md(Btn::Left, 0, 0)).swallow);
  blocked.setEnabled(false);
  blocked.setEnabled(true);
  blocked.onEvent(ku(ALT));
  CHECK(!blocked.onEvent(md(Btn::X1, 0, 0)).swallow);  // the release never came
  Decision d;
  d.setConfig(c);
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 0, 0));
  d.setEnabled(false);
  const Result r = lnx::ungrabbedRelease(d, Btn::Left, 5);
  CHECK(!r.swallow && r.injects.empty() && r.emits.empty() && d.state() == S::Idle);
  d.setEnabled(true);
  d.onEvent(ku(ALT));
  CHECK(d.onEvent(md(Btn::X1, 0, 0)).swallow && d.state() == S::Pending);
}

static void test_linux_ungrabbed_release_never_replays() {
  // A press Decision holds, released on a mouse that isn't grabbed: that release already reached the app, so the
  // click is not replayed, but a wheel still closes with its release.
  Decision d;
  d.onEvent(kd(ALT));
  d.onEvent(md(Btn::Left, 0, 0));
  Result r = lnx::ungrabbedRelease(d, Btn::Left, 1);
  CHECK(!r.swallow && r.injects.empty() && d.state() == S::Idle);
  openWheel(d);
  r = lnx::ungrabbedRelease(d, Btn::Left, 2);
  CHECK(findEmit(r, Emit::Kind::WheelRelease) != nullptr && d.state() == S::Idle);
  CHECK(lnx::ungrabbedRelease(d, Btn::Right, 3).emits.empty());  // nothing going on: nothing happens
}

static void test_linux_cursor_emits_place_a_wheel_gesture() {
  FakeCursor c;
  c.answers = {{500, 300}, {510, 300}, {510, 300}, {520, 305}, {530, 310}};
  lnx::CursorEmits p(c.query());
  CHECK(!p.open() && !p.onPoll().move && c.queries == 0);  // no polling while nothing is pending or open
  p.onPress(8);  // the swallowed press, at (500, 300) on screen
  auto e = p.onEmit(pointEmit(Emit::Kind::WheelOpen, 7, 8));  // Decision's own coordinates mean nothing here
  CHECK(e && e->kind == Emit::Kind::WheelOpen && e->x == 500 && e->y == 300 && p.open());
  CHECK(!p.onEmit(pointEmit(Emit::Kind::WheelMove, 30, 8)).has_value());  // dropped: polls send real moves
  auto m = p.onPoll().move;
  CHECK(m && m->kind == Emit::Kind::WheelMove && m->x == 510 && m->y == 300 && m->wheel == WheelKind::Ping);
  CHECK(!p.onPoll().move);  // unchanged: nothing sent
  m = p.onPoll().move;
  CHECK(m && m->x == 520 && m->y == 305);
  e = p.onEmit(pointEmit(Emit::Kind::WheelRelease, 40, 9));  // asks again at the release
  CHECK(e && e->kind == Emit::Kind::WheelRelease && e->x == 530 && e->y == 310 && !p.open() && !p.polling());
  const int before = c.queries;
  CHECK(!p.onPoll().move && c.queries == before);
}

static void test_linux_cursor_emits_click_cancel_and_toggle() {
  FakeCursor c;
  c.answers = {{40, 50}, {60, 70}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);
  auto e = p.onEmit(pointEmit(Emit::Kind::Click, 1, 2, WheelKind::Emote));
  CHECK(e && e->kind == Emit::Kind::Click && e->x == 40 && e->y == 50 && e->wheel == WheelKind::Emote && !p.open());
  p.onPress(8);
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0, WheelKind::Emote));
  CHECK(p.open());
  e = p.onEmit(Emit{Emit::Kind::Cancel});
  CHECK(e && e->kind == Emit::Kind::Cancel && !p.open());
  Emit toggled{Emit::Kind::Toggled};
  toggled.enabled = true;
  e = p.onEmit(toggled);
  CHECK(e && e->kind == Emit::Kind::Toggled && e->enabled);
  CHECK(c.queries == 2);  // only the presses asked
}

static void test_linux_cursor_emits_survive_a_silent_compositor() {
  FakeCursor c;
  c.answers = {{100, 200}, {110, 205}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0, WheelKind::Emote));
  const auto m = p.onPoll().move;
  CHECK(m && m->x == 110 && m->wheel == WheelKind::Emote);
  CHECK(!p.onPoll().move);  // no answer: nothing to send
  auto e = p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0, WheelKind::Emote));
  CHECK(e && e->x == 110 && e->y == 205);  // the last answer
  p.onPress(8);  // still silent: the last answer again
  e = p.onEmit(pointEmit(Emit::Kind::Click, 0, 0));
  CHECK(e && e->x == 110 && e->y == 205);
  lnx::CursorEmits none(nullptr);  // no compositor at all
  none.onPress(8);
  CHECK(!none.onPoll().dragged);
  e = none.onEmit(pointEmit(Emit::Kind::WheelOpen, 3, 4));
  CHECK(e && e->x == 0 && e->y == 0);
  CHECK(!none.onPoll().move);
}

static void test_linux_locked_pointer_measures_the_gesture_in_raw_motion() {
  // osu!lazer locks the pointer: the compositor answers the press position however far the mouse moves.
  FakeCursor c;
  c.stuck = {{400, 300}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);
  p.onMotion(5, 0);
  CHECK(!p.onPoll().dragged && !p.locked());  // 5 counts: not a drag in any unit
  p.onMotion(5, 0);                           // 10 counts, 10 px at the default scale
  for (int i = 0; i < lnx::CursorEmits::kLockConfirmPolls; ++i) CHECK(!p.onPoll().dragged && !p.locked());
  CHECK(p.onPoll().dragged && p.locked() && !p.polling());  // the cursor never moved: locked
  auto e = p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0, WheelKind::Emote));
  CHECK(e && e->x == 400 && e->y == 300 && p.open() && p.locked());
  const int queries = c.queries;
  p.onMotion(20, -10);
  auto m = p.onPoll().move;
  CHECK(m && m->x == 430 && m->y == 290 && m->wheel == WheelKind::Emote);  // the press plus every count since
  CHECK(!p.onPoll().move);  // nothing moved: nothing sent
  p.onMotion(-30, 0);
  m = p.onPoll().move;
  CHECK(m && m->x == 400 && m->y == 290);
  p.onMotion(0, 50);
  e = p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0));
  CHECK(e && e->x == 400 && e->y == 340 && !p.open() && !p.locked());
  CHECK(c.queries == queries);  // a locked gesture stops asking the compositor
  p.onMotion(100, 100);  // motion between gestures counts for nothing
  c.stuck = {{50, 50}};
  p.onPress(8);
  e = p.onEmit(pointEmit(Emit::Kind::Click, 0, 0));
  CHECK(e && e->x == 50 && e->y == 50);
}

static void test_linux_a_cursor_that_moves_is_never_locked() {
  // The compositor follows the mouse, a frame behind: raw motion passes the threshold first, the cursor catches up.
  FakeCursor c;
  c.answers = {{100, 100}, {100, 100}, {103, 100}, {106, 100}, {112, 100}, {118, 100}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);
  p.onMotion(9, 0);
  CHECK(!p.onPoll().dragged && !p.locked());  // still at the press: the first still poll
  p.onMotion(3, 0);
  CHECK(!p.onPoll().dragged && !p.locked());  // moved to 103: not locked, not past the threshold either
  p.onMotion(50, 0);
  CHECK(!p.onPoll().dragged);  // 106
  CHECK(p.onPoll().dragged && !p.locked());  // 112: the compositor's own threshold
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  p.onMotion(500, 500);  // ignored: the compositor's cursor is the position
  const auto m = p.onPoll().move;
  CHECK(m && m->x == 118 && m->y == 100);
  // A cursor that has moved off the press and stays there is never locked, however far the mouse goes.
  FakeCursor d;
  d.answers = {{0, 0}, {1, 0}, {1, 0}, {1, 0}, {1, 0}};
  lnx::CursorEmits q(d.query());
  q.onPress(8);
  q.onMotion(9, 0);
  for (int i = 0; i < 4; ++i) CHECK(!q.onPoll().dragged && !q.locked());
}

static void test_linux_a_locked_cursor_caught_mid_warp_still_locks() {
  // Hyprland moves a locked cursor and warps it back to the press, so a poll can land in between and see it away.
  // During the press: an answer short of the threshold doesn't reset the count.
  FakeCursor c;
  c.answers = {{100, 100}, {100, 100}, {103, 101}, {100, 100}, {100, 100}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);
  p.onMotion(30, 0);
  CHECK(!p.onPoll().dragged);  // still 1
  CHECK(!p.onPoll().dragged);  // (103, 101): caught mid-warp, ignored
  CHECK(!p.onPoll().dragged);  // still 2
  CHECK(p.onPoll().dragged && p.locked());  // still 3
  p.onEmit(Emit{Emit::Kind::Cancel});
  // An answer past the threshold opens the wheel the usual way; the cursor then keeps coming back to the press
  // while the mouse moves on, and the gesture turns locked.
  c.answers = {{100, 100}, {100, 100}, {300, 250}, {100, 100}, {100, 100}, {100, 100}};
  p.onPress(8);
  p.onMotion(30, 0);
  CHECK(!p.onPoll().dragged);
  CHECK(p.onPoll().dragged && !p.locked());  // (300, 250)
  auto e = p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  CHECK(e && e->x == 100 && e->y == 100);
  p.onMotion(10, 0);
  CHECK(!p.onPoll().move && !p.locked());  // back on the press: nothing to send yet (the wheel is already there)
  p.onMotion(10, 0);
  auto m = p.onPoll().move;
  CHECK(p.locked() && m && m->x == 150 && m->y == 100);  // twice: locked, and the raw motion since the press shows
  const int queries = c.queries;
  p.onMotion(0, 40);
  m = p.onPoll().move;
  CHECK(m && m->x == 150 && m->y == 140 && c.queries == queries);
  e = p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0));
  CHECK(e && e->x == 150 && e->y == 140);
  CHECK(p.pxPerCount() == lnx::CursorEmits::kDefaultPxPerCount);  // nothing learned from a gesture like that
  // A wheel held still after it opened doesn't count: the cursor is on the press because the mouse didn't move.
  c.answers = {{50, 50}, {70, 50}, {50, 50}, {50, 50}, {50, 50}, {50, 50}};
  p.onPress(8);
  p.onMotion(20, 0);
  CHECK(p.onPoll().dragged);
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  for (int i = 0; i < 4; ++i) CHECK(!p.onPoll().move && !p.locked());
}

static void test_linux_release_on_the_press_after_a_drag_uses_the_raw_motion() {
  // The wheel opened on an answer caught mid-warp and was released before the polls could tell: the cursor is
  // back on the press, but the mouse went 40 counts to the right.
  FakeCursor c;
  c.answers = {{50, 50}, {50, 50}, {200, 200}, {50, 50}};
  lnx::CursorEmits p(c.query());
  p.onPress(8);
  p.onMotion(40, 0);
  CHECK(!p.onPoll().dragged && p.onPoll().dragged && !p.locked());
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  auto e = p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0));
  CHECK(e && e->x == 90 && e->y == 50);
  CHECK(p.pxPerCount() == lnx::CursorEmits::kDefaultPxPerCount);  // the answer on the press teaches nothing
  // Released on the press after a mouse move too short for a drag: the compositor's answer stands.
  c.answers = {{50, 50}, {50, 50}, {200, 200}, {50, 50}};
  p.onPress(8);
  p.onMotion(5, 0);
  CHECK(!p.onPoll().dragged && p.onPoll().dragged);
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  e = p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0));
  CHECK(e && e->x == 50 && e->y == 50);
}

static void test_linux_raw_scale_is_learned_from_a_drag_the_compositor_followed() {
  // 80 counts moved the cursor 40 px: half a pixel per count. A locked gesture then measures in that scale.
  FakeCursor c;
  c.answers = {{0, 0}, {0, 0}, {40, 0}, {40, 0}};
  lnx::CursorEmits p(c.query());
  CHECK(p.pxPerCount() == lnx::CursorEmits::kDefaultPxPerCount);
  p.onPress(8);
  p.onMotion(30, 0);
  CHECK(!p.onPoll().dragged);  // (0, 0)
  p.onMotion(50, 0);
  CHECK(p.onPoll().dragged);  // (40, 0)
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  const auto e = p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0));  // asks: (40, 0)
  CHECK(e && e->x == 40 && p.pxPerCount() == 0.5);
  c.stuck = {{200, 200}};
  p.onPress(8);
  p.onMotion(16, 0);  // 8 px: exactly the threshold, not past it
  for (int i = 0; i <= lnx::CursorEmits::kLockConfirmPolls; ++i) CHECK(!p.onPoll().dragged);
  p.onMotion(2, 0);  // 9 px
  for (int i = 0; i < lnx::CursorEmits::kLockConfirmPolls; ++i) CHECK(!p.onPoll().dragged);
  CHECK(p.onPoll().dragged && p.locked());
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  p.onMotion(0, 60);
  const auto m = p.onPoll().move;
  CHECK(m && m->x == 209 && m->y == 230);
  p.onEmit(Emit{Emit::Kind::Cancel});
  CHECK(!p.locked() && p.pxPerCount() == 0.5);  // a locked gesture teaches nothing
  // Clicks and short drags teach nothing either; the scale is clamped to a sane range.
  c.stuck.reset();
  c.answers = {{0, 0}, {0, 0}, {3, 0}, {3, 0}};
  p.onPress(8);
  p.onMotion(200, 0);
  p.onEmit(pointEmit(Emit::Kind::Click, 0, 0));
  CHECK(p.pxPerCount() == 0.5);  // the cursor didn't pass the threshold
  p.onPress(8);
  p.onMotion(5, 0);
  c.answers = {{30, 0}, {30, 0}};
  CHECK(p.onPoll().dragged);
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0));
  CHECK(p.pxPerCount() == 0.5);  // 5 counts is too few to measure
  c.answers = {{0, 0}, {0, 0}, {1000, 0}, {1000, 0}};
  p.onPress(8);
  p.onMotion(10, 0);
  CHECK(!p.onPoll().dragged && p.onPoll().dragged);
  p.onEmit(pointEmit(Emit::Kind::WheelOpen, 0, 0));
  p.onEmit(pointEmit(Emit::Kind::WheelRelease, 0, 0));
  CHECK(p.pxPerCount() == 20.0);  // 100 px per count, clamped
}

int main() {
  test_json_parses_flat_objects();
  test_json_rejects_bad_input();
  test_json_escape();
  test_format_emit();
  test_format_emit_includes_wheel();
  test_parse_config_emote_fields();
  test_output_never_blocks_producer();
  test_alt_drag_opens_wheel_and_releases();
  test_plain_click_passes_through();
  test_alt_click_replays_when_click_ping_off();
  test_alt_click_pings_when_click_ping_on();
  test_self_injected_events_are_ignored();
  test_right_click_cancels_wheel();
  test_right_click_with_alt_but_no_gesture_passes();
  test_escape_cancels_wheel();
  test_trigger_released_during_wheel_keeps_wheel();
  test_trigger_released_during_pending_replays_click_after_mask();
  test_trigger_released_during_pending_pings_when_click_ping_on();
  test_alt_up_after_gesture_is_masked();
  test_plain_alt_tap_is_not_masked();
  test_alt_tab_clicks_pass_through();
  test_alt_tab_click_places_no_ping_even_with_click_ping_on();
  test_win_tab_clicks_pass_through_and_sync_clears_flag();
  test_fullscreen_game_keeps_the_click();
  test_fullscreen_probe_ignored_when_disabled();
  test_toggle_hotkey_disables_and_enables();
  test_hotkey_autorepeat_toggles_once();
  test_hotkey_ignored_while_suspended();
  test_disabling_mid_drag_cancels_and_swallows_up();
  test_config_change_mid_drag_cancels();
  test_lost_up_recovers_on_next_press();
  test_lost_right_up_does_not_swallow_next_click();
  test_lost_key_up_does_not_swallow_next_press();
  test_button_held_before_trigger_passes_up();
  test_watchdog_cancels_stale_gesture();
  test_watchdog_from_pending_swallows_later_up();
  test_sync_clears_stale_trigger();
  test_sync_keeps_keys_that_are_still_down();
  test_sync_never_sets_keys();
  test_sync_keeps_swallowed_trigger_key();
  test_ctrl_trigger_ignores_alt();
  test_capslock_trigger_swallows_key();
  test_custom_key_trigger();
  test_custom_key_tap_types_the_key();
  test_custom_key_rollover_keeps_typed_order();
  test_custom_key_flushes_before_modifier();
  test_custom_key_drag_types_nothing();
  test_caps_lock_is_not_tapped_through();
  test_mouse4_trigger_drags_with_side_button();
  test_ctrl_drag_opens_emote_wheel();
  test_alt_drag_is_still_the_ping_wheel();
  test_both_triggers_held_pass_through();
  test_emote_click_on_and_off();
  test_escape_and_right_click_cancel_emote_gesture();
  test_emote_trigger_none_ignores_ctrl_drag();
  test_custom_emote_key_is_swallowed();
  test_mouse5_emote_beside_alt_ping();
  test_pause_stops_both_wheels();
  test_releasing_emote_trigger_mid_wheel_keeps_wheel();
  test_mask_menu_keys_off_leaves_alt_up_alone();
  test_mac_keycodes_map_to_vks();
  test_mac_modifier_held();
  test_mac_modifier_transitions();
  test_mac_option_drag_through_transitions();
  test_linux_keycodes_map_to_vks();
  test_linux_buttons_map_both_ways();
  test_linux_device_kind_from_caps();
  test_linux_virtual_devices_by_sysfs_path();
  test_linux_key_union_holds_a_key_while_any_keyboard_does();
  test_linux_key_union_repeats_and_strays();
  test_linux_key_union_set_held();
  test_linux_key_union_drives_decision();
  test_linux_mask_tap_after_swallowed_press_under_alt_or_super();
  test_linux_parse_cursor_pos();
  test_linux_moved_past_threshold();
  test_linux_pending_press_reports_a_drag_once();
  test_linux_pending_press_stops_with_click_end_or_cancel();
  test_linux_pending_press_measures_from_the_first_answer();
  test_linux_threshold_move_opens_the_wheel();
  test_linux_ungrabbed_release_ends_a_paused_gesture();
  test_linux_ungrabbed_release_never_replays();
  test_linux_cursor_emits_place_a_wheel_gesture();
  test_linux_cursor_emits_click_cancel_and_toggle();
  test_linux_cursor_emits_survive_a_silent_compositor();
  test_linux_locked_pointer_measures_the_gesture_in_raw_motion();
  test_linux_a_cursor_that_moves_is_never_locked();
  test_linux_raw_scale_is_learned_from_a_drag_the_compositor_followed();
  test_linux_a_locked_cursor_caught_mid_warp_still_locks();
  test_linux_release_on_the_press_after_a_drag_uses_the_raw_motion();
  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
