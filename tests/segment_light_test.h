#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "govee_h6076/govee_h6076.h"

class SegmentTestUART final : public esphome::uart::UARTComponent {
 public:
  void write_array(const uint8_t *data, size_t size) override { this->sent.emplace_back(data, data + size); }
  bool peek_byte(uint8_t *data) override {
    if (!this->available())
      return false;
    *data = this->received_[this->position_];
    return true;
  }
  bool read_array(uint8_t *data, size_t size) override {
    if (size > this->available())
      return false;
    std::copy_n(this->received_.data() + this->position_, size, data);
    this->position_ += size;
    return true;
  }
  size_t available() override { return this->received_.size() - this->position_; }
  esphome::uart::UARTFlushResult flush() override { return esphome::uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS; }
  void check_logger_conflict() override {}
  void report(uint8_t command, std::initializer_list<uint8_t> payload) {
    const std::vector<uint8_t> bytes(payload);
    const auto packet = esphome::govee_h6076::protocol::encode(command, bytes.data(), bytes.size());
    this->received_.insert(this->received_.end(), packet.bytes.begin(), packet.bytes.begin() + packet.size);
  }
  void settle(esphome::govee_h6076::GoveeLight &output) {
    for (size_t budget = 0; budget < 40; ++budget) {
      output.loop();
      if (this->answered_ == this->sent.size())
        return;
      const auto &packet = this->sent[this->answered_++];
      if (packet[6] == 0x86) {
        this->power_ = packet[7] != 0;
        this->status();
      } else if (packet[6] == 0x87) {
        this->brightness_ = packet[7];
        this->report(0xC2, {0, this->brightness_});
      } else if (packet[6] == 0x88) {
        this->power_ = true;
        this->red_ = packet[7];
        this->green_ = packet[8];
        this->blue_ = packet[9];
        this->report(0xC3, {0, this->red_, this->green_, this->blue_, packet[10], packet[11]});
      } else if (packet[6] == 0x83) {
        this->report(0x63, {});
      } else if (packet[6] == 0x82) {
        this->status();
      }
    }
    std::fputs("FAIL: UART fixture did not settle\n", stderr);
    std::exit(1);
  }
  void status() { this->report(0xC1, {0, 0, uint8_t(this->power_), this->brightness_, 0, 0, 0, 0,
                                     this->red_, this->green_, this->blue_, 0, 0}); }
  bool is_on() const { return this->power_; }
  std::vector<std::vector<uint8_t>> sent;

 protected:
  std::vector<uint8_t> received_;
  size_t position_{0}, answered_{0};
  bool power_{false};
  uint8_t brightness_{20}, red_{51}, green_{0}, blue_{0};
};

// Run against ESPHome's real LightState/LightCall implementation, without a UART device.
inline void run_segment_light_tests() {
  using namespace esphome;
  using namespace esphome::govee_h6076;
  const auto check = [](bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "FAIL: %s\n", message);
      std::exit(1);
    }
  };
  GoveeLight output;
  light::LightState lamp(&output);
  lamp.set_default_transition_length(0);
#ifdef USE_LIGHT_RESUME_EFFECT
  lamp.set_resume_effect(true);
