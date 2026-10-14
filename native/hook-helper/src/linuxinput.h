#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "decision.h"

// Pure logic for the Linux hook layer. No Linux headers, so it is unit-tested on every platform.
namespace lp::lnx {

// evdev key codes from linux/input-event-codes.h.
constexpr uint16_t kKeyA = 30, kKeyLeftAlt = 56, kKeyF24 = 194;
constexpr uint16_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112, kBtnSide = 0x113, kBtnExtra = 0x114;

// evdev key code → Windows virtual-key code, by physical position. 0 when the key has no VK equivalent.
uint32_t keyCodeToVk(uint16_t code);
// evdev button code → mouse button. BTN_SIDE is Mouse 4 (X1), BTN_EXTRA Mouse 5 (X2). None for other codes.
Btn buttonOf(uint16_t code);
// Mouse button → evdev button code, for replays. 0 for Btn::None.
uint16_t buttonCode(Btn btn);

// The capability bits that sort a device.
struct DeviceCaps {
  bool relX = false;
  bool relY = false;
  bool btnLeft = false;
  bool abs = false;  // any EV_ABS axis: touchpads, tablets, touchscreens and joysticks have one
  bool keyA = false;
  bool keyLeftAlt = false;
};

enum class DeviceKind { Other, Mouse, Keyboard };

// Mouse: REL_X, REL_Y and BTN_LEFT without EV_ABS. Keyboard: letter or modifier keys. A node with both is a mouse,
// so it gets grabbed; its keys still count as keyboard keys.
DeviceKind deviceKind(const DeviceCaps& caps);

// Whether an input device's sysfs node (the resolved /sys/class/input/eventN) is virtual: uinput devices such as
// our own clones, ydotoold and Sunshine's passthrough, which reports a real bus type.
bool isVirtualDevice(const std::string& sysfsPath);

struct KeyTransition {
  uint32_t vk;
  bool down;
};

// Which keys are down across all keyboards: a VK is down while any device holds any key that maps to it.
// Turns each device's own key changes into the key-downs and key-ups Decision expects.
class KeyUnion {
 public:
  // One EV_KEY event from `device` (value 0 up, 1 down, 2 auto-repeat). A repeat of a held key is a key-down again,
  // as on Windows. Empty when the VK doesn't change or the key has no VK.
  std::vector<KeyTransition> onKey(int device, uint16_t code, int value);
  // `device` now holds exactly `codes` (when it is added, after a resync, or {} once it is gone): ups, then downs.
  std::vector<KeyTransition> setHeld(int device, const std::vector<uint16_t>& codes);
  bool isDown(uint32_t vk) const;

 private:
  std::set<uint32_t> downVks() const;
  std::map<int, std::set<uint16_t>> held_;  // device → its held keys that have a VK
};

// Whether to tap the mask key (KEY_F24 on the helper's own keyboard) after `e` was handled. Firefox and GTK open their
// menu bar on a lone Alt tap, a launcher may sit on a lone Super tap. Keyboards are only read, so the modifier's
// release can't be masked the Windows way; a key typed while it is still held makes the tap not lone.
bool shouldTapMask(const InputEvent& e, bool swallowed, const KeyUnion& keys);

// Parses Hyprland's reply to `j/cursorpos`, such as {"x": 4467, "y": 885} over several lines.
bool parseCursorPos(const std::string& reply, int& x, int& y);

// Gives Decision's emits their screen positions (docs/design-linux.md §2.5). Decision only sees motion summed from
// evdev; the compositor knows where the cursor is. The cursor thread calls this in the order the event thread
// queued presses and emits, and polls it every 8 ms while open().
class CursorEmits {
 public:
  using Query = std::function<bool(int& x, int& y)>;  // asks the compositor; false when it didn't answer

  explicit CursorEmits(Query query) : query_(std::move(query)) {}

  // A press started a gesture: a wheel or a click it turns into is placed where it happened.
  void onPress();
  // One of Decision's emits with its position filled in, or nothing: Decision's own wheelMoves are dropped.
  std::optional<Emit> onEmit(const Emit& e);
  // While a wheel is open: a wheelMove when the cursor moved since the last point sent.
  std::optional<Emit> onPoll();
  bool open() const { return open_; }

 private:
  void where(int& x, int& y);  // asks the compositor, or falls back to its last answer

  Query query_;
  bool open_ = false;
  WheelKind wheel_ = WheelKind::Ping;
  int pressX_ = 0, pressY_ = 0;
  int sentX_ = 0, sentY_ = 0;    // the last point sent while open
  int knownX_ = 0, knownY_ = 0;  // the compositor's last answer
};

}  // namespace lp::lnx
