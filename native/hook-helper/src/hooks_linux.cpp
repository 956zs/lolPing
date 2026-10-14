// Linux input layer (Hyprland): mice are grabbed and forwarded through uinput clones, keyboards are only read.
// Needs the input group and write access to /dev/uinput. See docs/design-linux.md §2.
#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app.h"
#include "commands.h"
#include "linuxinput.h"

namespace lp {
namespace {

constexpr int kExitFailed = 2;
constexpr int kExitNoAccess = 3;  // Electron treats this exit code as "can't run here", not as a crash
constexpr int kExitStalled = 5;
constexpr uint64_t kTickMs = 1000;
// A new clone is ignored until the compositor has opened it: grabbing first would lose the events in between.
constexpr uint64_t kCloneSettleMs = 250;
constexpr uint64_t kStallMs = 1000;
constexpr auto kCursorPoll = std::chrono::milliseconds(8);
constexpr auto kWatchdogPoll = std::chrono::milliseconds(250);
constexpr const char* kInputDir = "/dev/input";

uint64_t nowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string errnoText(int err) { return std::strerror(err); }

// $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock, or "" outside a Hyprland session.
std::string hyprSocketPath() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  const char* sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (runtime == nullptr || sig == nullptr || *runtime == '\0' || *sig == '\0') return "";
  std::string path = std::string(runtime) + "/hypr/" + sig + "/.socket.sock";
  return path.size() < sizeof(sockaddr_un{}.sun_path) ? path : "";
}

// One request on Hyprland's command socket, which answers and closes. A round trip takes up to a few ms, so only
// the cursor thread (and startup) call this, never the event loop.
bool hyprRequest(const std::string& path, const std::string& request, std::string& reply) {
  const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s < 0) return false;
  const timeval timeout{1, 0};  // a hung compositor must not hang the cursor thread for good
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  bool ok = ::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 &&
            ::send(s, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size());
  reply.clear();
  char buf[512];
  while (ok) {
    const ssize_t n = ::read(s, buf, sizeof(buf));
    if (n == 0) break;
    if (n < 0) {
      ok = errno == EINTR;
      continue;
    }
    reply.append(buf, static_cast<size_t>(n));
    ok = reply.size() <= 4096;
  }
  ::close(s);
  return ok;
}

bool queryCursor(const std::string& socket, int& x, int& y) {
  std::string reply;
  return hyprRequest(socket, "j/cursorpos", reply) && lnx::parseCursorPos(reply, x, y);
}

// Owns the Hyprland socket: fills in the positions of Decision's emits, in order, and polls the cursor while a
// wheel is open. The event loop only queues work here, so a slow compositor never delays a forwarded event.
class CursorThread {
 public:
  CursorThread(std::string socket, Output& out) : socket_(std::move(socket)), out_(out), thread_([this] { run(); }) {}
  ~CursorThread() { stop(); }
  CursorThread(const CursorThread&) = delete;
  CursorThread& operator=(const CursorThread&) = delete;

  void press() { push(Item{true, Emit{Emit::Kind::Cancel}}); }
  void emit(const Emit& e) { push(Item{false, e}); }
  // Sends what is queued, then joins.
  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stopping_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
  }

 private:
  struct Item {
    bool press;
    Emit emit;
  };

  void push(const Item& item) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      q_.push_back(item);
    }
    cv_.notify_one();
  }

  void run() {
    using Clock = std::chrono::steady_clock;
    lnx::CursorEmits placer([this](int& x, int& y) { return queryCursor(socket_, x, y); });
    auto nextPoll = Clock::now();
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
      const auto ready = [this] { return stopping_ || !q_.empty(); };
      if (q_.empty() && stopping_) return;
      if (placer.open()) cv_.wait_until(lk, nextPoll, ready);
      else cv_.wait(lk, ready);
      std::optional<Item> item;
      if (!q_.empty()) {
        item = q_.front();
        q_.pop_front();
      }
      lk.unlock();
      if (item && item->press) {
        placer.onPress();
      } else if (item) {
        if (const auto e = placer.onEmit(item->emit)) {
          out_.emit(*e);
          if (e->kind == Emit::Kind::WheelOpen) nextPoll = Clock::now();  // the first move right behind the open
        }
      }
      if (placer.open() && Clock::now() >= nextPoll) {
        if (const auto m = placer.onPoll()) out_.emit(*m);
        nextPoll = Clock::now() + kCursorPoll;
      }
      lk.lock();
    }
  }

  std::string socket_;
  Output& out_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> q_;
  bool stopping_ = false;
  std::thread thread_;  // declared last: starts after everything above is initialised
};

