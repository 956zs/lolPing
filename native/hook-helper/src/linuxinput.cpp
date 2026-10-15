#include "linuxinput.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "json.h"

namespace lp::lnx {
namespace {

struct KeyPair {
  uint16_t code;
  uint32_t vk;
};

// KEY_* from linux/input-event-codes.h.
constexpr KeyPair kKeys[] = {
    {30, 'A'}, {48, 'B'}, {46, 'C'}, {32, 'D'}, {18, 'E'}, {33, 'F'}, {34, 'G'}, {35, 'H'}, {23, 'I'}, {36, 'J'},
    {37, 'K'}, {38, 'L'}, {50, 'M'}, {49, 'N'}, {24, 'O'}, {25, 'P'}, {16, 'Q'}, {19, 'R'}, {31, 'S'}, {20, 'T'},
    {22, 'U'}, {47, 'V'}, {17, 'W'}, {45, 'X'}, {21, 'Y'}, {44, 'Z'},
    {11, '0'}, {2, '1'}, {3, '2'}, {4, '3'}, {5, '4'}, {6, '5'}, {7, '6'}, {8, '7'}, {9, '8'}, {10, '9'},
    {59, 0x70}, {60, 0x71}, {61, 0x72}, {62, 0x73}, {63, 0x74}, {64, 0x75}, {65, 0x76}, {66, 0x77},
    {67, 0x78}, {68, 0x79}, {87, 0x7A}, {88, 0x7B},                                  // F1-F12
    {183, 0x7C}, {184, 0x7D}, {185, 0x7E}, {186, 0x7F}, {187, 0x80}, {188, 0x81},
    {189, 0x82}, {190, 0x83}, {191, 0x84}, {192, 0x85}, {193, 0x86}, {194, 0x87},  // F13-F24
    {28, 0x0D}, {96, 0x0D},                                                          // Enter, keypad Enter
    {15, 0x09}, {57, 0x20}, {14, 0x08}, {1, 0x1B}, {111, 0x2E}, {110, 0x2D},         // Tab Space ⌫ Esc Del Ins
    {102, 0x24}, {107, 0x23}, {104, 0x21}, {109, 0x22},                              // Home End PgUp PgDn
    {105, 0x25}, {103, 0x26}, {106, 0x27}, {108, 0x28},                              // arrows
    {41, 0xC0}, {12, 0xBD}, {13, 0xBB}, {26, 0xDB}, {27, 0xDD}, {43, 0xDC},          // ` - = [ ] backslash
    {39, 0xBA}, {40, 0xDE}, {51, 0xBC}, {52, 0xBE}, {53, 0xBF}, {86, 0xE2},          // ; ' , . / and ISO <>
    {82, 0x60}, {79, 0x61}, {80, 0x62}, {81, 0x63}, {75, 0x64}, {76, 0x65}, {77, 0x66},
    {71, 0x67}, {72, 0x68}, {73, 0x69},                                              // keypad 0-9
    {55, 0x6A}, {78, 0x6B}, {74, 0x6D}, {83, 0x6E}, {98, 0x6F},                      // keypad * + - . /
    {58, 0x14}, {69, 0x90}, {70, 0x91}, {99, 0x2C}, {119, 0x13}, {127, 0x5D},        // locks, PrtSc, Pause, Menu
    {42, 0xA0}, {54, 0xA1}, {29, 0xA2}, {97, 0xA3}, {56, 0xA4}, {100, 0xA5},         // modifiers
    {125, 0x5B}, {126, 0x5C},
};

constexpr uint32_t kVkLAlt = 0xA4, kVkRAlt = 0xA5, kVkLWin = 0x5B, kVkRWin = 0x5C;

// A JSON number as Hyprland writes it: optional minus, digits, optional fraction. No exponent, hex or inf.
bool toCoord(const std::string& s, int& out) {
  size_t i = s.size() > 0 && s[0] == '-' ? 1 : 0;
  const size_t digits = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
  if (i == digits || i - digits > 9) return false;
  if (i < s.size() && s[i] == '.') {
    const size_t frac = ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == frac) return false;
  }
  if (i != s.size()) return false;
  out = static_cast<int>(std::lround(std::strtod(s.c_str(), nullptr)));
  return true;
}

}  // namespace

