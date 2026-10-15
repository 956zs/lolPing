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

// evdev event types and codes from linux/input-event-codes.h.
constexpr uint16_t kEvKey = 0x01, kEvRel = 0x02, kEvAbs = 0x03;
constexpr uint16_t kRelX = 0x00, kRelY = 0x01, kAbsX = 0x00, kAbsMtPositionX = 0x35;
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
  // ABS_X or ABS_MT_POSITION_X: an absolute pointer (touchpad, tablet, touchscreen). Other axes, such as the
  // ABS_MISC or ABS_VOLUME some gaming mice add to their pointer node, don't count.
  bool absPointer = false;
  bool keyA = false;
  bool keyLeftAlt = false;
};

// Whether a device reports an event type and code, as libevdev_has_event_code answers it.
using HasCode = std::function<bool(uint16_t type, uint16_t code)>;
DeviceCaps readCaps(const HasCode& has);

enum class DeviceKind { Other, Mouse, Keyboard };

// Mouse: REL_X, REL_Y and BTN_LEFT, and not an absolute pointer. Keyboard: letter or modifier keys. A node with both
// is a mouse, so it gets grabbed; its keys still count as keyboard keys.
DeviceKind deviceKind(const DeviceCaps& caps);

// Whether an input device's sysfs node (the resolved /sys/class/input/eventN) is a uinput device: our own clones,
// ydotoold, Sunshine's passthrough (which reports a real bus type). Those live under /sys/devices/virtual/input/.
// Bluetooth LE devices come through uhid, elsewhere under /sys/devices/virtual/, and are real.
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

// Whether the cursor at (x, y) is more than `thresholdPx` screen pixels from the press at (fromX, fromY): the same
// test Decision makes on Windows and macOS, where it gets screen coordinates.
bool movedPastThreshold(int fromX, int fromY, int x, int y, int thresholdPx);

// The move the event loop feeds Decision once the cursor thread reports that the pending press moved past the drag
// threshold on screen. Every other mouse event reaches Decision at (0, 0), so raw motion never opens a wheel.
InputEvent pastThresholdMove(int dragThresholdPx, uint64_t timeMs);

// A button released on a mouse that isn't grabbed. The compositor already has the release, so nothing is swallowed
// or replayed, but Decision sees it: a gesture cut short by a pause waits in SwallowUp for that release and would
// block every other trigger button until it came.
Result ungrabbedRelease(Decision& d, Btn btn, uint64_t timeMs);

// Gives Decision's emits their screen positions and measures the drag threshold on screen (docs/design-linux.md
// §2.5). evdev only reports relative motion, in device counts; the compositor knows where the cursor is. The cursor
// thread calls this in the order the event thread queued presses and emits, and polls it every 8 ms while polling().
//
// A window that locks the pointer (SDL's relative mouse mode, which osu!lazer uses for its high precision mouse)
// stops the compositor's cursor for good: it answers the press position however far the mouse moves. When that
// happens, the gesture is measured in raw motion instead, one pixel per count.
class CursorEmits {
 public:
  using Query = std::function<bool(int& x, int& y)>;  // asks the compositor; false when it didn't answer

  // Polls the compositor must answer exactly the press after raw motion passed the threshold, beyond the first,
  // before the pointer counts as locked. A busy compositor can be a frame behind the mouse; a locked one never
  // catches up.
  static constexpr int kLockConfirmPolls = 2;
  // Polls of an open wheel that find the cursor back exactly on the press while the mouse moved, before the
  // gesture counts as locked: Hyprland moves a locked cursor and warps it back, so a poll can catch it away.
  static constexpr int kReturnPolls = 2;
  // libinput's unit is about one pixel at 1000 DPI. The ratio could be measured from drags the compositor follows,
  // but a quick click measures it wrong (the last answer is stale, the counts are not), and a wrong ratio makes the
  // drag threshold unreachable; the direction is what the wheel needs, and that holds for any ratio.
  static constexpr double kPxPerCount = 1.0;

  explicit CursorEmits(Query query) : query_(std::move(query)) {}

  // A press Decision holds as pending: a wheel or a click it turns into is placed where it happened, and polls
  // watch it until it has moved more than `thresholdPx`.
  void onPress(int thresholdPx);
  // The pending press ended without an emit that says so (a replayed click, or the trigger let go).
  void onPressEnded();
  // Raw mouse motion since the last call, in device counts. Only counted while a press is pending or a wheel open.
  void onMotion(int dx, int dy);
  // One of Decision's emits with its position filled in, or nothing: Decision's own wheelMoves are dropped.
  std::optional<Emit> onEmit(const Emit& e);

  struct Poll {
    std::optional<Emit> move;  // an open wheel's cursor moved: send this wheelMove
    bool dragged = false;      // the pending press moved past its threshold: the event loop should open the wheel
  };
  Poll onPoll();
  bool polling() const { return open_ || pending_; }
  bool open() const { return open_; }
  // The current gesture is measured in raw motion because the compositor's cursor stayed at the press.
  bool locked() const { return locked_; }

 private:
  bool where(int& x, int& y);  // asks the compositor, or falls back to its last answer (false)
  void rawPoint(int& x, int& y) const;  // the press plus the scaled raw motion since
  bool rawPastThreshold() const;
  bool atPress(int x, int y) const;
  void gestureEnded();

  Query query_;
  bool open_ = false;
  bool pending_ = false;
  bool pressKnown_ = false;  // the compositor answered at the press; if not, the first poll that gets an answer sets it
  bool locked_ = false;
  bool movedSincePoll_ = false;
  int stillPolls_ = 0;   // pending polls with raw motion past the threshold and the compositor still at the press
  int returnPolls_ = 0;  // open polls that found the cursor back on the press while the mouse moved
  int thresholdPx_ = 0;
  WheelKind wheel_ = WheelKind::Ping;
  int pressX_ = 0, pressY_ = 0;
  int sentX_ = 0, sentY_ = 0;    // the last point sent while open
  int knownX_ = 0, knownY_ = 0;  // the compositor's last answer
  long rawX_ = 0, rawY_ = 0;     // raw motion since the press, in counts
};

}  // namespace lp::lnx
