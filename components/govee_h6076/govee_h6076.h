#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_effect.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#include "esphome/core/preferences.h"
#include "esphome/core/log.h"
#include "protocol.h"
#ifdef USE_GOVEE_DREAMVIEW
#include "dreamview_switch.h"
#ifdef USE_GOVEE_DREAMVIEW_MUSIC
#include "dreamview_select.h"
#endif
#include "esphome/components/button/button.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome::govee_h6076 {

#ifdef USE_NUMBER
class GoveeSleepNumber;
#endif

#ifdef USE_GOVEE_DREAMVIEW
class GoveeDreamViewSwitch;
#endif

class GoveeLight final : public Component, public light::LightOutput, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void setup_state(light::LightState *state) override { this->state_ = state; }
  light::LightTraits get_traits() override;
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *) override {}
#ifdef USE_GOVEE_DREAMVIEW
  void set_dreamview_enabled(bool enabled) { this->dreamview_enabled_ = enabled; }
  void add_dreamview_member(uint64_t address) {
    for (size_t i = 0; i < 6; ++i)
      this->dreamview_members_[this->dreamview_member_count_][i] = address >> ((5 - i) * 8);
    ++this->dreamview_member_count_;
  }
  void set_dreamview_switch(GoveeDreamViewSwitch *entity) { this->dreamview_switch_ = entity; }
  void set_dreamview_connection(binary_sensor::BinarySensor *entity) { this->dreamview_connection_ = entity; }
  void set_dreamview_problem(binary_sensor::BinarySensor *entity) { this->dreamview_problem_ = entity; }
#ifdef USE_GOVEE_DREAMVIEW_MUSIC
  void set_dreamview_music_effect(GoveeDreamViewSelect *entity) {
    this->dreamview_music_effect_ = entity;
    this->controller_.dreamview().enable_music_control();
  }
  void control_dreamview_music(uint8_t effect) {
    if (!this->controller_.request_dreamview_music(effect, millis()))
      ESP_LOGW("govee_h6076", "DreamView music request rejected: synchronization must be active and controller idle");
  }
#endif
#ifdef USE_TEXT_SENSOR
  void set_dreamview_status(text_sensor::TextSensor *entity) { this->dreamview_status_ = entity; }
#endif
  void control_dreamview(protocol::DreamView::Operation operation) {
    if (!this->controller_.request_dreamview(operation, millis()))
      ESP_LOGW("govee_h6076", "DreamView request rejected: controller busy, sleep active, or no members configured");
  }
#endif
  void set_native_sleep(bool enabled) { this->native_sleep_enabled_ = enabled; }
#ifdef USE_TIME
  void set_time_source(time::RealTimeClock *source) { this->time_source_ = source; }
#endif
  int native_timer_count() const { return this->controller_.active_timer_count(); }
  int native_wake_enabled() const { return this->controller_.wake_enabled(); }
  int native_wake_status() const { return this->controller_.wake_raw_status(); }
  bool start_native_sleep(int minutes);
  void cancel_native_sleep() {
    if (this->native_sleep_enabled_)
      this->controller_.cancel_sleep();
  }
  const char *native_sleep_state() const {
    if (!this->native_sleep_enabled_)
      return "disabled";
    if (this->controller_.sleep_cancel_failed() || this->controller_.failed())
      return "error";
    if (this->controller_.clock_sync_failed())
      return "clock_error";
    if (this->controller_.active_timer_count() < 0 || this->controller_.wake_enabled() < 0)
      return "waiting_for_timer_inventory";
    if (!this->controller_.clock_inventory_clear())
      return "stored_schedule_active";
#ifdef USE_TIME
    if (this->time_source_ == nullptr || !this->time_source_->now().is_valid())
      return "waiting_for_time_source";
    if (!this->controller_.clock_synced())
      return "waiting_for_clock";
#endif
    if (!this->controller_.sleep_status_known())
      return "unknown";
    if (this->controller_.busy())
      return "pending";
    return this->controller_.sleep_status().enabled ? "running" : "idle";
  }
  float native_sleep_remaining() const {
    if (!this->native_sleep_enabled_ || !this->controller_.sleep_status_known())
      return NAN;
    return this->controller_.sleep_status().enabled ? this->controller_.sleep_status().remaining_minutes : 0;
  }
