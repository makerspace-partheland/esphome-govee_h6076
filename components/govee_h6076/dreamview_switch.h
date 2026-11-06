#pragma once

#ifdef USE_GOVEE_DREAMVIEW

#include "esphome/components/switch/switch.h"
#ifdef USE_CONTROLLER_REGISTRY
#include "esphome/core/controller_registry.h"
#endif

namespace esphome::govee_h6076 {

class GoveeLight;

class GoveeDreamViewSwitch : public switch_::Switch {
 public:
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }
  void publish_native_state(bool known, bool enabled) {
    if (known) {
      // Switch applies inversion and deduplicates the raw hardware value itself.
      this->publish_state(enabled);
      return;
    }
    if (!this->has_state())
      return;
    // ESPHome 2026.10 Switch has no invalidate_state(), but its native API
    // transports has_state as missing_state. Do not fire boolean on/off callbacks
    // for an unknown value; the controller registry receives the invalidation.
    this->set_has_state(false);
    this->publish_dedup_ = {};
#ifdef USE_CONTROLLER_REGISTRY
    ControllerRegistry::notify_switch_update(this);
#endif
  }

 protected:
  void write_state(bool state) override;
  GoveeLight *parent_{nullptr};
};

}  // namespace esphome::govee_h6076

#endif  // USE_GOVEE_DREAMVIEW
