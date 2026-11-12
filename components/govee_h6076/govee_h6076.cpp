#include "govee_h6076.h"

#include <algorithm>
#include <cmath>
#include "esphome/core/hal.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_CDC
#error "GPIO18/19 must be reserved for the FR8018H UART, not a USB console."
#endif
#endif

namespace esphome::govee_h6076 {

static const char *const TAG = "govee_h6076";

#ifdef USE_GOVEE_SEGMENT_LIGHTS
void GoveeSegmentLight::update_state(light::LightState *state) {
  // LightState restores itself during setup; only later user commands select a pattern.
  if (this->publishing_ || !this->initialized_)
    return;
  const auto &values = state->current_values;
  const auto channel = [state, &values](float value) {
    return static_cast<uint8_t>(std::lround(state->gamma_correct_lut(value * values.get_color_brightness()) * 255));
  };
  const uint32_t color = (uint32_t(channel(values.get_red())) << 16) |
                         (uint32_t(channel(values.get_green())) << 8) | channel(values.get_blue());
  const uint8_t brightness =
      values.is_on() ? static_cast<uint8_t>(std::lround(state->gamma_correct_lut(values.get_brightness()) * 100)) : 0;
  this->controlling_ = true;
  this->parent_->control_segment(this->segment_index_, color, brightness);
  this->controlling_ = false;
}

void GoveeSegmentLight::write_state(light::LightState *) { this->initialized_ = true; }

void GoveeSegmentLight::publish_segment(const protocol::Segment &segment, bool active) {
  if (!this->initialized_)
    return;
  const bool on = active && segment.brightness != 0;
  const auto &previous = this->published_segment_;
  if (this->has_published_ && this->published_active_ == on && previous.red == segment.red &&
      previous.green == segment.green && previous.blue == segment.blue && previous.brightness == segment.brightness)
    return;
  this->published_segment_ = segment;
  this->published_active_ = on;
  this->has_published_ = true;
  // Preserve an in-flight transition, but do not cache a power state that was not published.
  if (this->controlling_) {
    if (on != this->state_->remote_values.is_on())
      this->has_published_ = false;
    return;
  }
  this->publishing_ = true;
  auto call = this->state_->make_call();
  call.set_state(on);
  const float red = this->state_->gamma_uncorrect_lut(segment.red / 255.0f);
  const float green = this->state_->gamma_uncorrect_lut(segment.green / 255.0f);
  const float blue = this->state_->gamma_uncorrect_lut(segment.blue / 255.0f);
  // LightCall normalizes RGB channels; retain their intensity separately, including black.
  call.set_rgb(red, green, blue);
  call.set_color_brightness(std::max({red, green, blue}));
  // Keep the last nonzero brightness available for the next turn-on command.
  if (segment.brightness != 0)
    call.set_brightness(this->state_->gamma_uncorrect_lut(segment.brightness / 100.0f));
  call.set_transition_length(0);
  call.set_save(false);
  call.perform();
  this->publishing_ = false;
}

void GoveeSegmentsEffect::init() {
  this->state_->add_remote_values_listener(this);
}

void GoveeSegmentsEffect::control_segment(size_t index, uint32_t color, uint8_t brightness) {
  this->set_segment(index, color, brightness);
  if (this->state_ == nullptr || (brightness == 0 && !this->active_))
    return;
  const bool any_on = std::any_of(this->segments_.begin(), this->segments_.end(),
                                [](const protocol::Segment &segment) { return segment.brightness != 0; });
  auto call = this->state_->make_call();
  call.set_state(any_on);
  if (any_on)
    call.set_effect(this->get_index());
  call.set_transition_length(0);
  call.perform();
}

void GoveeSegmentsEffect::publish_segments_() {
  const bool active = this->active_ && this->state_ != nullptr && this->state_->remote_values.is_on();
  for (size_t i = 0; i < this->segment_lights_.size(); ++i) {
    if (this->segment_lights_[i] != nullptr)
      this->segment_lights_[i]->publish_segment(this->segments_[i], active);
  }
}
#endif

void GoveeLight::setup() {
  this->controller_.begin(millis());
#ifdef USE_GOVEE_DREAMVIEW
  if (this->dreamview_enabled_) {
    // Separate the group receipt from the light's own restore preference.
    this->dreamview_preference_ = global_preferences->make_preference<protocol::DreamViewStoredGroup>(
        this->state_->get_object_id_hash() ^ 0x476F7644U);
    protocol::DreamViewStoredGroup stored;
    this->dreamview_preference_.load(&stored);
    this->controller_.dreamview().set_members(this->dreamview_members_, this->dreamview_member_count_, stored);
  }
#endif
  if (this->native_sleep_enabled_) {
    this->controller_.enable_sleep_support();
    this->controller_.request_timer_inventory();
  }
  this->status_set_warning();
}

bool GoveeLight::start_native_sleep(int minutes) {
  if (!this->native_sleep_enabled_ || !this->controller_.connected(millis()) || minutes < 10 || minutes > 240)
    return false;
  const auto &state = this->controller_.state();
  const uint8_t brightness =
      state.temperature == 0 && !this->controller_.effect_requested()
          ? static_cast<uint8_t>(
                std::lround(this->state_->gamma_correct_lut(
                                this->static_rgb_brightness_.brightness_percent(
                                    state, [this](float value) { return this->state_->gamma_uncorrect_lut(value); }) /
                                100.0f) *
                            100))
          : state.brightness;
  if (brightness < 10 || brightness > 100)
    return false;
  protocol::SleepSettings settings;
  settings.minutes = minutes;
  settings.brightness = brightness;
  if (!this->controller_.start_sleep(settings))
    return false;
  this->publish_controller_state_(true);
  return true;
}

light::LightTraits GoveeLight::get_traits() {
  light::LightTraits traits;
  // The H6076 manual specifies RGB and a fixed warm-white light source (2200 K).
  traits.set_supported_color_modes({light::ColorMode::RGB, light::ColorMode::WHITE});
  return traits;
}

void GoveeLight::update_state(light::LightState *state) {
  if (this->publishing_)
    return;
  const auto &values = state->current_values;
  const auto correct = [state](float value) { return state->gamma_correct_lut(value); };
  const protocol::RgbLightValues rgb_values{values.get_brightness(), values.get_color_brightness(), values.get_red(),
                                            values.get_green(), values.get_blue()};
  if (!this->controller_.ready()) {
    // Associate restore values only; this does not switch the running FR.
    if (values.get_color_mode() == light::ColorMode::RGB && !this->controller_.effect_requested())
      this->static_rgb_brightness_.restore(rgb_values, correct);
    return;
  }
  auto target = this->controller_.state();
  target.power = values.is_on();
  if (!target.power) {
    // Color writes can switch on the FR. Preserve its confirmed color and brightness when switching off.
    this->controller_.request(target);
    return;
  }
  target.brightness = static_cast<uint8_t>(std::lround(correct(values.get_brightness()) * 100));
  if (this->controller_.effect_requested()) {
    // Brightness and power remain controllable without stopping music by setting a color.
  } else if (values.get_color_mode() == light::ColorMode::WHITE) {
    target.brightness = static_cast<uint8_t>(std::lround(correct(values.get_brightness() * values.get_white()) * 100));
    target.red = target.green = target.blue = 0;
    target.temperature = this->WHITE_TEMPERATURE;
  } else {
    target = this->static_rgb_brightness_.request(target, rgb_values, correct);
  }
  this->controller_.request(target);
}

void GoveeLight::publish_controller_state_(bool cancel_music) {
  if (this->state_ == nullptr)
    return;
  const auto &value = this->controller_.state();
  this->publishing_ = true;
  auto call = this->state_->make_call();
  call.set_state(value.power);
  if (cancel_music)
    call.set_effect("None");
  call.set_brightness(this->state_->gamma_uncorrect_lut(value.brightness / 100.0f));
  if (value.temperature != 0) {
    call.set_color_mode(light::ColorMode::WHITE);
    call.set_white(1.0f);
  } else {
    const auto rgb = this->static_rgb_brightness_.for_publication(
        value, this->controller_.effect_requested() || this->controller_.sleep_may_be_active(),
        [this](float channel) { return this->state_->gamma_uncorrect_lut(channel); });
    call.set_color_mode(light::ColorMode::RGB);
    call.set_brightness(rgb.brightness);
    call.set_color_brightness(rgb.color_brightness);
    call.set_rgb(rgb.red, rgb.green, rgb.blue);
  }
  call.set_transition_length(0);
  call.set_publish(true);
  call.set_save(true);
  call.perform();
  this->publishing_ = false;
}

void GoveeLight::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  this->parser_.expire(now);
  // Wi-Fi and the API still get regular time to run, even with continuous input.
  for (size_t budget = 0; budget < 256 && this->available(); ++budget) {
    uint8_t byte;
    if (!this->read_byte(&byte))
      break;
    this->parser_.feed(byte, now, [this, now](uint8_t command, const uint8_t *data, size_t length) {
      this->controller_.receive(command, data, length, now);
    });
  }
#ifdef USE_GOVEE_DREAMVIEW
  auto &dreamview = this->controller_.dreamview();
  if (dreamview.storage_pending()) {
    const bool saved = this->dreamview_preference_.save(&dreamview.stored_group()) && global_preferences->sync();
    dreamview.storage_complete(saved);
    if (!saved)
      ESP_LOGW(TAG, "DreamView group state could not be saved");
  }
#endif
  if (this->controller_.ready() && !this->initial_state_synced_ &&
      !this->controller_.native_mode_protected() &&
      (!this->native_sleep_enabled_ || this->controller_.sleep_status_known())) {
    this->initial_state_synced_ = true;
    // The FR keeps running through an ESP reboot. Without a saved effect,
    // confirm its last static color once without changing power.
    if (!this->native_sleep_enabled_ || !this->controller_.sleep_status().enabled)
      this->controller_.sync_static_mode();
  }
  if (this->native_sleep_enabled_ && uint32_t(now - this->last_sleep_poll_) >= 5000 && !this->controller_.busy()) {
    this->last_sleep_poll_ = now;
    if (this->controller_.active_timer_count() < 0 || this->controller_.wake_enabled() < 0 ||
        this->controller_.clock_sync_failed())
      this->controller_.request_timer_inventory();
    else
      this->controller_.refresh_sleep_status();
  }
  if (this->native_sleep_enabled_ && uint32_t(now - this->last_sleep_publish_) >= 1000) {
    this->last_sleep_publish_ = now;
    this->publish_sleep_entities_();
  }
  this->controller_.tick(
      now, [this](const protocol::Packet &packet) { this->write_array(packet.bytes.data(), packet.size); });
#ifdef USE_TIME
  if (this->native_sleep_enabled_ && this->time_source_ != nullptr && !this->controller_.clock_synced() &&
      !this->controller_.busy()) {
    const auto current = this->time_source_->now();
    if (current.is_valid())
      this->controller_.sync_clock(current.hour, current.minute, current.second, current.day_of_week);
  }
#endif
#ifdef USE_GOVEE_DREAMVIEW
  this->publish_dreamview_();
#endif
  if (this->controller_.take_music_cancelled())
    this->publish_controller_state_(true);
  if (this->controller_.take_music_failure()) {
    ESP_LOGW(TAG, "Effect command failed; restoring static light");
    this->publish_controller_state_(true);
  }
  if (this->controller_.take_update())
    this->publish_controller_state_();
  if (this->controller_.connected(now) && !this->controller_.failed())
    this->status_clear_warning();
  else
    this->status_set_warning();
}