uint32_t keyCodeToVk(uint16_t code) {
  for (const auto& k : kKeys) {
    if (k.code == code) return k.vk;
  }
  return 0;
}

Btn buttonOf(uint16_t code) {
  switch (code) {
    case kBtnLeft: return Btn::Left;
    case kBtnRight: return Btn::Right;
    case kBtnMiddle: return Btn::Middle;
    case kBtnSide: return Btn::X1;
    case kBtnExtra: return Btn::X2;
    default: return Btn::None;
  }
}

uint16_t buttonCode(Btn btn) {
  switch (btn) {
    case Btn::Left: return kBtnLeft;
    case Btn::Right: return kBtnRight;
    case Btn::Middle: return kBtnMiddle;
    case Btn::X1: return kBtnSide;
    case Btn::X2: return kBtnExtra;
    default: return 0;
  }
}

DeviceCaps readCaps(const HasCode& has) {
  DeviceCaps c;
  c.relX = has(kEvRel, kRelX);
  c.relY = has(kEvRel, kRelY);
  c.btnLeft = has(kEvKey, kBtnLeft);
  c.absPointer = has(kEvAbs, kAbsX) || has(kEvAbs, kAbsMtPositionX);
  c.keyA = has(kEvKey, kKeyA);
  c.keyLeftAlt = has(kEvKey, kKeyLeftAlt);
  return c;
}

DeviceKind deviceKind(const DeviceCaps& c) {
  if (c.relX && c.relY && c.btnLeft && !c.absPointer) return DeviceKind::Mouse;
  if (c.keyA || c.keyLeftAlt) return DeviceKind::Keyboard;
  return DeviceKind::Other;
}

bool isVirtualDevice(const std::string& sysfsPath) { return sysfsPath.rfind("/sys/devices/virtual/input/", 0) == 0; }

std::set<uint32_t> KeyUnion::downVks() const {
  std::set<uint32_t> vks;
  for (const auto& device : held_) {
    for (const uint16_t code : device.second) vks.insert(keyCodeToVk(code));
  }
  return vks;
}

bool KeyUnion::isDown(uint32_t vk) const {
  for (const auto& device : held_) {
    for (const uint16_t code : device.second) {
      if (keyCodeToVk(code) == vk) return true;
    }
  }
  return false;
}

std::vector<KeyTransition> KeyUnion::onKey(int device, uint16_t code, int value) {
  const uint32_t vk = keyCodeToVk(code);
  if (vk == 0) return {};
  const bool was = isDown(vk);
  if (value == 0) {
    held_[device].erase(code);
    if (was && !isDown(vk)) return {{vk, false}};
    return {};
  }
  const bool fresh = held_[device].insert(code).second;
  // A second keyboard pressing a key that is already down changes nothing; this keyboard's own repeat does.
  if (!was || (value == 2 && !fresh)) return {{vk, true}};
  return {};
}

std::vector<KeyTransition> KeyUnion::setHeld(int device, const std::vector<uint16_t>& codes) {
  const std::set<uint32_t> before = downVks();
  std::set<uint16_t> mine;
  for (const uint16_t code : codes) {
    if (keyCodeToVk(code) != 0) mine.insert(code);
  }
  if (mine.empty()) held_.erase(device);
  else held_[device] = std::move(mine);
  const std::set<uint32_t> after = downVks();
  std::vector<KeyTransition> out;
  for (const uint32_t vk : before) {
    if (after.count(vk) == 0) out.push_back({vk, false});
  }
  for (const uint32_t vk : after) {
    if (before.count(vk) == 0) out.push_back({vk, true});
  }
  return out;
}

bool shouldTapMask(const InputEvent& e, bool swallowed, const KeyUnion& keys) {
  if (e.kind != EvKind::MouseDown || !swallowed) return false;
  return keys.isDown(kVkLAlt) || keys.isDown(kVkRAlt) || keys.isDown(kVkLWin) || keys.isDown(kVkRWin);
}

bool parseCursorPos(const std::string& reply, int& x, int& y) {
  std::map<std::string, std::string> m;
  if (!parseFlatJson(reply, m)) return false;
  const auto ix = m.find("x");
  const auto iy = m.find("y");
  int px = 0;
  int py = 0;
  if (ix == m.end() || iy == m.end() || !toCoord(ix->second, px) || !toCoord(iy->second, py)) return false;
  x = px;
  y = py;
  return true;
}