// The pointer passes through this process, so a stalled event loop would freeze it. When a grabbed mouse has had
// unread input for a second with no loop progress, exit: the kernel drops the grabs and Electron restarts us.
class Watchdog {
 public:
  Watchdog() : thread_([this] { run(); }) {}
  ~Watchdog() { stop(); }
  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  void advance() { loops_.fetch_add(1, std::memory_order_relaxed); }
  // Grabbed mice. An fd is unwatched before it is closed, so a reused number is never polled.
  void watch(int fd) {
    std::lock_guard<std::mutex> lk(fdMu_);
    fds_.push_back(fd);
  }
  void unwatch(int fd) {
    std::lock_guard<std::mutex> lk(fdMu_);
    fds_.erase(std::remove(fds_.begin(), fds_.end(), fd), fds_.end());
  }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(stopMu_);
      stopping_ = true;
    }
    stopCv_.notify_one();
    if (thread_.joinable()) thread_.join();
  }

 private:
  bool mouseInputWaiting() {
    std::lock_guard<std::mutex> lk(fdMu_);
    std::vector<pollfd> p;
    for (const int fd : fds_) p.push_back({fd, POLLIN, 0});
    if (p.empty() || ::poll(p.data(), p.size(), 0) <= 0) return false;
    return std::any_of(p.begin(), p.end(), [](const pollfd& f) { return (f.revents & POLLIN) != 0; });
  }

  void run() {
    uint64_t seen = loops_.load(std::memory_order_relaxed);
    std::optional<uint64_t> stalledSince;
    std::unique_lock<std::mutex> lk(stopMu_);
    while (!stopCv_.wait_for(lk, kWatchdogPoll, [this] { return stopping_; })) {
      const uint64_t loops = loops_.load(std::memory_order_relaxed);
      // An idle loop doesn't advance either: only input that waits unread counts as a stall.
      if (loops != seen || !mouseInputWaiting()) {
        seen = loops;
        stalledSince.reset();
        continue;
      }
      const uint64_t now = nowMs();
      if (!stalledSince) stalledSince = now;
      else if (now - *stalledSince >= kStallMs) _exit(kExitStalled);
    }
  }

  std::atomic<uint64_t> loops_{0};
  std::mutex fdMu_;
  std::vector<int> fds_;
  std::mutex stopMu_;
  std::condition_variable stopCv_;
  bool stopping_ = false;
  std::thread thread_;  // declared last
};

struct Device {
  int id = 0;
  std::string node;  // /dev/input/eventN
  int fd = -1;
  libevdev* dev = nullptr;
  bool mouse = false;
  bool dead = false;                 // unplugged: removed after this loop pass
  libevdev_uinput* clone = nullptr;  // mice, while lolPing is on
  bool grabbed = false;
  uint64_t grabAfterMs = 0;          // clone made, real mouse not grabbed yet
  std::vector<input_event> frame;    // passed events of the current frame, written at its SYN_REPORT
  int dx = 0;                        // motion of this frame not yet fed to Decision
  int dy = 0;
};

Decision* g_decision = nullptr;
Output* g_out = nullptr;
CursorThread* g_cursor = nullptr;
Watchdog* g_watchdog = nullptr;
lnx::KeyUnion g_keys;
std::vector<std::unique_ptr<Device>> g_devices;
int g_nextId = 1;
bool g_grabbing = false;              // mice are cloned and grabbed while Decision is enabled
int g_gestureMouse = 0;               // the mouse whose press Decision holds: replays go to its clone
libevdev_uinput* g_maskKeyboard = nullptr;
// Decision's coordinates: raw motion summed over every mouse. Only distances matter (the drag threshold); real
// screen positions come from the cursor thread.
int g_x = 0;
int g_y = 0;

bool isKeyDown(uint32_t vk) { return g_keys.isDown(vk); }

Device* findDevice(int id) {
  for (auto& d : g_devices) {
    if (d->id == id && !d->dead) return d.get();
  }
  return nullptr;
}

void writeEvent(const libevdev_uinput* ui, unsigned type, unsigned code, int value) {
  libevdev_uinput_write_event(ui, type, code, value);
}

