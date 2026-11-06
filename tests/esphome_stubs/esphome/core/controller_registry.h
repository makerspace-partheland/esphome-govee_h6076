#pragma once

namespace esphome {
struct ControllerRegistry {
  static inline unsigned invalidations{0};
  static inline bool last_has_state{true};
  template<typename Entity> static void notify_switch_update(Entity *entity) {
    ++invalidations;
    last_has_state = entity->has_state();
  }
};
}  // namespace esphome