bool movedPastThreshold(int fromX, int fromY, int x, int y, int thresholdPx) {
  return std::hypot(static_cast<double>(x - fromX), static_cast<double>(y - fromY)) > thresholdPx;
}

InputEvent pastThresholdMove(int dragThresholdPx, uint64_t timeMs) {
  InputEvent e;
  e.kind = EvKind::MouseMove;
  e.x = dragThresholdPx + 1;
  e.timeMs = timeMs;
  return e;
}

Result ungrabbedRelease(Decision& d, Btn btn, uint64_t timeMs) {
  InputEvent e;
  e.kind = EvKind::MouseUp;
  e.btn = btn;
  e.timeMs = timeMs;
  Result r = d.onEvent(e);
  r.swallow = false;
  r.injects.clear();
  return r;
}

void CursorTracker::setScreens(std::vector<ScreenRect> screens) {
  screens_ = std::move(screens);
  if (locked_) clamp();
}

void CursorTracker::setSensitivity(double pxPerCount) { sensitivity_ = pxPerCount; }

void CursorTracker::onMotion(int dx, int dy) {
  rawX_ += dx;
  rawY_ += dy;
}

void CursorTracker::takeRaw(long& dx, long& dy) {
  dx = rawX_;
  dy = rawY_;
  rawX_ = 0;
  rawY_ = 0;
}

// Keeps the locked window's cursor on a screen, as the window itself does: the nearest point of the screen it is
// on or closest to. Without screens there is nothing to keep it on.
void CursorTracker::clamp() {
  if (screens_.empty()) return;
  double bestX = vx_;
  double bestY = vy_;
  double bestDist = -1;
  for (const auto& s : screens_) {
    const double cx = std::min(std::max(vx_, static_cast<double>(s.x)), static_cast<double>(s.x + s.width - 1));
    const double cy = std::min(std::max(vy_, static_cast<double>(s.y)), static_cast<double>(s.y + s.height - 1));
    const double dist = std::hypot(cx - vx_, cy - vy_);
    if (bestDist < 0 || dist < bestDist) {
      bestDist = dist;
      bestX = cx;
      bestY = cy;
    }
  }
  vx_ = bestX;
  vy_ = bestY;
}

void CursorTracker::onAnswer(int x, int y) {
  long dx = 0;
  long dy = 0;
  takeRaw(dx, dy);
  const bool moved = dx != 0 || dy != 0;
  if (!known_) {
    known_ = true;
    lastX_ = x;
    lastY_ = y;
    return;
  }
  if (!locked_) {
    if (x != lastX_ || y != lastY_) {  // the cursor follows the mouse
      lastX_ = x;
      lastY_ = y;
      stillPolls_ = 0;
      stillRawX_ = 0;
      stillRawY_ = 0;
    } else if (moved) {  // the mouse moved, the cursor didn't: locked, once that holds for a few polls
      stillRawX_ += dx;
      stillRawY_ += dy;
      if (++stillPolls_ >= kStillPolls) {
        locked_ = true;
        lockX_ = x;
        lockY_ = y;
        awayPolls_ = 0;
        // The window took the cursor's position at the lock and has moved its own by the raw motion since.
        vx_ = x + static_cast<double>(stillRawX_) * sensitivity_;
        vy_ = y + static_cast<double>(stillRawY_) * sensitivity_;
        clamp();
      }
    }
    return;
  }
  vx_ += static_cast<double>(dx) * sensitivity_;
  vy_ += static_cast<double>(dy) * sensitivity_;
  clamp();
  if (x == lockX_ && y == lockY_) {
    awayPolls_ = 0;
    return;
  }
  // Away from the lock point: caught mid-warp once, unlocked (or warped elsewhere) when it stays away.
  if (++awayPolls_ >= kAwayPolls) {
    locked_ = false;
    lastX_ = x;
    lastY_ = y;
    stillPolls_ = 0;
    stillRawX_ = 0;
    stillRawY_ = 0;
  }
}

void CursorTracker::onNoAnswer() {
  long dx = 0;
  long dy = 0;
  takeRaw(dx, dy);
  if (!locked_) return;
  vx_ += static_cast<double>(dx) * sensitivity_;
  vy_ += static_cast<double>(dy) * sensitivity_;
  clamp();
}