void flushFrame(Device& d) {
  if (!d.frame.empty() && d.clone != nullptr) {
    for (const auto& ev : d.frame) writeEvent(d.clone, ev.type, ev.code, ev.value);
    writeEvent(d.clone, EV_SYN, SYN_REPORT, 0);
  }
  d.frame.clear();
}

void replayButton(Btn btn, bool down) {
  Device* m = findDevice(g_gestureMouse);
  const uint16_t code = lnx::buttonCode(btn);
  if (m == nullptr || m->clone == nullptr || code == 0) return;
  flushFrame(*m);  // the replay goes where the swallowed event was, after what came before it
  writeEvent(m->clone, EV_KEY, code, down ? 1 : 0);
  writeEvent(m->clone, EV_SYN, SYN_REPORT, 0);
}

void tapMask() {
  if (g_maskKeyboard == nullptr) return;
  writeEvent(g_maskKeyboard, EV_KEY, KEY_F24, 1);
  writeEvent(g_maskKeyboard, EV_SYN, SYN_REPORT, 0);
  writeEvent(g_maskKeyboard, EV_KEY, KEY_F24, 0);
  writeEvent(g_maskKeyboard, EV_SYN, SYN_REPORT, 0);
}

void startClone(Device& d) {
  if (d.clone != nullptr) return;
  const int rc = libevdev_uinput_create_from_device(d.dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &d.clone);
  if (rc < 0) {
    d.clone = nullptr;
    g_out->error("can't clone " + std::string(libevdev_get_name(d.dev)) + ": " + errnoText(-rc));
    return;
  }
  d.grabAfterMs = nowMs() + kCloneSettleMs;
}

// Gives the mouse back to the compositor: a paused lolPing is not in the input path at all.
void release(Device& d) {
  if (d.grabbed) {
    flushFrame(d);
    libevdev_grab(d.dev, LIBEVDEV_UNGRAB);
    g_watchdog->unwatch(d.fd);
    d.grabbed = false;
  }
  if (d.clone != nullptr) {
    libevdev_uinput_destroy(d.clone);  // libinput releases whatever the clone still held
    d.clone = nullptr;
  }
  d.frame.clear();
  d.dx = 0;
  d.dy = 0;
}

bool anyKeyHeld(const Device& d) {
  for (unsigned code = 0; code <= KEY_MAX; ++code) {
    if (libevdev_get_event_value(d.dev, EV_KEY, code) != 0) return true;
  }
  return false;
}

// A mouse is grabbed only while none of its buttons is held: the compositor saw that press and must see its release.
void grabSettled(uint64_t now) {
  for (auto& p : g_devices) {
    Device& d = *p;
    if (d.dead || d.clone == nullptr || d.grabbed || now < d.grabAfterMs || anyKeyHeld(d)) continue;
    const int rc = libevdev_grab(d.dev, LIBEVDEV_GRAB);
    if (rc < 0) {  // another program holds it (EBUSY): leave it alone until lolPing is turned on again
      g_out->error("can't grab " + std::string(libevdev_get_name(d.dev)) + ": " + errnoText(-rc));
      release(d);
      continue;
    }
    d.grabbed = true;
    g_watchdog->watch(d.fd);
  }
}

void updateGrabs() {
  const bool want = g_decision->enabled();
  if (want == g_grabbing) return;
  g_grabbing = want;
  for (auto& d : g_devices) {
    if (!d->mouse || d->dead) continue;
    if (want) startClone(*d);
    else release(*d);
  }
}

void apply(const Result& r) {
  for (const auto& e : r.emits) g_cursor->emit(e);
  for (const auto& i : r.injects) {
    // Key injects (a tapped custom trigger key) have nothing to do: keyboards are only read, so no key was held back.
    if (i.kind == InjectKind::ButtonDown) replayButton(i.btn, true);
    if (i.kind == InjectKind::ButtonUp) replayButton(i.btn, false);
  }
  updateGrabs();
}

// Decision may ask to swallow a key; the key has already reached the compositor, so only its emits matter.
void feedKeys(const std::vector<lnx::KeyTransition>& keys) {
  for (const auto& k : keys) {
    InputEvent e;
    e.kind = k.down ? EvKind::KeyDown : EvKind::KeyUp;
    e.vk = k.vk;
    e.timeMs = nowMs();
    apply(g_decision->onEvent(e));
  }
}