#ifdef USE_NUMBER
  void set_sleep_timer(GoveeSleepNumber *timer) { this->sleep_timer_ = timer; }
#endif
#ifdef USE_SENSOR
  void set_sleep_remaining_sensor(sensor::Sensor *entity) { this->sleep_remaining_sensor_ = entity; }
  void set_native_timer_count_sensor(sensor::Sensor *entity) { this->native_timer_count_sensor_ = entity; }
  void set_native_wake_status_sensor(sensor::Sensor *entity) { this->native_wake_status_sensor_ = entity; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_sleep_status_text_sensor(text_sensor::TextSensor *entity) { this->sleep_status_text_sensor_ = entity; }
  void set_native_timer_summary_text_sensor(text_sensor::TextSensor *entity) {
    this->native_timer_summary_text_sensor_ = entity;
  }
#endif
  void start_music(uint8_t mode, const protocol::MusicSettings &settings = {}) {
    this->music_settings_ = settings;
    this->music_mode_ = mode;
    this->controller_.start_music(this->music_sensitivity_, this->music_mode_, this->music_settings_);
  }
  void set_music_sensitivity(uint8_t sensitivity) {
    this->music_sensitivity_ = std::min<unsigned>(sensitivity, 99);
    if (this->controller_.music_requested())
      this->controller_.start_music(this->music_sensitivity_, this->music_mode_, this->music_settings_);
  }
  bool set_custom_brightness_shape(int shape) {
    if (shape < 0 || shape > 5 || this->music_mode_ != 0xFF || !this->controller_.music_requested())
      return false;
    this->music_settings_.custom_brightness_shape = static_cast<uint8_t>(shape);
    this->controller_.start_music(this->music_sensitivity_, this->music_mode_, this->music_settings_);
    return true;
  }
  void set_music_gradient(bool gradient) {
    if (this->music_mode_ != 0x32 && this->music_mode_ != 0x34 && this->music_mode_ != 0x37)
      return;
    this->music_settings_.gradient = gradient;
    if (this->controller_.music_requested())
      this->controller_.start_music(this->music_sensitivity_, this->music_mode_, this->music_settings_);
  }
  void set_music_background(uint32_t rgb, uint8_t brightness) {
    if ((this->music_mode_ != 0x33 && this->music_mode_ != 0x98) || brightness > 50)
      return;
    this->music_settings_.has_background = true;
    this->music_settings_.background_color = rgb;
    this->music_settings_.background_brightness = brightness;
    if (this->controller_.music_requested())
      this->controller_.start_music(this->music_sensitivity_, this->music_mode_, this->music_settings_);
  }
  void set_music_color(uint8_t index, uint32_t rgb) {
    if (index >= 7)
      return;
    this->palette_[index * 3] = rgb >> 16;
    this->palette_[index * 3 + 1] = rgb >> 8;
    this->palette_[index * 3 + 2] = rgb;
    this->controller_.set_palette(this->palette_);
  }
  void start_scene(uint16_t scene, const std::vector<uint8_t> &body) {
    this->controller_.start_scene(scene, body.data(), body.size());
  }
  void start_segments(const protocol::Segments &segments) { this->controller_.start_segments(segments); }
  void stop_music() { this->controller_.stop_music(); }
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
#ifdef USE_GOVEE_DREAMVIEW
  void publish_dreamview_();
  bool dreamview_enabled_{false};
  ESPPreferenceObject dreamview_preference_;
  protocol::DreamViewMembers dreamview_members_{};
  size_t dreamview_member_count_{0};
  GoveeDreamViewSwitch *dreamview_switch_{nullptr};
#ifdef USE_GOVEE_DREAMVIEW_MUSIC
  GoveeDreamViewSelect *dreamview_music_effect_{nullptr};
#endif
  binary_sensor::BinarySensor *dreamview_connection_{nullptr};
  binary_sensor::BinarySensor *dreamview_problem_{nullptr};
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *dreamview_status_{nullptr};
#endif
#endif
  void publish_controller_state_(bool cancel_music = false);
  void publish_sleep_entities_();
#ifdef USE_SENSOR
  sensor::Sensor *sleep_remaining_sensor_{nullptr};
  sensor::Sensor *native_timer_count_sensor_{nullptr};
  sensor::Sensor *native_wake_status_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *sleep_status_text_sensor_{nullptr};
  text_sensor::TextSensor *native_timer_summary_text_sensor_{nullptr};
#endif
#ifdef USE_NUMBER
  GoveeSleepNumber *sleep_timer_{nullptr};
#endif
  uint32_t last_sleep_publish_{0};
  light::LightState *state_{nullptr};
  protocol::Palette palette_{protocol::RAINBOW};
  protocol::Parser parser_;
  protocol::Controller controller_;
  protocol::StaticRgbBrightness static_rgb_brightness_;
  static constexpr uint16_t WHITE_TEMPERATURE = 2200;
  bool publishing_{false}, initial_state_synced_{false};
  bool native_sleep_enabled_{false};
#ifdef USE_TIME
  time::RealTimeClock *time_source_{nullptr};
#endif
  uint32_t last_sleep_poll_{0};
  uint8_t music_sensitivity_{99}, music_mode_{0x00};
  protocol::MusicSettings music_settings_{};
};