void GoveeLight::publish_sleep_entities_() {
#ifdef USE_NUMBER
  if (this->sleep_timer_ != nullptr && this->controller_.sleep_status_known() && !this->controller_.busy()) {
    const auto &status = this->controller_.sleep_status();
    this->sleep_timer_->publish_minutes(status.enabled ? status.settings.minutes : 0);
  }
#endif
#ifdef USE_SENSOR
  const auto publish_sensor = [](sensor::Sensor *entity, float value) {
    if (entity != nullptr && (!entity->has_state() || entity->state != value) &&
        !(entity->has_state() && std::isnan(entity->state) && std::isnan(value)))
      entity->publish_state(value);
  };
#endif
#ifdef USE_TEXT_SENSOR
  const auto publish_text = [](text_sensor::TextSensor *entity, const char *value) {
    if (entity != nullptr && (!entity->has_state() || entity->state != value))
      entity->publish_state(value);
  };
#endif
#ifdef USE_SENSOR
  publish_sensor(this->sleep_remaining_sensor_, this->native_sleep_remaining());
  const int timer_count = this->native_timer_count();
  publish_sensor(this->native_timer_count_sensor_, timer_count < 0 ? NAN : timer_count);
  const int wake_status = this->native_wake_status();
  publish_sensor(this->native_wake_status_sensor_, wake_status < 0 ? NAN : wake_status);
#endif
#ifdef USE_TEXT_SENSOR
  publish_text(this->sleep_status_text_sensor_, this->native_sleep_state());
  if (this->native_timer_summary_text_sensor_ != nullptr) {
    if (this->controller_.active_timer_count() < 0) {
      publish_text(this->native_timer_summary_text_sensor_, "unknown");
    } else {
      const auto summary = protocol::format_timer_summary(this->controller_.native_timers());
      publish_text(this->native_timer_summary_text_sensor_, summary.data());
    }
  }
#endif
}

