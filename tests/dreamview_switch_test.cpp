#define USE_GOVEE_DREAMVIEW
#define USE_CONTROLLER_REGISTRY
#include "../components/govee_h6076/dreamview_switch.h"

#include <cassert>
#include <iostream>

// Command handling lives in govee_h6076.cpp; this test concerns publication only.
void esphome::govee_h6076::GoveeDreamViewSwitch::write_state(bool) {}

int main() {
  using esphome::ControllerRegistry;
  using esphome::govee_h6076::GoveeDreamViewSwitch;
  for (bool inverted : {false, true}) {
    GoveeDreamViewSwitch entity;
    entity.set_inverted(inverted);
    entity.publish_native_state(false, false);
    assert(!entity.has_state() && entity.callbacks == 0);
    entity.publish_native_state(true, false);
    assert(entity.has_state() && entity.state == inverted && entity.callbacks == 1);
    entity.publish_native_state(true, true);
    assert(entity.state == !inverted && entity.callbacks == 2);
    entity.publish_native_state(true, true);
    assert(entity.callbacks == 2);
    const auto notifications = ControllerRegistry::invalidations;
    entity.publish_native_state(false, false);
    assert(!entity.has_state() && entity.callbacks == 2);
    assert(ControllerRegistry::invalidations == notifications + 1 && !ControllerRegistry::last_has_state);
    entity.publish_native_state(false, false);
    assert(ControllerRegistry::invalidations == notifications + 1);
    entity.publish_native_state(true, true);
    assert(entity.has_state() && entity.state == !inverted && entity.callbacks == 3);
    entity.publish_native_state(true, false);
    assert(entity.state == inverted && entity.callbacks == 4);
  }
  std::cout << "Native switch publication passed for normal, inverted, unknown and restored state\n";
}
