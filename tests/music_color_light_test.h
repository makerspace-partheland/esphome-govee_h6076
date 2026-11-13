#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "govee_h6076/govee_h6076.h"

inline void run_music_color_light_tests() {
  using namespace esphome;
  using namespace esphome::govee_h6076;
  const auto check = [](bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "FAIL: %s\n", message);
      std::exit(1);
    }
  };
  GoveeLight parent;
  light::LightState lamp(&parent);
  GoveeMusicColorLight first, second;
  light::LightState first_state(&first), second_state(&second);
  first.set_parent(&parent); first.set_color_index(0);
  second.set_parent(&parent); second.set_color_index(1);
  parent.set_music_color_light(0, &first); parent.set_music_color_light(1, &second);
  first_state.set_default_transition_length(0); second_state.set_default_transition_length(0);
  lamp.call(); lamp.call();
  first_state.call(); first_state.call(); second_state.call(); second_state.call();
  parent.set_music_color(0, 0x408020);
  float r, g, b;
  first_state.current_values_as_rgb(&r, &g, &b);
  check(std::abs(r - 64 / 255.0f) < 0.001f && std::abs(g - 128 / 255.0f) < 0.001f &&
        std::abs(b - 32 / 255.0f) < 0.001f, "API feedback preserves absolute RGB intensity");
  parent.set_music_color(1, 0x0000FF);
  first_state.turn_on().set_rgb(1, 0, 0).set_brightness(0.5f).perform();
  first_state.current_values_as_rgb(&r, &g, &b);
  check(std::abs(r - 0.5f) < 0.001f && g == 0 && b == 0, "native color edits retain brightness");
  check(second_state.remote_values.get_blue() == 1 && second_state.remote_values.get_red() == 0,
        "editing one palette slot preserves its neighbor");
  first_state.turn_off().perform();
  check(!first_state.remote_values.is_on(), "off supplies a black palette slot");
  first_state.turn_on().perform();
  first_state.current_values_as_rgb(&r, &g, &b);
  check(std::abs(r - 0.5f) < 0.001f, "turn-on restores the previous slot color and brightness");
  parent.set_music_color(0, 0);
  first_state.current_values_as_rgb(&r, &g, &b);
  check(r == 0 && g == 0 && b == 0, "an API black color must not become white");
  check(!lamp.remote_values.is_on(), "palette editing must not turn on the physical lamp");
  std::puts("Native music color controls: RGB, brightness, API feedback, independent slots and power isolation passed");
  std::exit(0);
}