void feedMotion(Device& d) {
  if (d.dx == 0 && d.dy == 0) return;
  if (g_decision->state() == Decision::State::Idle) {  // no gesture to measure: restart the sum, it never overflows
    g_x = 0;
    g_y = 0;
  }
  g_x += d.dx;
  g_y += d.dy;
  d.dx = 0;
  d.dy = 0;
  InputEvent e;
  e.kind = EvKind::MouseMove;
  e.x = g_x;
  e.y = g_y;
  e.timeMs = nowMs();
  apply(g_decision->onEvent(e));  // moves always pass
}

void handleButton(Device& d, const input_event& ev, Btn btn) {
  feedMotion(d);  // the press lands after the motion before it
  InputEvent e;
  e.kind = ev.value != 0 ? EvKind::MouseDown : EvKind::MouseUp;
  e.btn = btn;
  e.x = g_x;
  e.y = g_y;
  e.timeMs = nowMs();
  if (e.kind == EvKind::MouseDown) g_decision->syncKeyStates(isKeyDown);
  const Result r = g_decision->onEvent(e);
  if (e.kind == EvKind::MouseDown && r.swallow && g_decision->state() == Decision::State::Pending) {
    g_gestureMouse = d.id;
    g_cursor->press();
  }
  if (!r.swallow) d.frame.push_back(ev);
  if (lnx::shouldTapMask(e, r.swallow, g_keys)) tapMask();
  apply(r);
}

void handleEvent(Device& d, const input_event& ev) {
  const Btn btn = ev.type == EV_KEY ? lnx::buttonOf(ev.code) : Btn::None;
  // Keys count wherever they come from: some mice have a keyboard part on the same node.
  if (ev.type == EV_KEY && btn == Btn::None) feedKeys(g_keys.onKey(d.id, ev.code, ev.value));
  if (!d.mouse || !d.grabbed) return;  // the compositor reads it directly
  if (btn != Btn::None && (ev.value == 0 || ev.value == 1)) {
    handleButton(d, ev, btn);
  } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
    feedMotion(d);
    flushFrame(d);
  } else {
    if (ev.type == EV_REL && ev.code == REL_X) d.dx += ev.value;
    if (ev.type == EV_REL && ev.code == REL_Y) d.dy += ev.value;
    d.frame.push_back(ev);
  }
}

void readDevice(Device& d) {
  unsigned flags = LIBEVDEV_READ_FLAG_NORMAL;
  bool resynced = false;
  input_event ev;
  while (true) {
    const int rc = libevdev_next_event(d.dev, flags, &ev);
    if (rc == -EAGAIN && flags == LIBEVDEV_READ_FLAG_SYNC) {
      flags = LIBEVDEV_READ_FLAG_NORMAL;
      resynced = true;
      continue;
    }
    if (rc == -EAGAIN || rc == -EINTR) break;
    if (rc < 0) {  // ENODEV: unplugged
      d.dead = true;
      break;
    }
    // We fell behind and the kernel dropped events: libevdev hands out the difference to the real state.
    if (rc == LIBEVDEV_READ_STATUS_SYNC && flags == LIBEVDEV_READ_FLAG_NORMAL) {
      flags = LIBEVDEV_READ_FLAG_SYNC;
      continue;
    }
    handleEvent(d, ev);
  }
  if (resynced) g_decision->syncKeyStates(isKeyDown);
}

// Skips uinput devices (our clones, ydotoold, Sunshine) and nodes whose sysfs entry can't be read: grabbing one of
// our own clones would clone it again, without end.
bool isVirtualNode(const std::string& node) {
  const std::string sys = "/sys/class/input/" + node.substr(node.rfind('/') + 1);
  char* resolved = ::realpath(sys.c_str(), nullptr);
  if (resolved == nullptr) return true;
  const bool virt = lnx::isVirtualDevice(resolved);
  std::free(resolved);
  return virt;
}