#ifdef USE_GOVEE_DREAMVIEW
class GoveeDreamViewButton : public button::Button {
 public:
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }

 protected:
  void press_action() override {
    this->parent_->control_dreamview(protocol::DreamView::Operation::REMOVE);
  }
  GoveeLight *parent_{nullptr};
};
#endif

class GoveeMusicEffect final : public light::LightEffect {
 public:
  explicit GoveeMusicEffect(const char *name) : LightEffect(name) {}
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }
  void set_mode(uint8_t mode) { this->mode_ = mode; }
  void set_color(uint32_t rgb) {
    this->settings_.configured = this->settings_.fixed_color = true;
    this->settings_.red = rgb >> 16;
    this->settings_.green = rgb >> 8;
    this->settings_.blue = rgb;
  }
  void set_gradient(bool gradient) { this->settings_.gradient = gradient; }
  void set_custom_action_type(uint8_t value) { this->settings_.custom_action_type = value; }
  void set_custom_direction(uint8_t value) { this->settings_.custom_direction = value; }
  void set_custom_idle_brightness(uint8_t value) { this->settings_.custom_idle_brightness = value; }
  void set_custom_decay_speed(uint8_t value) { this->settings_.custom_decay_speed = value; }
  void set_custom_brightness_min(uint8_t value) { this->settings_.custom_brightness_min = value; }
  void set_custom_brightness_max(uint8_t value) { this->settings_.custom_brightness_max = value; }
  void set_custom_brightness_shape(uint8_t value) { this->settings_.custom_brightness_shape = value; }
  void set_custom_spot_length(uint8_t value) { this->settings_.custom_spot_length = value; }
  void set_piano_bars(uint8_t value) { this->settings_.piano_bars = value; }
  void set_custom_color_order(uint8_t value) { this->settings_.custom_color_order = value; }
  void set_custom_color_change(uint8_t value) { this->settings_.custom_color_change = value; }
  void set_custom_color_distribution(uint8_t value) { this->settings_.custom_color_distribution = value; }
  void set_background_color(uint32_t rgb) {
    this->settings_.has_background = true;
    this->settings_.background_color = rgb;
  }
  void set_background_brightness(uint8_t value) { this->settings_.background_brightness = value; }
  void set_calm(bool calm) {
    this->settings_.configured = true;
    this->settings_.calm = calm;
  }
  void start() override { this->parent_->start_music(this->mode_, this->settings_); }
  void stop() override { this->parent_->stop_music(); }
  void apply() override {}  // Audio and animation run entirely on the FR8018H.

 protected:
  GoveeLight *parent_{nullptr};
  uint8_t mode_{0x00};
  protocol::MusicSettings settings_{};
};

class GoveeSceneEffect final : public light::LightEffect {
 public:
  explicit GoveeSceneEffect(const char *name) : LightEffect(name) {}
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }
  void set_scene(uint16_t scene) { this->scene_ = scene; }
  void set_body(const std::vector<uint8_t> &body) { this->body_ = body; }
  void start() override { this->parent_->start_scene(this->scene_, this->body_); }
  void stop() override { this->parent_->stop_music(); }
  void apply() override {}

 protected:
  GoveeLight *parent_{nullptr};
  uint16_t scene_{0};
  std::vector<uint8_t> body_;
};

#ifdef USE_GOVEE_SEGMENT_LIGHTS
class GoveeSegmentsEffect;