#ifdef USE_GOVEE_DREAMVIEW
void GoveeDreamViewSwitch::write_state(bool state) {
  this->parent_->control_dreamview(state ? protocol::DreamView::Operation::ENABLE
                                       : protocol::DreamView::Operation::DISABLE);
}

void GoveeLight::publish_dreamview_() {
  const auto &dreamview = this->controller_.dreamview();
  if (!dreamview.tracking())
    return;
  // Activation and peer connectivity are independent hardware states. An enabled
  // session must remain switchable off even when a member cannot connect.
  if (this->dreamview_switch_ != nullptr)
    this->dreamview_switch_->publish_native_state(dreamview.enabled_known(), dreamview.enabled());
#ifdef USE_GOVEE_DREAMVIEW_MUSIC
  if (this->dreamview_music_effect_ != nullptr)
    this->dreamview_music_effect_->publish_native_state(dreamview.music_known(), dreamview.music_effect());
#endif
  if (this->dreamview_connection_ != nullptr) {
    if (dreamview.connection_known())
      this->dreamview_connection_->publish_state(dreamview.connected());
    else if (this->dreamview_connection_->has_state())
      this->dreamview_connection_->invalidate_state();
  }
  if (this->dreamview_problem_ != nullptr) {
    if (dreamview.enabled_known() || dreamview.failed())
      this->dreamview_problem_->publish_state(dreamview.failed());
    else if (this->dreamview_problem_->has_state())
      this->dreamview_problem_->invalidate_state();
  }
#ifdef USE_TEXT_SENSOR
  if (this->dreamview_status_ != nullptr &&
      (!this->dreamview_status_->has_state() || this->dreamview_status_->state != dreamview.status()))
    this->dreamview_status_->publish_state(dreamview.status());
#endif
}
#ifdef USE_GOVEE_DREAMVIEW_MUSIC
void GoveeDreamViewSelect::control(size_t index) { this->parent_->control_dreamview_music(index); }
#endif
#endif

void GoveeLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Component version: " GOVEE_H6076_VERSION);
  ESP_LOGCONFIG(TAG, "H6076 FR8018H UART light, warm white command: %u K", this->WHITE_TEMPERATURE);
}

}  // namespace esphome::govee_h6076
