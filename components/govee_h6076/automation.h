#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/log.h"
#include "esphome/components/light/light_state.h"
#include "govee_h6076.h"

namespace esphome::govee_h6076 {

template<typename... Ts> class StartSleepAction final : public Action<Ts...> {
 public:
  explicit StartSleepAction(GoveeLight *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(int, minutes)
  void play(const Ts &...x) override {
    if (!this->parent_->start_native_sleep(this->minutes_.value(x...)))
      ESP_LOGW("govee_h6076", "Sleep mode rejected: check the state, duration, and minimum brightness");
  }

 protected:
  GoveeLight *parent_;
};

template<typename... Ts> class CancelSleepAction final : public Action<Ts...> {
 public:
  explicit CancelSleepAction(GoveeLight *parent) : parent_(parent) {}
  void play(const Ts &...) override { this->parent_->cancel_native_sleep(); }

 protected:
  GoveeLight *parent_;
};

template<typename... Ts> class SetSegmentAction final : public Action<Ts...> {
 public:
  SetSegmentAction(GoveeSegmentsEffect *effect, light::LightState *state) : effect_(effect), state_(state) {}
  TEMPLATABLE_VALUE(int, segment)
  TEMPLATABLE_VALUE(int, red)
  TEMPLATABLE_VALUE(int, green)
  TEMPLATABLE_VALUE(int, blue)
  TEMPLATABLE_VALUE(int, brightness)
  void play(const Ts &...x) override {
    const int segment = this->segment_.value(x...);
    const int red = this->red_.value(x...);
    const int green = this->green_.value(x...);
    const int blue = this->blue_.value(x...);
    const int brightness = this->brightness_.value(x...);
    if (segment < 1 || segment > 7 || red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255 ||
        brightness < 0 || brightness > 100)
      return;
    if (this->effect_->get_light_state() != this->state_)
      return;
    this->effect_->set_segment(segment - 1, (uint32_t(red) << 16) | (uint32_t(green) << 8) | uint32_t(blue),
                               brightness);
    auto call = this->state_->turn_on();
    call.set_effect(this->effect_->get_index());
    call.perform();
  }

 protected:
  GoveeSegmentsEffect *effect_;
  light::LightState *state_;
};

template<typename... Ts> class SetMusicColorAction final : public Action<Ts...> {
 public:
  explicit SetMusicColorAction(GoveeLight *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(int, color_index)
  TEMPLATABLE_VALUE(int, red)
  TEMPLATABLE_VALUE(int, green)
  TEMPLATABLE_VALUE(int, blue)
  void play(const Ts &...x) override {
    const int index = this->color_index_.value(x...);
    const int red = this->red_.value(x...);
    const int green = this->green_.value(x...);
    const int blue = this->blue_.value(x...);
    if (index < 1 || index > 7 || red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255)
      return;
    this->parent_->set_music_color(index - 1, (uint32_t(red) << 16) | (uint32_t(green) << 8) | uint32_t(blue));
  }

 protected:
  GoveeLight *parent_;
};

template<typename... Ts> class SetMusicBackgroundAction final : public Action<Ts...> {
 public:
  explicit SetMusicBackgroundAction(GoveeLight *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(int, red)
  TEMPLATABLE_VALUE(int, green)
  TEMPLATABLE_VALUE(int, blue)
  TEMPLATABLE_VALUE(int, brightness)
  void play(const Ts &...x) override {
    const int red = this->red_.value(x...);
    const int green = this->green_.value(x...);
    const int blue = this->blue_.value(x...);
    const int brightness = this->brightness_.value(x...);
    if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255 || brightness < 0 || brightness > 50)
      return;
    this->parent_->set_music_background((uint32_t(red) << 16) | (uint32_t(green) << 8) | uint32_t(blue), brightness);
  }

 protected:
  GoveeLight *parent_;
};

template<typename... Ts> class SetMusicGradientAction final : public Action<Ts...> {
 public:
  explicit SetMusicGradientAction(GoveeLight *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, gradient)
  void play(const Ts &...x) override { this->parent_->set_music_gradient(this->gradient_.value(x...)); }

 protected:
  GoveeLight *parent_;
};

template<typename... Ts> class SetCustomBrightnessShapeAction final : public Action<Ts...> {
 public:
  explicit SetCustomBrightnessShapeAction(GoveeLight *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(int, shape)
  void play(const Ts &...x) override { this->parent_->set_custom_brightness_shape(this->shape_.value(x...)); }

 protected:
  GoveeLight *parent_;
};

}  // namespace esphome::govee_h6076