void CursorTracker::resync(int x, int y) {
  if (!locked_) return;
  vx_ = x;
  vy_ = y;
  clamp();
}

bool CursorTracker::position(int& x, int& y) const {
  if (!known_) return false;
  if (locked_) {
    x = static_cast<int>(std::lround(vx_));
    y = static_cast<int>(std::lround(vy_));
  } else {
    x = lastX_;
    y = lastY_;
  }
  return true;
}

void CursorEmits::setConfig(const Config& c) {
  tracker_.setScreens(c.screens);
  tracker_.setSensitivity(c.lockedSensitivity);
}

bool CursorEmits::sample() {
  int x = 0;
  int y = 0;
  const bool answered = query_ && query_(x, y);
  if (answered) tracker_.onAnswer(x, y);
  else tracker_.onNoAnswer();
  return answered;
}

void CursorEmits::onPress(int thresholdPx) {
  // Without an answer the press is placed at the last known position, but measured from the first answer: the
  // last one may be old.
  pressKnown_ = sample();
  where(pressX_, pressY_);
  thresholdPx_ = thresholdPx;
  pending_ = true;
  rawX_ = 0;
  rawY_ = 0;
}

void CursorEmits::onPressEnded() { pending_ = false; }

void CursorEmits::onMotion(int dx, int dy) {
  tracker_.onMotion(dx, dy);
  if (polling()) {
    rawX_ += dx;
    rawY_ += dy;
  }
}

std::optional<Emit> CursorEmits::onEmit(const Emit& e) {
  Emit out = e;
  switch (e.kind) {
    case Emit::Kind::WheelOpen:
      pending_ = false;
      open_ = true;
      wheel_ = e.wheel;
      out.x = pressX_;
      out.y = pressY_;
      sentX_ = pressX_;
      sentY_ = pressY_;
      return out;
    case Emit::Kind::Click:
      pending_ = false;
      out.x = pressX_;
      out.y = pressY_;
      return out;
    case Emit::Kind::WheelMove:
      return std::nullopt;  // the poll sends real positions instead
    case Emit::Kind::WheelRelease: {
      open_ = false;
      sample();
      where(out.x, out.y);
      // The pointer got locked during this gesture and the tracker hasn't caught on yet: the compositor answers
      // the press however far the mouse went. The raw motion says where.
      const int rx = pressX_ + static_cast<int>(std::lround(static_cast<double>(rawX_)));
      const int ry = pressY_ + static_cast<int>(std::lround(static_cast<double>(rawY_)));
      if (pressKnown_ && !tracker_.locked() && out.x == pressX_ && out.y == pressY_ &&
          movedPastThreshold(pressX_, pressY_, rx, ry, thresholdPx_)) {
        out.x = rx;
        out.y = ry;
      }
      return out;
    }
    case Emit::Kind::Cancel:
      pending_ = false;
      open_ = false;
      return out;
    case Emit::Kind::Toggled:
      return out;
  }
  return out;
}

CursorEmits::Poll CursorEmits::onPoll() {
  Poll p;
  const bool wasLocked = tracker_.locked();
  sample();
  int x = 0;
  int y = 0;
  if (!polling() || !where(x, y)) return p;
  // The lock was noticed during this gesture: the wheel sits on the lock point, and the raw motion since the press
  // says where the window's cursor went from there, including what the first polls took for a drag.
  if (!wasLocked && tracker_.locked() && pressKnown_) {
    tracker_.resync(pressX_ + static_cast<int>(rawX_), pressY_ + static_cast<int>(rawY_));
    where(x, y);
  }
  if (pending_) {
    if (!pressKnown_) {  // no answer at the press: measure from the first one
      pressX_ = x;
      pressY_ = y;
      pressKnown_ = true;
    } else if (movedPastThreshold(pressX_, pressY_, x, y, thresholdPx_)) {
      pending_ = false;  // once: the wheel's open emit or the press's end comes next
      p.dragged = true;
    }
    return p;
  }
  if (x == sentX_ && y == sentY_) return p;
  sentX_ = x;
  sentY_ = y;
  Emit move{Emit::Kind::WheelMove};
  move.x = x;
  move.y = y;
  move.wheel = wheel_;
  p.move = move;
  return p;
}

}  // namespace lp::lnx