// Opens and sorts one /dev/input/eventN. `denied` is set when it couldn't be opened for lack of permission.
void addDevice(const std::string& node, bool& denied) {
  for (const auto& d : g_devices) {
    if (d->node == node && !d->dead) return;
  }
  if (isVirtualNode(node)) return;
  const int fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (errno == EACCES || errno == EPERM) denied = true;
    return;
  }
  libevdev* dev = nullptr;
  if (libevdev_new_from_fd(fd, &dev) < 0) {
    ::close(fd);
    return;
  }
  lnx::DeviceCaps caps;
  caps.relX = libevdev_has_event_code(dev, EV_REL, REL_X) != 0;
  caps.relY = libevdev_has_event_code(dev, EV_REL, REL_Y) != 0;
  caps.btnLeft = libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) != 0;
  caps.abs = libevdev_has_event_type(dev, EV_ABS) != 0;
  caps.keyA = libevdev_has_event_code(dev, EV_KEY, KEY_A) != 0;
  caps.keyLeftAlt = libevdev_has_event_code(dev, EV_KEY, KEY_LEFTALT) != 0;
  const lnx::DeviceKind kind = lnx::deviceKind(caps);
  if (kind == lnx::DeviceKind::Other) {
    libevdev_free(dev);
    ::close(fd);
    return;
  }
  auto d = std::make_unique<Device>();
  d->id = g_nextId++;
  d->node = node;
  d->fd = fd;
  d->dev = dev;
  d->mouse = kind == lnx::DeviceKind::Mouse;
  Device& added = *d;
  g_devices.push_back(std::move(d));
  std::vector<uint16_t> held;  // a modifier may already be down
  for (unsigned code = 0; code <= KEY_MAX; ++code) {
    if (libevdev_get_event_value(dev, EV_KEY, code) != 0) held.push_back(static_cast<uint16_t>(code));
  }
  feedKeys(g_keys.setHeld(added.id, held));
  if (added.mouse && g_grabbing) startClone(added);
}

void dropDevice(Device& d) {
  release(d);
  feedKeys(g_keys.setHeld(d.id, {}));
  libevdev_free(d.dev);
  ::close(d.fd);
}

void scanDevices(bool& denied) {
  DIR* dir = ::opendir(kInputDir);
  if (dir == nullptr) {
    if (errno == EACCES) denied = true;
    return;
  }
  while (const dirent* e = ::readdir(dir)) {
    if (std::strncmp(e->d_name, "event", 5) == 0) addDevice(std::string(kInputDir) + "/" + e->d_name, denied);
  }
  ::closedir(dir);
}

// udev sets a new node's permissions after it appears, hence IN_ATTRIB as well as IN_CREATE.
void readHotplug(int fd) {
  alignas(inotify_event) char buf[4096];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) return;
    for (ssize_t off = 0; off < n;) {
      const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
      bool denied = false;
      if ((ev->mask & IN_Q_OVERFLOW) != 0) scanDevices(denied);
      else if (ev->len > 0 && std::strncmp(ev->name, "event", 5) == 0)
        addDevice(std::string(kInputDir) + "/" + ev->name, denied);
      off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
    }
  }
}

libevdev_uinput* createMaskKeyboard() {
  libevdev* dev = libevdev_new();
  if (dev == nullptr) return nullptr;
  libevdev_set_name(dev, "lolPing mask key");
  libevdev_enable_event_type(dev, EV_KEY);
  libevdev_enable_event_code(dev, EV_KEY, KEY_F24, nullptr);
  libevdev_uinput* ui = nullptr;
  const int rc = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &ui);
  libevdev_free(dev);  // the uinput device keeps no reference to it
  return rc == 0 ? ui : nullptr;
}

// Returns false on shutdown.
bool handleCommand(const std::string& text) {
  const Command c = parseCommand(text, g_decision->config());
  switch (c.kind) {
    case Command::Kind::Config: apply(g_decision->setConfig(c.config)); break;
    case Command::Kind::SetEnabled: apply(g_decision->setEnabled(c.flag)); break;
    case Command::Kind::Suspend: apply(g_decision->setSuspended(c.flag)); break;  // grabs stay: Decision passes all
    case Command::Kind::Shutdown: return false;
    default: g_out->error("invalid command: " + text); break;
  }
  return true;
}

// Reads what stdin has and runs every complete line. Returns false once stdin is closed (Electron is gone) or on
// shutdown.
bool readStdin(std::string& pending) {
  char buf[4096];
  const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
  if (n < 0) return errno == EINTR || errno == EAGAIN;
  if (n == 0) return false;
  pending.append(buf, static_cast<size_t>(n));
  for (size_t nl = pending.find('\n'); nl != std::string::npos; nl = pending.find('\n')) {
    std::string line = pending.substr(0, nl);
    pending.erase(0, nl + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!handleCommand(line)) return false;
  }
  return true;
}