#endif
  GoveeSegmentsEffect pattern("Segments");
  pattern.set_parent(&output);
  lamp.add_effects({&pattern});
  std::array<GoveeSegmentLight, 7> outputs;
  std::array<light::LightState, 7> lights{{&outputs[0], &outputs[1], &outputs[2], &outputs[3],
                                        &outputs[4], &outputs[5], &outputs[6]}};
  for (size_t i = 0; i < lights.size(); ++i) {
    outputs[i].set_parent(&pattern);
    outputs[i].set_segment_index(i);
    pattern.set_segment_light(i, &outputs[i]);
    pattern.set_segment(i, 0xFF0000, 100);
    lights[i].set_default_transition_length(0);
  }
  lights[6].set_state_callback([](light::LightStateRTCState &state, bool) { state.state = true; });
  lamp.call();
  lamp.call();
  for (auto &light : lights) {
    light.call();
    light.call();
  }
  check(!lamp.remote_values.is_on() && !pattern.is_active(), "startup must not turn on the lamp");
  lamp.publish_state();
  for (const auto &light : lights)
    check(!light.remote_values.is_on(), "inactive segments must appear off");

  lights[0].turn_on().set_rgb(0, 1, 0).set_brightness(0.5f).perform();
  check(lamp.remote_values.is_on() && pattern.is_active(), "a segment command must activate the pattern");
  for (size_t i = 1; i < lights.size(); ++i)
    check(lights[i].remote_values.is_on(), "startup must preserve the configured segment brightness values");
  check(lights[0].remote_values.is_on() && lights[0].remote_values.get_green() == 1,
        "the edited segment must retain its color");
  check(std::abs(lights[0].remote_values.get_brightness() - 0.5f) < 0.01f,
        "segment brightness must remain independent");
  check(lights[1].remote_values.get_red() == 1 && lights[1].remote_values.get_green() == 0,
        "editing one segment must preserve its neighbor");

  lights[0].turn_off().perform();
  check(!lights[0].remote_values.is_on() && lights[1].remote_values.is_on() && lamp.remote_values.is_on(),
        "turning off one segment must leave the others on");
  lights[0].turn_on().perform();
  check(lights[0].remote_values.is_on() && lights[0].remote_values.get_green() == 1 &&
            std::abs(lights[0].remote_values.get_brightness() - 0.5f) < 0.01f,
        "turn-on must retain the last color and brightness");

  pattern.set_segment(6, 0x0000FF, 25);
  check(lights[6].remote_values.is_on() && lights[6].remote_values.get_blue() == 1 &&
            lights[6].remote_values.get_brightness() == 0.25f,
        "the existing segment API must update the new light entities");

  pattern.set_segment(3, 0x402010, 80);
  float red, green, blue;
  lights[3].current_values_as_rgb(&red, &green, &blue);
  check(std::abs(red - 64 / 255.0f * 0.8f) < 0.001f &&
            std::abs(green - 32 / 255.0f * 0.8f) < 0.001f &&
            std::abs(blue - 16 / 255.0f * 0.8f) < 0.001f,
        "publishing a dim RGB color must preserve absolute channel intensity");
  pattern.set_segment(4, 0x000000, 100);
  lights[4].current_values_as_rgb(&red, &green, &blue);
  check(red == 0 && green == 0 && blue == 0, "a black segment must not become white during publication");

  lights[2].turn_on().set_brightness(0.4f).set_transition_length(1000).perform();
  lights[2].loop();
  check(lights[2].is_transformer_active() && lights[2].remote_values.get_brightness() == 0.4f,
        "segment feedback must not interrupt its own brightness transition");
  lights[0].turn_on().set_brightness(0.75f).perform();
  check(lights[2].is_transformer_active(), "editing a neighbor must not interrupt an unrelated transition");

  lamp.turn_on().set_effect(0u).set_rgb(1, 1, 1).perform();
  for (const auto &light : lights)
    check(!light.remote_values.is_on(), "leaving the pattern must turn off segment indicators");
  check(lamp.get_current_effect_index() == 0, "feedback must not reactivate the segment pattern");
  lights[6].turn_on().perform();
  check(pattern.is_active() && lights[6].remote_values.get_blue() == 1,
        "returning to segments must preserve the pattern");
  lamp.turn_off().perform();
  for (const auto &light : lights)
    check(!light.remote_values.is_on(), "main power-off must turn off segment indicators");
#ifdef USE_LIGHT_RESUME_EFFECT
  lamp.turn_on().perform();
  check(pattern.is_active() && lights[6].remote_values.get_blue() == 1,
        "main power-on must resume the segment pattern without losing its colors");
  lamp.turn_off().perform();
#endif
  lights[0].turn_on().perform();
  for (auto &light : lights)
    light.turn_off().perform();
  check(!lamp.remote_values.is_on(), "turning off the last segment must turn off the main lamp");
  lights[6].turn_on().perform();
  check(lamp.remote_values.is_on() && lights[6].remote_values.is_on() && !lights[0].remote_values.is_on(),
        "turning on one segment must preserve disabled neighbors");

  // Exercise the actual output/controller boundary too: the FR's color command can power it on.
  SegmentTestUART bus;
  GoveeLight connected_output;
  connected_output.set_uart_parent(&bus);
  light::LightState connected_lamp(&connected_output);
  connected_lamp.set_default_transition_length(0);
  GoveeSegmentsEffect connected_pattern("Connected segments");
  connected_pattern.set_parent(&connected_output);
  connected_lamp.add_effects({&connected_pattern});
  connected_lamp.call();
  connected_lamp.call();
  // Synthetic status: on, 20% master brightness, blue RGB; no device identity bytes.
  bus.report(0xC1, {0, 0, 1, 20, 0, 0, 0, 0, 0, 0, 51, 0, 0});
  connected_output.loop();
  bus.report(0xC3, {0, 0, 0, 51, 0, 0});
  connected_output.loop();
  bus.report(0xC1, {0, 0, 1, 20, 0, 0, 0, 0, 0, 0, 51, 0, 0});
  connected_output.loop();
  check(connected_lamp.remote_values.is_on(), "the UART fixture must publish a confirmed on state before shutdown");
  connected_lamp.turn_on().set_effect(1u).perform();
  connected_output.loop();
  // A segment response can report a different native RGB color while the effect is in flight.
  bus.report(0xC1, {0, 0, 1, 20, 0, 0, 0, 0, 255, 127, 0, 0, 0});
  connected_output.loop();
  bus.sent.clear();
  connected_lamp.turn_off().perform();
  connected_output.loop();
  check(!bus.sent.empty() && bus.sent.back()[6] == 0x86 && bus.sent.back()[7] == 0,
        "segment shutdown must send power-off first");
  bus.report(0xC1, {0, 0, 0, 20, 0, 0, 0, 0, 255, 127, 0, 0, 0});
  connected_output.loop();
  check(bus.sent.size() == 1, "a confirmed shutdown must not be followed by a color or brightness command");
  check(!connected_lamp.remote_values.is_on(), "confirmed segment shutdown must stay off");
  std::puts("Segment light startup, color, brightness, API feedback, mode changes and power checks passed");
  std::exit(0);
}