class GoveeSegmentLight final : public light::LightOutput {
 public:
  void set_parent(GoveeSegmentsEffect *parent) { this->parent_ = parent; }
  void set_segment_index(size_t index) { this->segment_index_ = index; }
  void setup_state(light::LightState *state) override { this->state_ = state; }
  light::LightTraits get_traits() override {
    light::LightTraits traits;
    traits.set_supported_color_modes({light::ColorMode::RGB});
    return traits;
  }
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *) override;
  void publish_segment(const protocol::Segment &segment, bool active);

 protected:
  GoveeSegmentsEffect *parent_{nullptr};
  light::LightState *state_{nullptr};
  size_t segment_index_{0};
  protocol::Segment published_segment_{};
  bool initialized_{false}, publishing_{false}, controlling_{false}, has_published_{false}, published_active_{false};
};
#endif

class GoveeSegmentsEffect final : public light::LightEffect
#ifdef USE_GOVEE_SEGMENT_LIGHTS
    , public light::LightRemoteValuesListener
#endif
{
 public:
  explicit GoveeSegmentsEffect(const char *name) : LightEffect(name) {}
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }
  void set_segment(size_t index, uint32_t color, uint8_t brightness) {
    if (index >= this->segments_.size() || brightness > 100)
      return;
    this->segments_[index] = {uint8_t(color >> 16), uint8_t(color >> 8), uint8_t(color), brightness};
    if (this->active_)
      this->parent_->start_segments(this->segments_);
#ifdef USE_GOVEE_SEGMENT_LIGHTS
    this->publish_segments_();
#endif
  }
#ifdef USE_GOVEE_SEGMENT_LIGHTS
  void init() override;
  void set_segment_light(size_t index, GoveeSegmentLight *output) { this->segment_lights_[index] = output; }
  void control_segment(size_t index, uint32_t color, uint8_t brightness);
  void on_light_remote_values_update() override { this->publish_segments_(); }
#endif
  void start() override {
    this->active_ = true;
    this->parent_->start_segments(this->segments_);
  }
  void stop() override {
    this->active_ = false;
    this->parent_->stop_music();
#ifdef USE_GOVEE_SEGMENT_LIGHTS
    this->publish_segments_();
#endif
  }
  void apply() override {}

 protected:
#ifdef USE_GOVEE_SEGMENT_LIGHTS
  void publish_segments_();
  std::array<GoveeSegmentLight *, 7> segment_lights_{};
#endif
  GoveeLight *parent_{nullptr};
  protocol::Segments segments_{};
  bool active_{false};
};

#ifdef USE_NUMBER
class GoveeSleepNumber final : public number::Number {
 public:
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }
  void publish_minutes(uint8_t minutes) {
    if (!this->has_state() || this->state != minutes)
      this->publish_state(minutes);
  }

 protected:
  void control(float value) override {
    if (!std::isfinite(value) || value < 0 || value > 240 || std::floor(value) != value)
      return;
    if (value == 0) {
      this->parent_->cancel_native_sleep();
    } else if (!this->parent_->start_native_sleep(static_cast<int>(value))) {
      ESP_LOGW("govee_h6076",
               "Sleep mode: 10 to 240 minutes, the light must be on, and brightness must be at least 10%%");
    }
    // The lamp controller feedback is the only confirmation of the new duration.
  }
  GoveeLight *parent_{nullptr};
};

class GoveeMusicSensitivityNumber final : public number::Number, public Component {
 public:
  void set_parent(GoveeLight *parent) { this->parent_ = parent; }
  void set_initial_value(uint8_t value) { this->initial_value_ = value; }
  void setup() override {
    this->preference_ = this->make_entity_preference<uint8_t>();
    uint8_t value = this->initial_value_;
    uint8_t restored;
    if (this->preference_.load(&restored) && restored <= 99)
      value = restored;
    this->parent_->set_music_sensitivity(value);
    this->publish_state(value);
  }

 protected:
  void control(float value) override {
    if (!std::isfinite(value))
      return;
    const auto setting = static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 99.0f)));
    this->parent_->set_music_sensitivity(setting);
    this->preference_.save(&setting);
    this->publish_state(setting);
  }
  GoveeLight *parent_{nullptr};
  ESPPreferenceObject preference_;
  uint8_t initial_value_{99};
};
#endif

}  // namespace esphome::govee_h6076

#include "automation.h"
