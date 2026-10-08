/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/logging.h"

#include <cstdlib>
#include <cstring>
#include <chrono>

#include "xenia/hid/input_system.h"

#include "xenia/base/profiling.h"
#include "xenia/hid/hid_flags.h"
#include "xenia/hid/input_driver.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"

#ifdef XE_PLATFORM_WIN32
#include "xenia/hid/portal/hardware_portal.h"
#endif  // XE_PLATFORM_WIN32

namespace xe {
namespace hid {

namespace {
// True when the caller is executing on a guest XThread rather than the host UI
// thread. XThread::GetCurrentThread() is null off a guest thread. File-local so
// xthread.h stays out of input_system.h, which is included very widely.
bool IsGuestThread() { return kernel::XThread::GetCurrentThread() != nullptr; }
}  // namespace

DEFINE_bool(vibration, true, "Toggle controller vibration.", "HID");

DEFINE_bool(
    log_input_poll_breakdown, false,
    "Report which XamInputGetState flag values the guest polls with, how many "
    "of those polls are answered, and how much connected-slot churn they "
    "cause, as one 'HidPoll' line per second. Attribution only - no binding is "
    "changed, no notification is suppressed, and nothing about the state the "
    "game sees differs from a run with this off.\n"
    "WHY THIS EXISTS. UpdateUsedSlot is called from GetStateForUI, i.e. from "
    "the guest's per-frame query path, so a slot's connected state is decided "
    "by whether an individual poll matched rather than by whether a device is "
    "present. GetStateForUI only answers success when "
    "(flags & driver->GetInputType()) != 0, and InputType::Controller is 1 "
    "(input_driver.h) while X_INPUT_FLAG_GAMEPAD is 1 (input.h) - they agree "
    "only by numeric coincidence. Any flag value with bit 0 clear, such as "
    "X_INPUT_FLAG_ANY_USER (1 << 30), therefore fails that test and reads as "
    "'disconnected'. Two guest threads polling with different flags then flip "
    "connected_slots back and forth, and every flip logs and broadcasts "
    "kXNotificationSystemInputDevicesChanged under the kernel global lock.\n"
    "A captured session on a Pocket S shows exactly that: 32797 'controller "
    "connected/disconnected' lines, about two per guest frame across 22163 "
    "guest presents, with Main XThread doing the connects and two other guest "
    "threads doing the disconnects. That is the hypothesis this reports on.\n"
    "READING IT. 'flips' near 2x 'polls' means the slots are thrashing. "
    "Compare err against polls per flags value: a bucket with err == polls is "
    "a flag value being answered as 'not connected' every time, which names "
    "the bug. A bucket that is entirely successful means that caller is not "
    "part of it. host_polls are counted but not bucketed - they come from the "
    "host UI loop, are always flags=0x1, and always succeed, so bucketing them "
    "would bury the guest flags value that is being misread. 'dropped' is "
    "non-zero when more than 8 distinct (slot, flags) pairs appeared, so the "
    "per-bucket lines are then incomplete.\n"
    "CAVEATS. Logging only - nothing about the state the game sees differs from "
    "a run with this off, and the counters are not even maintained when it is "
    "off. But do not read an fps number off a run with any log_gpu_* cvar "
    "enabled (AGENTS.md trap 10). NOT VERIFIED: this cvar has never been run. "
    "The captured session it is modelled on predates the build that contains it, "
    "so it has never printed a line and every figure above is inferred from "
    "that log rather than measured here.",
    "HID");

DEFINE_double(left_stick_deadzone_percentage, 0.0,
              "Defines deadzone level for left stick. Allowed range [0.0-1.0].",
              "HID");
DEFINE_double(
    right_stick_deadzone_percentage, 0.0,
    "Defines deadzone level for right stick. Allowed range [0.0-1.0].", "HID");

DEFINE_transient_string(
    slot_bindings_passthrough, "",
    "Internal: slot bindings handed from a parent xenia process to its child "
    "across out-of-process relaunch. Not persisted.",
    "HID");

InputSystem::InputSystem(xe::ui::Window* window) : window_(window) {
#ifdef XE_PLATFORM_WIN32
  portal_ = std::make_unique<HardwarePortal>();
#endif  // XE_PLATFORM_WIN32
}

InputSystem::~InputSystem() = default;

X_STATUS InputSystem::Setup() {
  LoadSlotBindingsFromPassthrough();
  // SDL drains its initial DEVICEADDED events inside the driver's own Setup,
  // before AddDriver wires devices_changed_callback_ — that auto-reconcile is
  // lost, so already-connected controllers need an explicit pass to attach.
  ReconcileBindings();
  return X_STATUS_SUCCESS;
}

void InputSystem::AddDriver(std::unique_ptr<InputDriver> driver) {
  driver->set_devices_changed_callback([this]() { NotifyDevicesChanged(); });
  drivers_.push_back(std::move(driver));
}

void InputSystem::NotifyDevicesChanged() {
  if (reconcile_pending_.exchange(true)) {
    return;
  }
  if (!window_) {
    reconcile_pending_.store(false);
    return;
  }
  window_->app_context().CallInUIThreadDeferred([this]() {
    reconcile_pending_.store(false);
    {
      auto lock = this->lock();
      ReconcileBindings();
    }
    // The device list has changed; menu/toolbar must refresh even if
    // ReconcileBindings didn't alter any bindings (e.g. a dismissed device
    // replugged is visible again but stays unbound).
    if (bindings_changed_cb_) {
      bindings_changed_cb_();
    }
  });
}

void InputSystem::RecordPoll(uint32_t slot, uint32_t flags, bool error,
                            bool from_guest) {
  auto& s = poll_stats_;
  if (!from_guest) {
    // Host polls (emulator_window.cc's per-frame loop) are counted but not
    // bucketed: they are always flags=0x1 and always succeed, so bucketing
    // them would bury the guest flag value that is being misread.
    s.host_polls++;
    return;
  }
  s.polls++;
  if (error) {
    s.errors++;
  }
  PollBucket* bucket = nullptr;
  for (auto& b : s.buckets) {
    if (b.slot == slot && b.flags == flags) {
      bucket = &b;
      break;
    }
  }
  if (!bucket) {
    for (auto& b : s.buckets) {
      if (b.slot == 0xFF) {
        b.slot = static_cast<uint8_t>(slot);
        b.flags = flags;
        bucket = &b;
        break;
      }
    }
  }
  if (bucket) {
    bucket->polls++;
    if (error) {
      bucket->errors++;
    }
  } else {
    s.dropped++;
  }
  LogPollStatsIfDue();
}

void InputSystem::RecordUsedSlotFlip() {
  auto* ks = kernel::kernel_state();
  if (cvars::log_input_poll_breakdown) {
    auto& s = poll_stats_;
    s.flips++;
    if (ks) {
      s.notifications++;
    }
  }
  // Broadcast unconditionally: this is the behaviour that existed before the
  // counter was introduced, and the cvar must not change what the guest sees.
  if (ks) {
    ks->BroadcastNotification(kXNotificationSystemInputDevicesChanged, 0);
  }
}

void InputSystem::LogPollStatsIfDue() {
  auto& s = poll_stats_;
  const uint64_t now = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  if (!s.last_report_ns) {
    s.last_report_ns = now;
    return;
  }
  if (now - s.last_report_ns < 1000000000ull) {
    return;
  }
  s.last_report_ns = now;

  XELOGI("HidPoll: {} guest polls | err={} | host_polls={} | flips={} | notif={} | "
         "dropped={}",
         s.polls, s.errors, s.host_polls, s.flips, s.notifications, s.dropped);
  for (const auto& b : s.buckets) {
    if (b.slot == 0xFF) {
      continue;
    }
    XELOGI("HidPoll[{}]: flags=0x{:X} polls={} err={}", b.slot, b.flags, b.polls,
           b.errors);
  }

  s.buckets = {};
  s.polls = 0;
  s.errors = 0;
  s.host_polls = 0;
  s.flips = 0;
  s.notifications = 0;
  s.dropped = 0;
}

void InputSystem::UpdateUsedSlot(InputDriver* driver, uint8_t slot,
                                 bool connected) {
  if (slot == XUserIndexAny) {
    XELOGW("{} received requrest for slot any! Unsupported", __func__);
    return;
  }

  // Do not report passthrough as a controller.
  if (driver && driver->GetInputType() == InputType::Keyboard) {
    return;
  }

  if (connected_slots.test(slot) == connected) {
    // No state change, so nothing to do.
    return;
  }

  XELOGI(controller_slot_state_change_message[connected].c_str(), slot);
  connected_slots.flip(slot);
  RecordUsedSlotFlip();

  if (driver) {
    X_INPUT_CAPABILITIES capabilities = {};
    const X_RESULT result = driver->GetCapabilities(slot, 0, &capabilities);
    if (result != X_STATUS_SUCCESS) {
      return;
    }

    controllers_max_joystick_value[slot] = {
        {capabilities.gamepad.thumb_lx, capabilities.gamepad.thumb_ly},
        {capabilities.gamepad.thumb_rx, capabilities.gamepad.thumb_ry}};
  }
}

X_RESULT InputSystem::GetCapabilities(uint32_t user_index, uint32_t flags,
                                      X_INPUT_CAPABILITIES* out_caps) {
  SCOPE_profile_cpu_f("hid");

  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto& binding = slot_bindings_[user_index];
  if (!binding.driver || (flags & binding.driver->GetInputType()) == 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT result =
      binding.driver->GetCapabilities(binding.driver_slot, flags, out_caps);
  if (result == X_ERROR_SUCCESS && binding.subtype_override != 0) {
    out_caps->sub_type = binding.subtype_override;
  }
  return result;
}

X_RESULT InputSystem::GetState(uint32_t user_index, uint32_t flags,
                               X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  // If UI is blocking input, return zeroed state to the game
  if (ui_input_blockers_.load() > 0) {
    std::memset(out_state, 0, sizeof(X_INPUT_STATE));
    return X_ERROR_SUCCESS;
  }

  X_RESULT result = GetStateForUI(user_index, flags, out_state);

  // Mask buttons that were held when a UI dialog closed until they're
  // released, so the close-press doesn't carry through into the game.
  if (result == X_ERROR_SUCCESS && user_index < XUserMaxUserCount &&
      consumed_buttons_[user_index] != 0) {
    uint16_t buttons = out_state->gamepad.buttons;
    consumed_buttons_[user_index] &= buttons;
    out_state->gamepad.buttons = buttons & ~consumed_buttons_[user_index];
  }

  return result;
}

X_RESULT InputSystem::GetStateForUI(uint32_t user_index, uint32_t flags,
                                    X_INPUT_STATE* out_state) {
  SCOPE_profile_cpu_f("hid");

  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto& binding = slot_bindings_[user_index];
  if (binding.driver && (flags & binding.driver->GetInputType()) != 0) {
    X_RESULT r = binding.driver->GetState(binding.driver_slot, out_state);
    if (r == X_ERROR_SUCCESS) {
      if (cvars::log_input_poll_breakdown) {
        RecordPoll(user_index, flags, false, IsGuestThread());
      }
      UpdateUsedSlot(binding.driver, user_index, true);
      AdjustDeadzoneLevels(user_index, &out_state->gamepad);
      if (out_state->gamepad.buttons != 0) {
        last_used_slot = user_index;
      }
      return r;
    }
  }
  if (cvars::log_input_poll_breakdown) {
    RecordPoll(user_index, flags, true, IsGuestThread());
  }
  UpdateUsedSlot(nullptr, user_index, false);
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

void InputSystem::AddUIInputBlocker() { ui_input_blockers_.fetch_add(1); }

void InputSystem::RemoveUIInputBlocker() {
  // Before removing the blocker, capture any currently pressed buttons.
  // These will be masked from game input until they are released, preventing
  // the button press that closed the UI from carrying over into the game.
  X_INPUT_STATE state;
  for (uint32_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    if (GetStateForUI(user_index, 1, &state) == X_ERROR_SUCCESS) {
      consumed_buttons_[user_index] |= state.gamepad.buttons;
    }
  }

  ui_input_blockers_.fetch_sub(1);
}

X_RESULT InputSystem::SetState(uint32_t user_index,
                               X_INPUT_VIBRATION* vibration) {
  SCOPE_profile_cpu_f("hid");
  X_INPUT_VIBRATION modified_vibration = ModifyVibrationLevel(vibration);
  if (user_index >= XUserMaxUserCount) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto& binding = slot_bindings_[user_index];
  if (!binding.driver) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return binding.driver->SetState(binding.driver_slot, &modified_vibration);
}

X_RESULT InputSystem::GetKeystroke(uint32_t user_index, uint32_t flags,
                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");

  // If UI is blocking input, return empty keystroke to the game.
  if (ui_input_blockers_.load() > 0) {
    std::memset(out_keystroke, 0, sizeof(X_INPUT_KEYSTROKE));
    return X_ERROR_EMPTY;
  }

  bool any_connected = false;

  auto try_driver = [&](InputDriver* driver, uint8_t driver_slot) -> X_RESULT {
    X_RESULT r = driver->GetKeystroke(driver_slot, flags, out_keystroke);
    if (r == X_ERROR_INVALID_PARAMETER || r == X_ERROR_DEVICE_NOT_CONNECTED) {
      return r;
    }
    any_connected = true;
    if (r == X_ERROR_SUCCESS) {
      last_used_slot = user_index;
    }
    return r;
  };

  // Bound device for this slot.
  if (user_index < XUserMaxUserCount) {
    auto& binding = slot_bindings_[user_index];
    if (binding.driver && (flags & binding.driver->GetInputType()) != 0) {
      X_RESULT r = try_driver(binding.driver, binding.driver_slot);
      if (r == X_ERROR_SUCCESS) {
        return r;
      }
    }
  }

  // Passthrough fallback: Keyboard-type drivers feed all slots in parallel
  // and don't participate in the binding table.
  if (flags & InputType::Keyboard) {
    for (auto& driver : drivers_) {
      if (driver->GetInputType() != InputType::Keyboard) {
        continue;
      }
      X_RESULT r = try_driver(driver.get(), static_cast<uint8_t>(user_index));
      if (r == X_ERROR_SUCCESS) {
        return r;
      }
    }
  }

  return any_connected ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

bool InputSystem::GetVibrationCvar() { return cvars::vibration; }

void InputSystem::ToggleVibration() {
  OVERRIDE_PERSIST_bool(vibration, !cvars::vibration);
  // Send instant update to vibration state to prevent awaiting for next tick.
  X_INPUT_VIBRATION vibration = X_INPUT_VIBRATION();

  for (uint8_t user_index = 0; user_index < XUserMaxUserCount; user_index++) {
    SetState(user_index, &vibration);
  }
}

void InputSystem::AdjustDeadzoneLevels(const uint8_t slot,
                                       X_INPUT_GAMEPAD* gamepad) {
  if (slot >= XUserMaxUserCount) {
    return;
  }

  // Left stick
  if (cvars::left_stick_deadzone_percentage > 0.0 &&
      cvars::left_stick_deadzone_percentage < 1.0) {
    const double deadzone_lx_percentage =
        controllers_max_joystick_value[slot].first.first *
        cvars::left_stick_deadzone_percentage;
    const double deadzone_ly_percentage =
        controllers_max_joystick_value[slot].first.second *
        cvars::left_stick_deadzone_percentage;

    const double theta = std::atan2(static_cast<double>(gamepad->thumb_ly),
                                    static_cast<double>(gamepad->thumb_lx));

    const double deadzone_y_value = std::sin(theta) * deadzone_ly_percentage;
    const double deadzone_x_value = std::cos(theta) * deadzone_lx_percentage;

    if (gamepad->thumb_ly > -deadzone_y_value &&
        gamepad->thumb_ly < deadzone_y_value) {
      gamepad->thumb_ly = 0;
    }

    if (gamepad->thumb_lx > -deadzone_x_value &&
        gamepad->thumb_lx < deadzone_x_value) {
      gamepad->thumb_lx = 0;
    }
  }

  // Right stick
  if (cvars::right_stick_deadzone_percentage > 0.0 &&
      cvars::right_stick_deadzone_percentage < 1.0) {
    const double deadzone_rx_percentage =
        controllers_max_joystick_value[slot].second.first *
        cvars::right_stick_deadzone_percentage;
    const double deadzone_ry_percentage =
        controllers_max_joystick_value[slot].second.second *
        cvars::right_stick_deadzone_percentage;

    const double theta = std::atan2(static_cast<double>(gamepad->thumb_ry),
                                    static_cast<double>(gamepad->thumb_rx));

    const double deadzone_y_value = std::sin(theta) * deadzone_ry_percentage;
    const double deadzone_x_value = std::cos(theta) * deadzone_rx_percentage;

    if (gamepad->thumb_ry > -deadzone_y_value &&
        gamepad->thumb_ry < deadzone_y_value) {
      gamepad->thumb_ry = 0;
    }

    if (gamepad->thumb_rx > -deadzone_x_value &&
        gamepad->thumb_rx < deadzone_x_value) {
      gamepad->thumb_rx = 0;
    }
  }
}

X_INPUT_VIBRATION InputSystem::ModifyVibrationLevel(
    X_INPUT_VIBRATION* vibration) {
  X_INPUT_VIBRATION modified_vibration = *vibration;
  if (cvars::vibration) {
    return modified_vibration;
  }

  // TODO(Gliniak): Use modifier instead of boolean value.
  modified_vibration.left_motor_speed = 0;
  modified_vibration.right_motor_speed = 0;
  return modified_vibration;
}
std::unique_lock<xe_unlikely_mutex> InputSystem::lock() {
  return std::unique_lock<xe_unlikely_mutex>{lock_};
}

void InputSystem::ReconcileBindings() {
  struct LiveDevice {
    InputDriver* driver;
    InputDeviceInfo info;
  };
  std::vector<LiveDevice> live;
  for (auto& d : drivers_) {
    for (auto& info : d->EnumerateDevices()) {
      live.push_back({d.get(), std::move(info)});
    }
  }

  // Pass 1: demote bindings whose device has gone away.
  for (auto& b : slot_bindings_) {
    if (!b.driver) {
      continue;
    }
    bool found = false;
    for (auto& l : live) {
      if (l.driver == b.driver && l.info.driver_slot == b.driver_slot) {
        found = true;
        break;
      }
    }
    if (!found) {
      b.driver = nullptr;
      b.driver_slot = 0;
    }
  }

  // Pass 2: place devices that aren't currently bound.
  for (auto& l : live) {
    bool already_bound = false;
    for (auto& b : slot_bindings_) {
      if (b.driver == l.driver && b.driver_slot == l.info.driver_slot) {
        already_bound = true;
        break;
      }
    }
    if (already_bound) {
      continue;
    }

    // Reattach by stable_id (matches detached bindings).
    bool placed = false;
    if (!l.info.stable_id.empty()) {
      for (auto& b : slot_bindings_) {
        if (!b.driver && b.stable_id == l.info.stable_id) {
          b.driver = l.driver;
          b.driver_slot = l.info.driver_slot;
          b.display_name = l.info.display_name;
          placed = true;
          break;
        }
      }
    }
    if (placed) {
      continue;
    }

    // No reattach available — auto-bind only if the device opts in and the
    // user hasn't dismissed it this session.
    if (!l.info.auto_bind) {
      continue;
    }
    if (!l.info.stable_id.empty() && dismissed_ids_.count(l.info.stable_id)) {
      continue;
    }

    // Try preferred slot if it's not currently active.
    if (l.info.preferred_slot >= 0 &&
        l.info.preferred_slot < XUserMaxUserCount) {
      auto& b = slot_bindings_[l.info.preferred_slot];
      if (!b.driver) {
        b.driver = l.driver;
        b.driver_slot = l.info.driver_slot;
        b.stable_id = l.info.stable_id;
        b.display_name = l.info.display_name;
        continue;
      }
    }

    // Place in the first slot with no active driver. A detached binding
    // (driver==null but stable_id set) gets its identity overwritten — the
    // original device, if it returns, will auto-bind to the next free slot.
    for (auto& b : slot_bindings_) {
      if (b.driver) {
        continue;
      }
      const bool was_detached_other = b.stable_id != l.info.stable_id;
      b.driver = l.driver;
      b.driver_slot = l.info.driver_slot;
      b.stable_id = l.info.stable_id;
      b.display_name = l.info.display_name;
      if (was_detached_other) {
        b.subtype_override = 0;
      }
      break;
    }
  }
}

std::vector<InputSystem::EnumeratedDevice> InputSystem::EnumerateDevices() {
  ReconcileBindings();
  std::vector<EnumeratedDevice> out;
  for (auto& d : drivers_) {
    for (auto& info : d->EnumerateDevices()) {
      int bound_slot = -1;
      for (uint32_t s = 0; s < XUserMaxUserCount; ++s) {
        if (slot_bindings_[s].driver == d.get() &&
            slot_bindings_[s].driver_slot == info.driver_slot) {
          bound_slot = static_cast<int>(s);
          break;
        }
      }
      out.push_back({d.get(), std::move(info), bound_slot});
    }
  }
  return out;
}

void InputSystem::BindSlot(uint32_t guest_slot, InputDriver* driver,
                           uint8_t driver_slot, std::string stable_id,
                           std::string display_name) {
  if (guest_slot >= XUserMaxUserCount || !driver) {
    return;
  }

  // If this device is already bound elsewhere, vacate that slot so it moves.
  for (uint32_t s = 0; s < XUserMaxUserCount; ++s) {
    if (s == guest_slot) {
      continue;
    }
    auto& b = slot_bindings_[s];
    if (b.driver == driver && b.driver_slot == driver_slot) {
      b = {};
    } else if (!stable_id.empty() && b.stable_id == stable_id) {
      b = {};
    }
  }

  if (!stable_id.empty()) {
    dismissed_ids_.erase(stable_id);
  }

  // Whether the slot was empty or held a different device, the override (if
  // any) belonged to the previous device and must not carry to the new one.
  const bool different_device =
      slot_bindings_[guest_slot].stable_id != stable_id ||
      slot_bindings_[guest_slot].driver != driver ||
      slot_bindings_[guest_slot].driver_slot != driver_slot;
  auto& target = slot_bindings_[guest_slot];
  target.driver = driver;
  target.driver_slot = driver_slot;
  target.stable_id = std::move(stable_id);
  target.display_name = std::move(display_name);
  if (different_device) {
    target.subtype_override = 0;
  }

  driver->OnBoundToSlot(driver_slot, guest_slot);

  if (kernel::kernel_state()) {
    kernel::kernel_state()->BroadcastNotification(
        kXNotificationSystemInputDevicesChanged, 0);
  }
  if (bindings_changed_cb_) {
    bindings_changed_cb_();
  }
}

void InputSystem::SetSlotSubtypeOverride(uint32_t guest_slot, uint8_t subtype) {
  if (guest_slot >= XUserMaxUserCount) {
    return;
  }
  auto& binding = slot_bindings_[guest_slot];
  if (binding.subtype_override == subtype) {
    return;
  }
  binding.subtype_override = subtype;
  if (kernel::kernel_state()) {
    kernel::kernel_state()->BroadcastNotification(
        kXNotificationSystemInputDevicesChanged, 0);
  }
  if (bindings_changed_cb_) {
    bindings_changed_cb_();
  }
}

void InputSystem::UnbindSlot(uint32_t guest_slot) {
  if (guest_slot >= XUserMaxUserCount) {
    return;
  }
  auto previous = slot_bindings_[guest_slot];
  slot_bindings_[guest_slot] = {};
  if (!previous.stable_id.empty()) {
    dismissed_ids_.insert(previous.stable_id);
  }
  if (previous.driver) {
    previous.driver->OnUnboundFromSlot(previous.driver_slot);
  }
  if (kernel::kernel_state()) {
    kernel::kernel_state()->BroadcastNotification(
        kXNotificationSystemInputDevicesChanged, 0);
  }
  if (bindings_changed_cb_) {
    bindings_changed_cb_();
  }
}

// Format: four ';'-separated slot entries of "stable_id[:subtype]". Subtype is
// omitted when zero; an empty entry means an unbound slot. Stable IDs are
// "keyboard" or 32-hex-char SDL GUIDs, so ';' and ':' need no escaping.
std::string InputSystem::SerializeSlotBindingsForPassthrough() const {
  std::string out;
  bool any = false;
  for (uint32_t s = 0; s < XUserMaxUserCount; ++s) {
    if (s) {
      out += ';';
    }
    const auto& b = slot_bindings_[s];
    if (b.stable_id.empty()) {
      continue;
    }
    any = true;
    out += b.stable_id;
    if (b.subtype_override) {
      out += ':';
      out += std::to_string(b.subtype_override);
    }
  }
  return any ? out : std::string();
}

void InputSystem::LoadSlotBindingsFromPassthrough() {
  const std::string& s = cvars::slot_bindings_passthrough;
  if (s.empty()) {
    return;
  }
  uint32_t slot = 0;
  size_t pos = 0;
  while (slot < XUserMaxUserCount && pos <= s.size()) {
    size_t sep = s.find(';', pos);
    if (sep == std::string::npos) {
      sep = s.size();
    }
    std::string entry = s.substr(pos, sep - pos);
    pos = sep + 1;
    if (!entry.empty()) {
      uint8_t subtype = 0;
      size_t colon = entry.find(':');
      if (colon != std::string::npos) {
        unsigned long v = std::strtoul(entry.c_str() + colon + 1, nullptr, 10);
        subtype = static_cast<uint8_t>(v);
        entry.resize(colon);
      }
      auto& b = slot_bindings_[slot];
      b.driver = nullptr;
      b.driver_slot = 0;
      b.stable_id = std::move(entry);
      b.display_name.clear();
      b.subtype_override = subtype;
    }
    ++slot;
  }
}

}  // namespace hid
}  // namespace xe