int pollTimeout(uint64_t now, uint64_t nextTick) {
  uint64_t due = nextTick;
  for (const auto& d : g_devices) {
    if (d->clone != nullptr && !d->grabbed && d->grabAfterMs > now) due = std::min(due, d->grabAfterMs);
  }
  return due > now ? static_cast<int>(due - now) : 0;
}

// Returns the exit code: 0 once stdin closes or on shutdown.
int runLoop(int inotifyFd) {
  std::string pending;
  std::vector<pollfd> fds;
  std::vector<Device*> owners;  // owners[i] reads fds[i + first]
  uint64_t nextTick = nowMs() + kTickMs;
  while (true) {
    fds.clear();
    owners.clear();
    fds.push_back({STDIN_FILENO, POLLIN, 0});
    if (inotifyFd >= 0) fds.push_back({inotifyFd, POLLIN, 0});
    const size_t first = fds.size();
    // Keyboards first: when a key and a click wait together, the key (Alt) almost always came first.
    for (const bool mice : {false, true}) {
      for (auto& d : g_devices) {
        if (d->mouse != mice) continue;
        fds.push_back({d->fd, POLLIN, 0});
        owners.push_back(d.get());
      }
    }
    const int n = ::poll(fds.data(), fds.size(), pollTimeout(nowMs(), nextTick));
    g_watchdog->advance();
    if (n < 0 && errno != EINTR) {
      g_out->error("poll failed: " + errnoText(errno));
      return kExitFailed;
    }
    for (size_t i = 0; n > 0 && i < owners.size(); ++i) {
      if (fds[first + i].revents != 0) readDevice(*owners[i]);
    }
    // Unplugged devices go before hotplug runs: a new device may take the same node name.
    for (auto it = g_devices.begin(); it != g_devices.end();) {
      if (!(*it)->dead) {
        ++it;
        continue;
      }
      dropDevice(**it);
      it = g_devices.erase(it);
    }
    if (inotifyFd >= 0 && fds[1].revents != 0) readHotplug(inotifyFd);
    if (fds[0].revents != 0 && ((fds[0].revents & POLLNVAL) != 0 || !readStdin(pending))) return 0;
    const uint64_t now = nowMs();
    if (now >= nextTick) {
      g_decision->syncKeyStates(isKeyDown);
      apply(g_decision->tick(now));
      nextTick = now + kTickMs;
    }
    grabSettled(now);
  }
}

void closeAll(int inotifyFd) {
  for (auto& d : g_devices) {
    release(*d);
    libevdev_free(d->dev);
    ::close(d->fd);
  }
  g_devices.clear();
  if (g_maskKeyboard != nullptr) libevdev_uinput_destroy(g_maskKeyboard);
  g_maskKeyboard = nullptr;
  if (inotifyFd >= 0) ::close(inotifyFd);
}

}  // namespace

int runHooks(Output& out) {
  const std::string socket = hyprSocketPath();
  int x = 0;
  int y = 0;
  if (socket.empty() || !queryCursor(socket, x, y)) {
    out.error("noHyprland", "Can't reach Hyprland: lolPing on Linux needs a Hyprland session");
    return kExitNoAccess;
  }
  Decision decision;
  decision.setMaskMenuKeys(false);  // the key-up is already gone when we read it; see tapMask()
  g_decision = &decision;
  g_out = &out;
  g_maskKeyboard = createMaskKeyboard();
  if (g_maskKeyboard == nullptr) {
    out.error("noAccess", "Can't write /dev/uinput: lolPing needs a udev rule that gives access to it");
    return kExitNoAccess;
  }
  CursorThread cursor(socket, out);
  Watchdog watchdog;
  g_cursor = &cursor;
  g_watchdog = &watchdog;

  const int inotifyFd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotifyFd < 0 || ::inotify_add_watch(inotifyFd, kInputDir, IN_CREATE | IN_ATTRIB) < 0)
    out.error("can't watch /dev/input: mice plugged in later are not seen");
  bool denied = false;
  scanDevices(denied);
  const bool anyMouse = std::any_of(g_devices.begin(), g_devices.end(), [](const auto& d) { return d->mouse; });
  if (!anyMouse && denied) {
    closeAll(inotifyFd);
    out.error("noAccess", "Can't read the mouse: lolPing needs the input group");
    return kExitNoAccess;
  }
  updateGrabs();
  out.ready();
  const int code = runLoop(inotifyFd);
  closeAll(inotifyFd);
  return code;
}

}  // namespace lp
