#pragma once

#ifdef USE_GOVEE_DREAMVIEW_MUSIC

#include "esphome/components/select/select.h"
#ifdef USE_CONTROLLER_REGISTRY
#include "esphome/core/controller_registry.h"
#endif

namespace esphome::govee_h6076 {

class GoveeLight;

class GoveeDreamViewSelect final : public select::Select {
 public:
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }
  void publish_native_state(bool known, size_t index) {
    if (known) {
      if (!this->has_state() || this->active_index() != index)
        this->publish_state(index);
    } else if (this->has_state()) {
      // Select has no invalidate_state(); the native API uses missing_state.
      this->set_has_state(false);
#ifdef USE_CONTROLLER_REGISTRY
      ControllerRegistry::notify_select_update(this);
#endif
    }
  }

 protected:
  void control(size_t index) override;
  GoveeLight *parent_{nullptr};
};

}  // namespace esphome::govee_h6076

#endif  // USE_GOVEE_DREAMVIEW_MUSIC
