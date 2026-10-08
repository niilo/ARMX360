/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_INPUT_SYSTEM_H_
#define XENIA_HID_INPUT_SYSTEM_H_

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/portal/portal.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {
namespace hid {

class InputSystem {
 public:
  explicit InputSystem(xe::ui::Window* window);
  ~InputSystem();

  xe::ui::Window* window() const { return window_; }

  X_STATUS Setup();

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void ClearDrivers() { drivers_.clear(); }
  size_t driver_count() const { return drivers_.size(); }
#if XE_PLATFORM_xendroid
  // XenDroid: the JNI input bridge (xendroid_emu.cpp) needs the raw driver
  // pointer to forward Android motion/key events. Guarded so edge desktop stays
  // byte-identical (replaces an earlier accidental blanket `public:` that flipped
  // the whole trailing member section public on all platforms).
  InputDriver* driver(size_t index) const { return drivers_[index].get(); }
#endif

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, uint32_t flags,
                    X_INPUT_STATE* out_state);
  // GetState variant for UI that bypasses the input blocker
  X_RESULT GetStateForUI(uint32_t user_index, uint32_t flags,
                         X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke);

  // Block/unblock input to the game (for UI dialogs)
  void AddUIInputBlocker();
  void RemoveUIInputBlocker();

  bool GetVibrationCvar();
  void ToggleVibration();

  const std::bitset<XUserMaxUserCount> GetConnectedSlots() const {
    return connected_slots;
  }

  uint32_t GetLastUsedSlot() const { return last_used_slot; }

  Portal* GetPortal() { return portal_.get(); }

  std::unique_lock<xe_unlikely_mutex> lock();

  // Which device currently feeds a guest controller slot.
  struct SlotBinding {
    InputDriver* driver = nullptr;  // null = empty or detached
    uint8_t driver_slot = 0;
    // Retained while detached so the same device reattaches to this slot.
    std::string stable_id;
    // Cached at bind time; UI uses it for the bound state only.
    std::string display_name;
    // 0 = report whatever the driver detected (e.g. SDL's XInput subtype
    // translation); non-zero = force this XINPUT_DEVSUBTYPE_* value when the
    // guest reads capabilities.
    uint8_t subtype_override = 0;
  };

  // One enumerated device + its current binding state.
  struct EnumeratedDevice {
    InputDriver* driver;
    InputDeviceInfo info;
    int bound_slot;  // -1 if not bound
  };

  std::vector<EnumeratedDevice> EnumerateDevices();
  void BindSlot(uint32_t guest_slot, InputDriver* driver, uint8_t driver_slot,
                std::string stable_id, std::string display_name);
  void UnbindSlot(uint32_t guest_slot);
  // Pass subtype = 0 to clear the override and use the device's detected type.
  void SetSlotSubtypeOverride(uint32_t guest_slot, uint8_t subtype);
  const SlotBinding& GetSlotBinding(uint32_t guest_slot) const {
    return slot_bindings_[guest_slot];
  }

  // Serializes slot bindings for --slot_bindings_passthrough=; empty when
  // nothing to forward. Format documented at the definition.
  std::string SerializeSlotBindingsForPassthrough() const;

  // Invoked whenever the binding table changes — explicit bind/unbind, or
  // ReconcileBindings demoting/auto-binding due to hotplug. Fires on the
  // calling thread with lock_ held, so subscribers must defer any work that
  // re-enters InputSystem (CallInUIThreadDeferred — never sync, since lock_
  // is a non-recursive spinlock).
  using BindingsChangedCallback = std::function<void()>;
  void SetBindingsChangedCallback(BindingsChangedCallback cb) {
    bindings_changed_cb_ = std::move(cb);
  }

  // Drivers call this after a hotplug add/remove; schedules ReconcileBindings
  // to run on the UI thread (which takes lock_) so the SDL event thread can
  // post the notification without holding any of our locks.
  void NotifyDevicesChanged();

 private:
  typedef std::pair<uint16_t, uint16_t> joystick_value;

  const std::string controller_slot_state_change_message[2] = {
      "Controller disconnected from slot {}.",
      "New controller connected to slot {}."};

  void UpdateUsedSlot(InputDriver* driver, uint8_t slot, bool connected);
  void AdjustDeadzoneLevels(const uint8_t slot, X_INPUT_GAMEPAD* gamepad);
  X_INPUT_VIBRATION ModifyVibrationLevel(X_INPUT_VIBRATION* vibration);

  // Detach gone devices, reattach by stable_id, auto-bind new devices to the
  // first empty guest slot. Cheap; safe to call from the polling path.
  void ReconcileBindings();

  // Seeds slot_bindings_ as detached entries from the passthrough cvar;
  // ReconcileBindings reattaches them by stable_id as devices appear.
  void LoadSlotBindingsFromPassthrough();

  // Flip accounting for log_input_poll_breakdown: called from UpdateUsedSlot
  // once a connected-slot transition has actually been applied, and from
  // RecordPoll to decide when the once-per-second report is due.
  void RecordUsedSlotFlip();
  void LogPollStatsIfDue();

  // Attribution for log_input_poll_breakdown. Records the (slot, flags) pair a
  // poll arrived with, whether it was answered, and whether it came from a
  // guest thread. Guest and host polls are separated because the host polls
  // (emulator_window.cc's per-frame loop) would otherwise dominate the buckets
  // with flags=0x1 successes and hide the guest flags value being misread.
  // Called from GetStateForUI; does nothing unless the cvar is on.
  void RecordPoll(uint32_t slot, uint32_t flags, bool error, bool from_guest);

  xe::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<Portal> portal_;

  std::bitset<XUserMaxUserCount> connected_slots = {};
  std::array<SlotBinding, XUserMaxUserCount> slot_bindings_{};
  // True while a NotifyDevicesChanged() reconcile is pending on the UI
  // thread; coalesces bursts of hotplug events into a single reconcile.
  std::atomic<bool> reconcile_pending_{false};
  // Devices the user has explicitly unbound this session; suppresses
  // re-auto-binding by ReconcileBindings until BindSlot or app restart.
  std::set<std::string> dismissed_ids_;

  BindingsChangedCallback bindings_changed_cb_;
  std::array<std::pair<joystick_value, joystick_value>, XUserMaxUserCount>
      controllers_max_joystick_value = {};
  uint32_t last_used_slot = 0;

  xe_unlikely_mutex lock_;

  // Reference count for UI elements blocking game input
  std::atomic<int> ui_input_blockers_{0};

  // Buttons that should be masked from game input until released (per slot).
  // This prevents button presses used to close UI dialogs from being
  // seen by the game immediately after the dialog closes.
  std::array<uint16_t, XUserMaxUserCount> consumed_buttons_{};

  struct PollBucket {
    uint32_t flags = 0;
    uint8_t slot = 0xFF;
    uint32_t polls = 0;
    uint32_t errors = 0;
  };
  // Bounded so a guest that varies flags per call cannot grow this without
  // limit; dropped is reported rather than silently folded into a catch-all.
  static constexpr size_t kMaxPollBuckets = 8;
  struct PollStats {
    std::array<PollBucket, kMaxPollBuckets> buckets{};
    uint32_t polls = 0;
    uint32_t errors = 0;
    uint32_t host_polls = 0;
    uint32_t flips = 0;
    uint32_t notifications = 0;
    uint32_t dropped = 0;
    uint64_t last_report_ns = 0;
  };
  PollStats poll_stats_{};
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_INPUT_SYSTEM_H_
