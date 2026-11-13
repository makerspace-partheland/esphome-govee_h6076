#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <initializer_list>

// Standalone protocol derived from the preserved H6076 image; contains no manufacturer code.
namespace esphome::govee_h6076::protocol {

constexpr size_t MAX_PAYLOAD = 1024;
constexpr uint32_t FRAME_TIMEOUT_MS = 100;
constexpr uint32_t ACK_TIMEOUT_MS = 500;
constexpr uint32_t POLL_INTERVAL_MS = 5000;
constexpr uint32_t LINK_TIMEOUT_MS = 15000;

inline uint8_t checksum(const uint8_t *data, size_t length) {
  uint8_t sum = 0;
  for (size_t i = 0; i < length; ++i)
    sum += data[i];
  return sum;
}

// Up to twelve 20-byte control packets plus the UART frame; no firmware packets.
struct Packet {
  std::array<uint8_t, 248> bytes{};
  size_t size{0};
};

inline Packet encode(uint8_t command, const uint8_t *payload = nullptr, size_t length = 0) {
  Packet p;
  if (length > 240 || (length != 0 && payload == nullptr))
    return p;
  p.size = length + 8;
  p.bytes[0] = 0x55;
  p.bytes[1] = 0x01;
  p.bytes[2] = static_cast<uint8_t>(length);
  p.bytes[6] = command;
  if (length != 0)
    std::memcpy(p.bytes.data() + 7, payload, length);
  p.bytes[p.size - 1] = checksum(p.bytes.data(), p.size - 1);
  return p;
}

using Palette = std::array<uint8_t, 21>;
constexpr Palette RAINBOW{255, 0, 0, 255, 127, 0, 255, 255, 0, 0, 255, 0, 0, 0, 255, 0, 255, 255, 139, 0, 255};

#ifdef USE_GOVEE_DREAMVIEW
}  // namespace esphome::govee_h6076::protocol
#include "dreamview.h"
namespace esphome::govee_h6076::protocol {
#endif

struct SleepSettings {
  // Native minute resolution: after cancellation, the FR retains the seconds remainder.
  // A subsequent run can therefore end up to 59 seconds early (there is no seconds timer).
  uint16_t minutes{15};
  uint8_t brightness{30};
  bool custom_color{false};
  uint32_t rgb{0xFF3200};
  uint16_t temperature{0};
};

inline Packet clock_packet(int hour, int minute, int second, int weekday) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || weekday < 1 || weekday > 7)
    return {};
  // The auxiliary function at byte 6 remains disabled; synchronize only the time and day of the week.
  uint8_t data[20]{0x33, 0x09, uint8_t(hour), uint8_t(minute), uint8_t(second), uint8_t(weekday)};
  for (size_t i = 0; i < 19; ++i)
    data[19] ^= data[i];
  return encode(0x83, data, sizeof(data));
}

struct NativeTimer {
  bool enabled{false}, power{false};
  uint8_t hour{0}, minute{0}, repeat_mask{0};
};
using NativeTimers = std::array<NativeTimer, 4>;

inline std::array<char, 128> format_timer_summary(const NativeTimers &timers) {
  std::array<char, 128> summary{};
  size_t offset = 0;
  for (size_t i = 0; i < timers.size(); ++i) {
    const auto &timer = timers[i];
    const int written =
        std::snprintf(summary.data() + offset, summary.size() - offset, "%s%u:%s,%02u:%02u,%02X,%s", i == 0 ? "" : ";",
                      unsigned(i), timer.enabled ? "enabled" : "disabled", unsigned(timer.hour), unsigned(timer.minute),
                      unsigned(timer.repeat_mask), timer.power ? "on" : "off");
    if (written < 0 || static_cast<size_t>(written) >= summary.size() - offset)
      return {};
    offset += static_cast<size_t>(written);
  }
  return summary;
}

inline Packet timers_query_packet() {
  uint8_t data[20]{0xAA, 0x23, 0xFF};
  data[19] = 0x76;
  return encode(0x83, data, sizeof(data));
}

inline Packet wake_query_packet() {
  uint8_t data[20]{0xAA, 0x12};
  data[19] = 0xB8;
  return encode(0x83, data, sizeof(data));
}

inline bool decode_timers(const uint8_t *data, size_t length, NativeTimers &timers) {
  if (data == nullptr || length != 20 || data[0] != 0xAA || data[1] != 0x23 || data[2] != 0xFF)
    return false;
  uint8_t sum = 0;
  for (size_t i = 0; i < 20; ++i)
    sum ^= data[i];
  if (sum != 0)
    return false;
  NativeTimers parsed;
  for (size_t i = 0; i < 4; ++i) {
    const auto *entry = data + 3 + i * 4;
    if ((entry[0] & 0x7E) != 0 || entry[1] > 23 || entry[2] > 59)
      return false;
    parsed[i] = {bool(entry[0] & 0x80), bool(entry[0] & 1), entry[1], entry[2], entry[3]};
  }
  timers = parsed;
  return true;
}

// A native write response of 0 does not prove acceptance; read back the status to confirm it.
// Always set the remaining time together with the duration; a remaining value of 0 would underflow to 255 in the FR.
inline bool valid_sleep_settings(const SleepSettings &settings) {
  return settings.minutes >= 10 && settings.minutes <= 240 && settings.brightness >= 10 && settings.brightness <= 100 &&
         settings.rgb <= 0xFFFFFF &&
         (settings.temperature == 0 || (settings.temperature >= 2000 && settings.temperature <= 9000)) &&
         (!settings.custom_color || settings.rgb != 0);
}

inline Packet sleep_packet(bool enabled, const SleepSettings &requested = {}) {
  const bool valid = valid_sleep_settings(requested);
  if (enabled && !valid)
    return {};
  const SleepSettings settings = valid ? requested : SleepSettings{};
  // The FR treats black as no color specified, even when a temperature is set.
  if (settings.custom_color && settings.rgb == 0)
    return {};
  uint8_t data[20]{0x33,
                   0x11,
                   uint8_t(enabled),
                   settings.brightness,
                   uint8_t(settings.minutes),
                   uint8_t(settings.minutes),
                   uint8_t(settings.custom_color),
                   uint8_t(settings.rgb >> 16),
                   uint8_t(settings.rgb >> 8),
                   uint8_t(settings.rgb),
                   uint8_t(settings.temperature >> 8),
                   uint8_t(settings.temperature)};
  for (size_t i = 0; i < 19; ++i)
    data[19] ^= data[i];
  return encode(0x83, data, sizeof(data));
}

// Native status response AA 11: status data, not a write confirmation.
struct SleepStatus {
  bool enabled{false};
  uint8_t remaining_minutes{0};
  SleepSettings settings{};
};

inline Packet sleep_query_packet() {
  uint8_t data[20]{0xAA, 0x11};
  data[19] = 0xBB;
  return encode(0x83, data, sizeof(data));
}

inline bool decode_sleep_status(const uint8_t *data, size_t length, SleepStatus &status) {
  if (data == nullptr || length != 20 || data[0] != 0xAA || data[1] != 0x11 || data[2] > 1)
    return false;
  uint8_t sum = 0;
  for (size_t i = 0; i < 20; ++i)
    sum ^= data[i];
  if (sum != 0)
    return false;
  SleepStatus parsed;
  parsed.enabled = data[2] != 0;
  parsed.settings.brightness = data[3];
  parsed.settings.minutes = data[4];
  parsed.remaining_minutes = data[5];
  parsed.settings.rgb = (uint32_t(data[7]) << 16) | (uint32_t(data[8]) << 8) | data[9];
  parsed.settings.temperature = (uint16_t(data[10]) << 8) | data[11];
  if (!valid_sleep_settings(parsed.settings))
    return false;
  // Native black and inconsistent remaining times must also remain readable.
  // The stricter write limits apply only when setting the value again.
  parsed.settings.custom_color = data[6] != 0;
  status = parsed;
  return true;
}

// H6076 “from device”: 33 05 0f <mode> <0..99> <flag>, padded to 19 bytes
// and terminated with XOR. The outer UART frame uses addition.
inline bool palette_mode(uint8_t mode) {
  return mode == 0xFF || mode == 0x4A || mode == 0x5D || mode == 0x33 || mode == 0x34 || mode == 0x35 || mode == 0x59 ||
         mode == 0x98 || mode == 0x30 || mode == 0x31 || mode == 0x37 || mode == 0x8E || mode == 0x5A || mode == 0x71 ||
         mode == 0x16 || mode == 0x8F || mode == 0x4D || mode == 0x50 || mode == 0x4E || mode == 0x4F || mode == 0x51 ||
         mode == 0x32;
}
inline bool valid_music_mode(uint8_t mode) {
  return palette_mode(mode) || mode == 5 || mode == 0x90 || mode == 0x91 || mode == 0 || mode == 4 || mode == 6 ||
         mode == 8 || mode == 2 || mode == 7;
}
inline bool initialize_music_palette(uint8_t mode) { return mode == 5 || mode == 0x90 || mode == 0x91; }
inline bool music_setup_required(uint8_t mode) { return palette_mode(mode) || initialize_music_palette(mode); }
struct MusicSettings {
  bool configured{false}, fixed_color{false}, calm{false};
  uint8_t red{255}, green{0}, blue{0};
  bool has_background{false};
  bool gradient{false};
  uint32_t background_color{0};
  uint8_t background_brightness{50};
  uint8_t custom_action_type{1};
  uint8_t custom_brightness_min{20}, custom_brightness_max{100};
  uint8_t piano_bars{14}, custom_spot_length{0}, custom_brightness_shape{0};
  uint8_t custom_direction{5}, custom_idle_brightness{0}, custom_decay_speed{1}, custom_color_order{0},
      custom_color_change{0}, custom_color_distribution{0};
};
inline bool supports_music_settings(uint8_t mode) {
  return mode == 2 || mode == 4 || mode == 6 || mode == 7 || mode == 8;
}
inline Packet music_packet(uint8_t mode, uint8_t sensitivity, const MusicSettings &settings = {}) {
  if (!valid_music_mode(mode))
    return {};
  uint8_t data[20]{
      0x33, 5, 15, mode, uint8_t(std::min<unsigned>(sensitivity, 99)), uint8_t(mode == 0 || mode == 4 ? 1 : 0)};
  if (mode == 0)
    data[6] = 255;
  if (settings.configured) {
    if (!supports_music_settings(mode) || (settings.calm && mode != 4))
      return {};
    std::fill(data + 5, data + 19, 0);
    const size_t flag = mode == 4 ? 6 : 5;
    if (mode == 4)
      data[5] = settings.calm ? 0 : 1;
    data[flag] = settings.fixed_color ? 1 : 0;
    if (settings.fixed_color) {
      data[flag + 1] = settings.red;
      data[flag + 2] = settings.green;
      data[flag + 3] = settings.blue;
    }
  }
  for (size_t i = 0; i < 19; ++i)
    data[19] ^= data[i];
  return encode(0x83, data, sizeof(data));
}
// Configurable music profile; four related A3 packets precede the mode packet.
inline Packet custom_music_profile(const Palette &palette, uint8_t action_type = 1, uint8_t direction = 5,
                                   uint8_t idle_brightness = 0, uint8_t decay_speed = 1, uint8_t color_order = 0,
                                   uint8_t color_change = 0, uint8_t color_distribution = 0, uint8_t spot_length = 0,
                                   uint8_t brightness_shape = 0, uint8_t brightness_min = 20,
                                   uint8_t brightness_max = 100) {
  if (action_type < 1 || action_type > 6 || spot_length > 14 || brightness_shape > 5 || brightness_min > 100 ||
      brightness_max > 100 || brightness_min > brightness_max)
    return {};
  if ((direction != 4 && direction != 5) || idle_brightness > 100 || decay_speed < 1 || decay_speed > 5 ||
      color_order > 1 || color_change > 1 ||
      (color_distribution != 0 && color_distribution != 2 && color_distribution != 3))
    return {};
  if (action_type != 1 && (direction != 5 || idle_brightness != 0 || decay_speed != 1 || color_order != 0 ||
                           color_change != 0 || color_distribution != 0))
    return {};
  uint8_t body[56]{1, 56, 0, 5, 1, 0, 14, 0, 0, 0, 0, 20, 100, 1, 3, 0, 1, 2, 0, 7};
  body[0] = action_type;
  body[3] = direction;
  body[18] = color_distribution;
  body[10] = brightness_shape;
  body[11] = brightness_min;
  body[12] = brightness_max;
  if (spot_length != 0)
    body[16] = body[17] = spot_length;
  std::copy(palette.begin(), palette.end(), body + 20);
  const uint8_t tail[15]{0, 1, 1, 0, 0, 100, 1, 3, 0, 0, 0, 0, 0, 0, 1};
  std::copy(tail, tail + 15, body + 41);
  body[41] = color_change;
  body[44] = color_order;
  body[45] = idle_brightness;
  body[47] = decay_speed;
  uint8_t payload[59]{1, 4, 0x42};
  std::copy(body, body + 56, payload + 3);
  uint8_t frames[80]{};
  for (size_t index = 0; index < 4; ++index) {
    auto *frame = frames + index * 20;
    frame[0] = 0xA3;
    frame[1] = index == 3 ? 0xFF : uint8_t(index);
    const size_t offset = index * 17;
    std::copy(payload + offset, payload + std::min<size_t>(59, offset + 17), frame + 2);
    for (size_t i = 0; i < 19; ++i)
      frame[19] ^= frame[i];
  }
  return encode(0x83, frames, sizeof(frames));
}

inline Packet music_palette(uint8_t mode, const Palette &palette, const MusicSettings &settings = {}) {
  if (!palette_mode(mode))
    return {};
  if (mode == 0xFF)
    return custom_music_profile(
        palette, settings.custom_action_type, settings.custom_direction, settings.custom_idle_brightness,
        settings.custom_decay_speed, settings.custom_color_order, settings.custom_color_change,
        settings.custom_color_distribution, settings.custom_spot_length, settings.custom_brightness_shape,
        settings.custom_brightness_min, settings.custom_brightness_max);
  if (settings.background_brightness > 50 || settings.piano_bars < 1 || settings.piano_bars > 14)
    return {};
  const uint32_t background =
      settings.has_background ? settings.background_color : (mode == 0x98 ? 0x0000FF : 0xFF0000);
  const uint8_t bg_r = background >> 16, bg_g = background >> 8, bg_b = background;
  uint8_t payload[35]{1, 2, 0x41, mode, 7};
  std::copy(palette.begin(), palette.end(), payload + 5);
  size_t length = 26;
  const auto tail = [&](std::initializer_list<uint8_t> values) {
    std::copy(values.begin(), values.end(), payload + 26);
    length += values.size();
  };
  switch (mode) {
    case 0x30:
      tail({10, 80});
      break;
    case 0x31:
      tail({5, 100, 10});
      break;
    case 0x32:
      tail({2, uint8_t(settings.gradient), 0x61});
      break;
    case 0x33:
      tail({bg_r, bg_g, bg_b, settings.background_brightness, 0x62, 1, 3, 2, 6});
      break;
    case 0x34:
      tail({uint8_t(settings.gradient), settings.piano_bars, 10, 4, 7});
      break;
    case 0x35:
      tail({0, 1, 5, 80});
      break;
    case 0x37:
      tail({1, 10, uint8_t(settings.gradient)});
      break;
    case 0x4A:
      tail({0});
      break;
    case 0x5D:
      tail({8});
      break;
    case 0x59:
      tail({0, 2, 5});
      break;
    case 0x5A:
      tail({6});
      break;
    case 0x71:
      tail({0});
      break;
    case 0x98:
      tail({bg_r, bg_g, bg_b, 3});
      break;
    default:
      break;
  }
  const size_t count = (length + 16) / 17;
  payload[1] = uint8_t(count);
  uint8_t frames[60]{};
  for (size_t index = 0; index < count; ++index) {
    auto *frame = frames + index * 20;
    frame[0] = 0xA3;
    frame[1] = uint8_t(index + 1 == count ? 0xFF : index);
    const size_t offset = index * 17;
    std::copy(payload + offset, payload + std::min(length, offset + 17), frame + 2);
    for (size_t i = 0; i < 19; ++i)
      frame[19] ^= frame[i];
  }
  return encode(0x83, frames, count * 20);
}

inline bool valid_scene(uint8_t scene) {
  switch (scene) {
    case 2:
    case 41:
    case 99:
    case 36:
    case 38:
    case 40:
    case 22:
    case 50:
    case 35:
      return true;
    default:
      return false;
  }
}
// Type-2 scenes contain length-limited effect layers. Arbitrary commands are not allowed.
constexpr size_t MAX_SCENE_BODY = 196;
inline bool valid_scene_body(const uint8_t *body, size_t size) {
  if (body == nullptr || size < 2 || size > MAX_SCENE_BODY || body[0] == 0)
    return false;
  size_t offset = 1;
  for (size_t i = 0; i < body[0]; ++i) {
    if (offset >= size)
      return false;
    const size_t length = body[offset++];
    if (length == 0 || length > size - offset)
      return false;
    offset += length;
  }
  for (; offset < size; ++offset)
    if (body[offset] != 0)
      return false;
  return true;
}
inline size_t scene_frame_count(size_t size) { return std::max<size_t>(2, (size + 3 + 16) / 17); }
inline Packet scene_upload_packet(const uint8_t *body, size_t size, size_t index) {
  if (!valid_scene_body(body, size))
    return {};
  const size_t count = scene_frame_count(size);
  if (index >= count)
    return {};
  uint8_t frame[20]{0xA3, uint8_t(index + 1 == count ? 0xFF : index)};
  for (size_t i = 0; i < 17; ++i) {
    const size_t offset = index * 17 + i;
    if (offset == 0)
      frame[i + 2] = 1;
    else if (offset == 1)
      frame[i + 2] = uint8_t(count);
    else if (offset == 2)
      frame[i + 2] = 2;
    else if (offset - 3 < size)
      frame[i + 2] = body[offset - 3];
  }
  for (size_t i = 0; i < 19; ++i)
    frame[19] ^= frame[i];
  return encode(0x83, frame, sizeof(frame));
}
inline Packet scene_upload(const uint8_t *body, size_t size) {
  if (!valid_scene_body(body, size))
    return {};
  std::array<uint8_t, 240> frames{};
  const size_t count = scene_frame_count(size);
  for (size_t i = 0; i < count; ++i) {
    const auto frame = scene_upload_packet(body, size, i);
    std::copy(frame.bytes.begin() + 7, frame.bytes.begin() + 27, frames.begin() + 20 * i);
  }
  return encode(0x83, frames.data(), count * 20);
}
inline Packet scene_packet(uint16_t scene, bool uploaded = false) {
  if (!uploaded && (scene > 255 || !valid_scene(uint8_t(scene))))
    return {};
  uint8_t data[20]{0x33, 0x05, 0x04, uint8_t(scene), uint8_t(scene >> 8)};
  for (size_t i = 0; i < 19; ++i)
    data[19] ^= data[i];
  return encode(0x83, data, sizeof(data));
}

// The seven H6076 segments are selected with a bitmask from bottom to top.
// Mask 0 would select the entire lamp and is explicitly rejected here.
inline Packet segment_color(uint8_t mask, uint8_t red, uint8_t green, uint8_t blue) {
  if (mask == 0 || mask > 0x7F)
    return {};
  uint8_t frame[20]{0x33, 5, 0x15, 1, red, green, blue};
  frame[12] = mask;
  for (size_t i = 0; i < 19; ++i)
    frame[19] ^= frame[i];
  return encode(0x83, frame, sizeof(frame));
}

struct Segment {
  uint8_t red{255}, green{255}, blue{255}, brightness{100};
};
using Segments = std::array<Segment, 7>;
// Native segment brightness dims the tested FR firmware but leaves visible output at 0.
// RGB scaling preserves a true black output at zero brightness.
inline Packet segment_packet(size_t index, const Segment &segment) {
  if (index >= 7 || segment.brightness > 100)
    return {};
  const auto scale = [&segment](uint8_t value) -> uint8_t {
    return uint8_t((uint16_t(value) * segment.brightness + 50) / 100);
  };
  return segment_color(uint8_t(1U << index), scale(segment.red), scale(segment.green), scale(segment.blue));
}

class Parser {
 public:
  void expire(uint32_t now) {
    if (this->size_ != 0 && uint32_t(now - this->last_byte_) >= FRAME_TIMEOUT_MS)
      this->size_ = 0;
  }

  template<typename Callback> void feed(uint8_t byte, uint32_t now, Callback callback) {
    this->expire(now);
    this->last_byte_ = now;
    if (this->size_ == this->buffer_.size())
      this->discard_(1);
    this->buffer_[this->size_++] = byte;
    while (this->size_ >= 2) {
      if (this->buffer_[0] != 0x55 || this->buffer_[1] != 0x01) {
        this->discard_(1);
        continue;
      }
      if (this->size_ < 7)
        return;
      const uint32_t length = uint32_t(this->buffer_[2]) | (uint32_t(this->buffer_[3]) << 8) |
                              (uint32_t(this->buffer_[4]) << 16) | (uint32_t(this->buffer_[5]) << 24);
      if (length > MAX_PAYLOAD) {
        this->discard_(1);
        continue;
      }
      const size_t total = length + 8;
      if (this->size_ < total)
        return;
      if (checksum(this->buffer_.data(), total - 1) != this->buffer_[total - 1]) {
        this->discard_(1);
        continue;
      }
      callback(this->buffer_[6], this->buffer_.data() + 7, length);
      this->discard_(total);
    }
    if (this->size_ == 1 && this->buffer_[0] != 0x55)
      this->size_ = 0;
  }

 private:
  void discard_(size_t count) {
    this->size_ -= count;
    std::memmove(this->buffer_.data(), this->buffer_.data() + count, this->size_);
  }
  std::array<uint8_t, MAX_PAYLOAD + 8> buffer_{};
  size_t size_{0};
  uint32_t last_byte_{0};
};

struct State {
  bool power{false};
  uint8_t brightness{100};
  uint8_t red{255}, green{255}, blue{255};
  uint16_t temperature{0};
};

inline bool same_color(const State &a, const State &b) {
  return a.red == b.red && a.green == b.green && a.blue == b.blue && a.temperature == b.temperature;
}
inline bool operator==(const State &a, const State &b) {
  return a.power == b.power && a.brightness == b.brightness && same_color(a, b);
}

struct RgbLightValues {
  float brightness{1.0f}, color_brightness{1.0f};
  float red{1.0f}, green{1.0f}, blue{1.0f};
};

struct IdentityGamma {
  float operator()(float value) const { return value; }
};

// Logical Home Assistant colors remain separate from quantized UART amplitudes.
class StaticRgbBrightness {
 public:
  template<typename Gamma = IdentityGamma>
  State request(State target, const RgbLightValues &values, Gamma correct = {}) {
    this->requested_ = {StaticRgbBrightness::wire_values_(target, values, true, correct), values, true};
    this->restore_pending_ = false;
    return this->requested_.wire;
  }

  template<typename Gamma = IdentityGamma> void restore(const RgbLightValues &values, Gamma correct = {}) {
    request({}, values, correct);
    this->restore_pending_ = true;
  }

  template<typename Gamma = IdentityGamma> RgbLightValues resolve(const State &wire, Gamma uncorrect = {}) {
    // For static RGB, the amplitudes confirm the visible brightness.
    // The separate native percentage command may still be pending or may have failed.
    if (this->requested_.valid && same_color(wire, this->requested_.wire)) {
      this->confirmed_ = this->requested_;
      this->confirmed_.wire = wire;
      this->restore_pending_ = false;
      return this->confirmed_.values;
    }
    if (this->restore_pending_) {
      this->restore_pending_ = false;
      // The unscaled color from the previous firmware can also be associated with the logical restore
      // on the first status update, without sending it again.
      const auto previous_wire = StaticRgbBrightness::wire_values_({}, this->requested_.values, false, IdentityGamma{});
      const auto previous_scaled =
          StaticRgbBrightness::wire_values_({}, this->requested_.values, true, IdentityGamma{});
      if ((same_color(wire, previous_wire) || same_color(wire, previous_scaled)) &&
          wire.brightness == previous_wire.brightness) {
        this->confirmed_ = {wire, this->requested_.values, true};
        return this->confirmed_.values;
      }
    }
    // A native brightness-only report does not change the RGB amplitudes.
    if (this->confirmed_.valid && same_color(wire, this->confirmed_.wire))
      return this->confirmed_.values;
    RgbLightValues values;
    const float red = uncorrect(wire.red / 255.0f);
    const float green = uncorrect(wire.green / 255.0f);
    const float blue = uncorrect(wire.blue / 255.0f);
    const float maximum = std::max({red, green, blue});
    values.brightness = wire.brightness / 100.0f;
    if (maximum != 0) {
      // Colors set directly on the FR can be brighter than the ineffective native
      // RGB percentage value indicates. The conversion back must not clip them.
      values.brightness = std::max(values.brightness, maximum);
      values.red = red / maximum;
      values.green = green / maximum;
      values.blue = blue / maximum;
      values.color_brightness = maximum / values.brightness;
    } else {
      // The previous hue cannot be reconstructed from black.
      if (this->confirmed_.valid)
        values = this->confirmed_.values;
      else if (this->requested_.valid)
        values = this->requested_.values;
      values.brightness = wire.brightness / 100.0f;
      if (wire.brightness != 0)
        values.color_brightness = 0;
    }
    this->confirmed_ = {wire, values, true};
    return values;
  }

  template<typename Gamma = IdentityGamma> uint8_t brightness_percent(const State &wire, Gamma uncorrect = {}) {
    return static_cast<uint8_t>(std::lround(resolve(wire, uncorrect).brightness * 100));
  }

  template<typename Gamma = IdentityGamma>
  RgbLightValues for_publication(const State &wire, bool native_brightness, Gamma uncorrect = {}) {
    auto values = resolve(wire, uncorrect);
    // Effects and an active sleep mode continue to use the native
    // brightness; the stored RGB color remains independent of it.
    if (native_brightness)
      values.brightness = uncorrect(wire.brightness / 100.0f);
    return values;
  }

 private:
  template<typename Gamma>
  static State wire_values_(State target, const RgbLightValues &values, bool scale_brightness, Gamma correct) {
    target.brightness = static_cast<uint8_t>(std::lround(values.brightness * 100));
    const float brightness = scale_brightness ? values.brightness : 1.0f;
    const auto channel = [&](float value) {
      return static_cast<uint8_t>(std::lround(correct(value * values.color_brightness * brightness) * 255));
    };
    target.red = channel(values.red);
    target.green = channel(values.green);
    target.blue = channel(values.blue);
    target.temperature = 0;
    return target;
  }
  struct Snapshot {
    State wire;
    RgbLightValues values;
    bool valid{false};
  };
  Snapshot requested_, confirmed_;
  bool restore_pending_{false};
};

// Only one command at a time; new targets replace pending intermediate values.
class Controller {
 public:
  void begin(uint32_t now) { this->last_poll_ = now - POLL_INTERVAL_MS + 500; }
  bool ready() const { return this->ready_; }
  bool busy() const {
    return this->pending_ != 0 || this->dirty_ != 0 || this->sleep_dirty_ || this->sleep_query_dirty_ ||
           this->inventory_stage_ != 0 || this->clock_dirty_;
  }
#ifdef USE_GOVEE_DREAMVIEW
  DreamView &dreamview() { return this->dreamview_; }
  const DreamView &dreamview() const { return this->dreamview_; }
  bool request_dreamview_music(uint8_t effect, uint32_t now) {
    if (!this->connected(now) || this->dirty_ != 0 || this->sleep_dirty_ || this->sleep_may_be_active() ||
        (this->sleep_tracking_ && !this->sleep_known_))
      return false;
    return this->dreamview_.request_music_effect(effect);
  }
  bool request_dreamview(DreamView::Operation operation, uint32_t now) {
    if (!this->connected(now) ||
        (operation == DreamView::Operation::ENABLE &&
         (this->dirty_ != 0 || (this->pending_ != 0 && !this->sent_dreamview_) || this->sleep_dirty_ ||
          this->sleep_may_be_active() ||
          (this->sleep_tracking_ && !this->sleep_known_))))
      return false;
    if (!this->dreamview_.request(operation))
      return false;
    if (operation == DreamView::Operation::ENABLE) {
      this->dreamview_restore_power_allowed_ = false;
      this->music_requested_ = this->scene_requested_ = this->segments_requested_ = false;
      this->dirty_ &= ~(MUSIC | COLOR);
      this->force_color_ = false;
      this->music_cancelled_ = true;
      // Power is acknowledged before the native enable command. Remote members
      // still need their own power and a confirmed connection status.
      this->target_.power = true;
      this->clear_satisfied_();
      if (!this->actual_.power)
        this->dirty_ |= POWER;
    }
    return true;
  }
#endif
  bool native_mode_protected() const {
#ifdef USE_GOVEE_DREAMVIEW
    return this->dreamview_.protects_static();
#else
    return false;
#endif
  }
  bool clock_synced() const { return this->clock_synced_; }
  bool clock_sync_failed() const { return this->clock_failed_; }
  bool sync_clock(int hour, int minute, int second, int weekday) {
    const auto candidate = clock_packet(hour, minute, second, weekday);
    if (busy() || !this->ready_ || !clock_inventory_clear() || candidate.size == 0 || this->clock_failed_)
      return false;
    this->clock_packet_ = candidate;
    this->clock_dirty_ = true;
    this->clock_synced_ = false;
    return true;
  }
  void request_timer_inventory() {
    this->inventory_stage_ = 1;
    this->timers_known_ = this->wake_known_ = false;
  }
  int active_timer_count() const {
    if (!this->timers_known_)
      return -1;
    int count = 0;
    for (const auto &timer : this->timers_)
      count += timer.enabled;
    return count;
  }
  int wake_enabled() const { return this->wake_known_ ? int(this->wake_enabled_) : -1; }
  int wake_raw_status() const { return this->wake_known_ ? int(this->wake_raw_) : -1; }
  const NativeTimers &native_timers() const { return this->timers_; }
  bool clock_inventory_clear() const { return this->active_timer_count() == 0 && this->wake_enabled() == 0; }
  void enable_sleep_support() {
    this->sleep_tracking_ = true;
    this->sleep_known_ = false;
    this->sleep_query_dirty_ = true;
  }
  void refresh_sleep_status() {
    if (this->sleep_tracking_ && !busy())
      this->sleep_query_dirty_ = true;
  }
  bool sleep_status_known() const { return this->sleep_known_; }
  const SleepStatus &sleep_status() const { return this->sleep_status_; }
  bool sleep_may_be_active() const { return this->sleep_maybe_active_; }
  bool sleep_cancel_failed() const { return this->sleep_blocked_; }
  bool start_sleep(const SleepSettings &settings) {
    if (!this->ready_ || !this->actual_.power || !this->target_.power || !valid_sleep_settings(settings) ||
        this->sleep_blocked_ ||
        (this->sleep_tracking_ && (!this->sleep_known_ || !this->clock_synced_)))
      return false;
#ifdef USE_GOVEE_DREAMVIEW
    if (this->native_mode_protected()) {
      if (!this->dreamview_.enabled_known() || this->dreamview_.startup_pending())
        return false;
      this->end_dreamview_();
      this->sleep_waiting_for_dreamview_ = true;
    }
#endif
    this->stop_music();
    this->dirty_ &= ~(COLOR | BRIGHTNESS);
    this->force_color_ = false;
    this->sleep_settings_ = settings;
    ++this->sleep_generation_;
    this->sleep_requested_ = this->sleep_dirty_ = true;
    return true;
  }
  void cancel_sleep() {
#ifdef USE_GOVEE_DREAMVIEW
    this->sleep_waiting_for_dreamview_ = false;
#endif
    if (this->sleep_tracking_ && !this->sleep_known_) {
      this->sleep_requested_ = false;
      this->sleep_query_dirty_ = this->sleep_dirty_ = true;
      ++this->sleep_generation_;
      return;
    }
    if (this->sleep_requested_ || this->sleep_maybe_active_ || this->sleep_blocked_) {
      ++this->sleep_generation_;
      this->sleep_requested_ = false;
      this->sleep_dirty_ = true;
      this->sleep_blocked_ = false;
    }
  }
  bool connected(uint32_t now) const { return this->ready_ && uint32_t(now - this->last_status_) < LINK_TIMEOUT_MS; }
  bool failed() const { return this->failed_; }
  const State &state() const { return this->actual_; }
  bool music_requested() const { return this->music_requested_; }
  bool effect_requested() const {
    return this->music_requested_ || this->scene_requested_ || this->segments_requested_;
  }
  bool take_music_failure() {
    const bool failed = this->music_failed_;
    this->music_failed_ = false;
    return failed;
  }
  bool take_music_cancelled() {
    const bool cancelled = this->music_cancelled_;
    this->music_cancelled_ = false;
    return cancelled;
  }

  void sync_static_mode() {
    if (effect_requested() || this->native_mode_protected())
      return;
    this->force_color_ = true;
    if (this->ready_ && this->target_.power)
      this->dirty_ |= COLOR;
  }

  void start_music(uint8_t sensitivity, uint8_t mode = 0x04, const MusicSettings &settings = {}) {
    if (!valid_music_mode(mode) ||
        (settings.configured && (!supports_music_settings(mode) || (settings.calm && mode != 4))))
      return;
    if (palette_mode(mode) && music_palette(mode, this->palette_, settings).size == 0)
      return;
    this->end_dreamview_();
    this->cancel_sleep();
    this->music_settings_ = settings;
    ++this->music_generation_;
    this->music_stage_ = 0;
    this->music_mode_ = mode;
    this->music_sensitivity_ = std::min<unsigned>(sensitivity, 99);
    this->scene_requested_ = false;
    this->segments_requested_ = false;
    this->music_requested_ = true;
    this->music_failed_ = false;
    this->force_color_ = false;
    this->dirty_ = (this->dirty_ & ~COLOR) | MUSIC;
  }

  void start_scene(uint16_t scene, const uint8_t *body = nullptr, size_t size = 0) {
    if (size == 0) {
      if (scene > 255 || !valid_scene(uint8_t(scene)))
        return;
    } else if (!valid_scene_body(body, size))
      return;
    this->end_dreamview_();
    this->cancel_sleep();
    this->scene_body_size_ = size;
    this->scene_frame_ = 0;
    if (size != 0)
      std::copy(body, body + size, this->scene_body_.begin());
    ++this->music_generation_;
    this->scene_id_ = scene;
    this->scene_requested_ = true;
    this->segments_requested_ = false;
    this->music_requested_ = false;
    this->music_failed_ = false;
    this->music_stage_ = 0;
    this->force_color_ = false;
    this->dirty_ = (this->dirty_ & ~COLOR) | MUSIC;
  }

  void start_segments(const Segments &segments) {
    for (const auto &segment : segments)
      if (segment.brightness > 100)
        return;
    this->end_dreamview_();
    this->cancel_sleep();
    ++this->music_generation_;
    this->segments_ = segments;
    this->segment_index_ = 0;
    this->segments_requested_ = true;
    this->scene_requested_ = this->music_requested_ = false;
    this->music_failed_ = false;
    this->force_color_ = false;
    this->dirty_ = (this->dirty_ & ~COLOR) | MUSIC;
  }

  void set_palette(const Palette &palette) {
    this->palette_ = palette;
#ifdef USE_GOVEE_DREAMVIEW
    this->dreamview_.set_palette(palette);
#endif
    if (this->music_requested_)
      this->start_music(this->music_sensitivity_, this->music_mode_, this->music_settings_);
  }

  void stop_music() {
    if (!effect_requested())
      return;
    this->music_requested_ = false;
    this->scene_requested_ = false;
    this->segments_requested_ = false;
    this->dirty_ &= ~MUSIC;
    if (this->pending_ == 0x83
#ifdef USE_GOVEE_DREAMVIEW
        && !this->sent_dreamview_
#endif
    )
      this->pending_ = 0;      // Do not restart a music command that has already been sent.
    this->sync_static_mode();  // A static color stops the FR music mode.
  }

  void request(const State &state) {
#ifdef USE_GOVEE_DREAMVIEW
    if (this->ready_)
      this->dreamview_restore_power_allowed_ = false;
#endif
    if (!this->ready_)
      return;  // Do not restore ESPHome state before the first valid full FR status.
    if (!state.power || !same_color(state, this->target_))
      this->end_dreamview_();
    this->cancel_sleep();
    if (state.power != this->target_.power)
      this->dirty_ |= POWER;
    if (state.brightness != this->target_.brightness)
      this->dirty_ |= BRIGHTNESS;
    if (!effect_requested() && (!same_color(state, this->target_) || (this->force_color_ && state.power)))
      this->dirty_ |= COLOR;
    const State color = this->target_;
    this->target_ = state;
    if (effect_requested()) {
      this->target_.red = color.red;
      this->target_.green = color.green;
      this->target_.blue = color.blue;
      this->target_.temperature = color.temperature;
    }
    this->clear_satisfied_();
  }

  void receive(uint8_t command, const uint8_t *data, size_t length, uint32_t now) {
    // The original firmware acknowledges SetPt (0x83) through ptReal (0x63).
    // The payload consists of zero or more complete 20-byte packets.
    if (command == 0x63) {
      if (length % 20 != 0)
        return;
      for (size_t offset = 0; offset < length; offset += 20) {
        uint8_t sum = 0;
        for (size_t i = 0; i < 20; ++i)
          sum ^= data[offset + i];
        if (sum != 0)
          return;
      }
      if (this->pending_ == 0x83) {
#ifdef USE_GOVEE_DREAMVIEW
        if (this->sent_dreamview_) {
          for (size_t offset = 0; offset < length; offset += 20) {
            if (this->dreamview_.receive(data + offset, now)) {
              this->sync_dreamview_initial_state_();
              this->restore_dreamview_power_();
              this->sent_dreamview_ = false;
              this->pending_ = 0;
              this->reject_dreamview_mode_change_();
              break;
            }
          }
          return;
        }
#endif
        if (this->sent_clock_) {
          bool matched = false;
          for (size_t offset = 0; offset < length; offset += 20)
            matched |= data[offset] == 0x33 && data[offset + 1] == 0x09 && data[offset + 2] == 0;
          if (!matched)
            return;
          this->sent_clock_ = this->clock_dirty_ = false;
          this->clock_synced_ = true;
          this->pending_ = 0;
          return;
        }
        if (this->sent_inventory_ != 0) {
          bool matched = false;
          for (size_t offset = 0; offset < length; offset += 20) {
            if (this->sent_inventory_ == 1 && decode_timers(data + offset, 20, this->timers_)) {
              this->timers_known_ = true;
              matched = true;
              break;
            }
            if (this->sent_inventory_ == 2 && data[offset] == 0xAA && data[offset + 1] == 0x12 &&
                (data[offset + 2] == 0 || data[offset + 2] == 1 || data[offset + 2] == 0xFF)) {
              // FF marks the unconfigured default state (activation bit off).
              this->wake_known_ = true;
              this->wake_raw_ = data[offset + 2];
              this->wake_enabled_ = this->wake_raw_ == 1;
              matched = true;
              break;
            }
          }
          if (!matched)
            return;
          if (this->sent_inventory_ == 2)
            this->clock_failed_ = false;
          this->inventory_stage_ = this->sent_inventory_ == 1 ? 2 : 0;
          this->sent_inventory_ = 0;
          this->pending_ = 0;
          return;
        }
        if (this->sent_sleep_query_) {
          SleepStatus status;
          bool matched = false;
          for (size_t offset = 0; offset < length; offset += 20)
            if (decode_sleep_status(data + offset, 20, status)) {
              matched = true;
              break;
            }
          if (!matched)
            return;
          this->sleep_status_ = status;
          this->sleep_known_ = true;
          this->sleep_query_dirty_ = false;
          this->sent_sleep_query_ = false;
          this->pending_ = 0;
          this->sleep_maybe_active_ = status.enabled;
          if (!this->sleep_requested_)
            this->sleep_settings_ = status.settings;
          if (!status.enabled)
            this->sleep_blocked_ = false;
          const bool settings_match = status.settings.minutes == this->sleep_settings_.minutes &&
                                      status.settings.brightness == this->sleep_settings_.brightness &&
                                      status.settings.custom_color == this->sleep_settings_.custom_color &&
                                      (!this->sleep_settings_.custom_color ||
                                       (status.settings.rgb == this->sleep_settings_.rgb &&
                                        status.settings.temperature == this->sleep_settings_.temperature));
          if (this->sleep_verifying_ &&
              (status.enabled != this->sleep_requested_ || (this->sleep_requested_ && !settings_match))) {
            this->sleep_verifying_ = false;
            this->sleep_dirty_ = false;
            this->sleep_blocked_ = status.enabled;
            this->failed_ = true;
            return;
          }
          this->sleep_verifying_ = false;
          if (status.enabled && this->dirty_ != 0)
            this->cancel_sleep();
          return;
        }
        if (this->sent_sleep_) {
          bool matched = false;
          for (size_t offset = 0; offset < length; offset += 20)
            matched |= data[offset] == 0x33 && data[offset + 1] == 0x11 && data[offset + 2] == 0;
          if (!matched)
            return;  // An empty transport response does not confirm cancellation.
          this->pending_ = 0;
          this->sent_sleep_ = false;
          this->sleep_dirty_ = this->sleep_generation_ != this->sent_sleep_generation_ ||
                               this->sleep_requested_ != this->sent_sleep_enabled_;
          if (!this->sent_sleep_enabled_)
            this->sleep_maybe_active_ = false;
          if (this->sleep_tracking_) {
            this->sleep_known_ = false;
            this->sleep_query_dirty_ = true;
            this->sleep_verifying_ = this->sleep_generation_ == this->sent_sleep_generation_;
          }
          this->failed_ = false;
          return;
        }
        // A3/41 acknowledges a music palette: mode echo, then status (0 = accepted).
        // A valid XOR alone does not confirm successful acceptance.
        const bool sent_palette = this->packet_.size >= 28 && this->packet_.bytes[7] == 0xA3 &&
                                  (this->packet_.bytes[11] == 0x41 || this->packet_.bytes[11] == 0x42);
        bool foreign_reply_only = length != 0;
        bool rejected = false;
        for (size_t offset = 0; offset < length; offset += 20) {
          if (data[offset + 1] == 0x60) {
            continue;
          }
          if (data[offset] == 0xA3 && (data[offset + 1] == 0x41 || data[offset + 1] == 0x42)) {
            if (sent_palette && data[offset + 1] == this->packet_.bytes[11] &&
                data[offset + 2] == this->packet_.bytes[12]) {
              foreign_reply_only = false;
              rejected |= data[offset + 3] != 0;
            }
          } else if (data[offset] == 0xAA) {
            continue;  // A status report does not acknowledge an effect command.
          } else if (data[offset] == 0x33 &&
                     (this->packet_.bytes[7] != 0x33 || data[offset + 1] != this->packet_.bytes[8])) {
            // For example, a delayed sleep response (33 11)
            // must not acknowledge a light/music command (33 05).
            continue;
          } else {
            foreign_reply_only = false;
          }
        }
        if (foreign_reply_only)
          return;
        this->pending_ = 0;
        if (rejected && this->sent_generation_ == this->music_generation_) {
          this->dirty_ &= ~MUSIC;
          this->music_requested_ = this->scene_requested_ = this->segments_requested_ = false;
          this->music_failed_ = true;
          this->failed_ = true;
          this->force_color_ = true;
          if (this->target_.power)
            this->dirty_ |= COLOR;
          this->changed_ = this->ready_;
          return;
        }
        if (this->sent_generation_ == this->music_generation_) {
          if (this->segments_requested_ && this->segment_index_ < 6)
            ++this->segment_index_;
          else if (this->scene_requested_ && this->scene_body_size_ != 0 && this->scene_frame_ == 0)
            ++this->scene_frame_;
          else if (this->music_requested_ && music_setup_required(this->music_mode_) && this->music_stage_ == 0)
            this->music_stage_ = 1;
          else
            this->dirty_ &= ~MUSIC;
        }
        this->failed_ = false;
      }
      return;
    }
    State next = this->actual_;
    if (command == 0xC1) {
      if (length < 13 || data[2] > 1 || data[3] > 100)
        return;
      next.power = data[2] != 0;
      next.brightness = data[3];
      next.red = data[8];
      next.green = data[9];
      next.blue = data[10];
      next.temperature = (uint16_t(data[11]) << 8) | data[12];
    } else if (command == 0xC2) {
      if (length < 2 || data[1] > 100)
        return;
      next.brightness = data[1];
    } else if (command == 0xC3) {
      if (length < 6)
        return;
      next.red = data[1];
      next.green = data[2];
      next.blue = data[3];
      next.temperature = (uint16_t(data[4]) << 8) | data[5];
    } else {
      return;  // No provisioning, manufacturer resets, or firmware commands.
    }
    this->changed_ |= !(next == this->actual_) || (!this->ready_ && command == 0xC1);
    this->actual_ = next;
    if (!this->ready_ && command == 0xC1) {
      this->target_ = this->actual_;
      // A saved effect must not turn on a lamp that is actually off
      // or block publication of its state.
      if (!this->actual_.power && effect_requested()) {
        this->music_requested_ = false;
        this->scene_requested_ = false;
        this->segments_requested_ = false;
        this->dirty_ &= ~MUSIC;
        this->music_cancelled_ = true;
      }
    }
    if (command == 0xC1)
      this->ready_ = true;
    if (command == 0xC1 && !next.power) {
      this->sleep_requested_ = this->sleep_maybe_active_ = this->sleep_dirty_ = this->sleep_blocked_ = false;
      if (this->sent_sleep_ && this->pending_ == 0x83) {
        this->pending_ = 0;
        this->sent_sleep_ = false;
      }
    }
    // Partial reports before the first full status do not confirm a connection.
    if (!this->ready_)
      return;
    if (this->sleep_tracking_ && uint32_t(now - this->last_status_) >= LINK_TIMEOUT_MS) {
      this->sleep_known_ = false;
      this->sleep_query_dirty_ = true;
      this->clock_synced_ = this->clock_failed_ = false;
      this->request_timer_inventory();
    }
    this->last_status_ = now;
    if (this->pending_ == 0x82 && command == 0xC1)
      this->pending_ = 0;
    else if (this->pending_ == 0x86 && command == 0xC1 && next.power == this->sent_.power)
      this->pending_ = 0;
    else if (this->pending_ == 0x87 && (command == 0xC2 || command == 0xC1) &&
             next.brightness == this->sent_.brightness)
      this->pending_ = 0;
    else if (this->pending_ == 0x88 && (command == 0xC3 || command == 0xC1) && same_color(next, this->sent_)) {
      this->pending_ = 0;
      this->force_color_ = false;
    }
    this->clear_satisfied_();
    if (!busy()) {
      this->target_ = this->actual_;
      this->failed_ = false;
      // After “music off” while the lamp is off, restore static mode on the next
      // power-on through the physical button as well.
      if (this->force_color_ && this->target_.power && !effect_requested() && !this->native_mode_protected())
        this->dirty_ |= COLOR;
    }
  }

  bool take_update() {
    if (!this->ready_ || busy() || !this->changed_)
      return false;
    this->changed_ = false;
    return true;
  }

  template<typename Sink> void tick(uint32_t now, Sink sink) {
#ifdef USE_GOVEE_DREAMVIEW
    const bool automatic_before = this->dreamview_.automatic_configuration();
    this->dreamview_.service(now, this->connected(now),
                            !this->sleep_tracking_ || (this->sleep_known_ && !this->sleep_maybe_active_));
    if (!automatic_before && this->dreamview_.automatic_configuration()) {
      this->dreamview_previous_power_ = (this->dirty_ & POWER) ? this->target_.power : this->actual_.power;
      this->dreamview_restore_power_allowed_ = true;
    }
    this->sync_dreamview_initial_state_();
    this->restore_dreamview_power_();
    this->reject_dreamview_mode_change_();
    if (this->sleep_waiting_for_dreamview_ && !this->native_mode_protected())
      this->sleep_waiting_for_dreamview_ = false;
    if (this->sent_dreamview_ && !this->connected(now)) {
      this->pending_ = 0;
      this->sent_dreamview_ = false;
      this->dreamview_.fail();
      this->reject_dreamview_mode_change_();
    }
#endif
    if (this->pending_ != 0) {
      if (uint32_t(now - this->sent_at_) < ACK_TIMEOUT_MS)
        return;
      if (this->attempts_ < 3) {
        ++this->attempts_;
        this->sent_at_ = now;
        sink(this->packet_);
        return;
      }
#ifdef USE_GOVEE_DREAMVIEW
      if (this->sent_dreamview_) {
        this->pending_ = 0;
        this->sent_dreamview_ = false;
        this->dreamview_.fail();
        this->reject_dreamview_mode_change_();
        this->last_poll_ = now;
        return;
      }
#endif
      // Do not retry old switching commands indefinitely after three attempts.
      if (this->sent_clock_) {
        this->pending_ = 0;
        this->sent_clock_ = this->clock_dirty_ = this->clock_synced_ = false;
        this->clock_failed_ = true;
        this->failed_ = true;
        this->last_poll_ = now;
        return;
      }
      if (this->sent_inventory_ != 0) {
        this->pending_ = 0;
        this->sent_inventory_ = 0;
        this->inventory_stage_ = 0;
        this->failed_ = true;
        this->last_poll_ = now;
        return;
      }
      if (this->sent_sleep_query_) {
        this->pending_ = 0;
        this->sent_sleep_query_ = false;
        this->sleep_query_dirty_ = false;
        this->sleep_blocked_ = true;
        this->failed_ = true;
        this->last_poll_ = now;
        return;
      }
      if (this->sent_sleep_) {
        this->pending_ = 0;
        this->sent_sleep_ = false;
        this->sleep_requested_ = false;
        this->sleep_dirty_ = this->sent_sleep_enabled_;  // After an uncertain start, try cancellation once.
        this->sleep_blocked_ = !this->sent_sleep_enabled_;
        this->failed_ = true;
        this->last_poll_ = now;
        return;
      }
#ifdef USE_GOVEE_DREAMVIEW
      // In particular, a failed power-on must not be followed by native enable.
      if (this->dreamview_.operation_pending()) {
        this->dreamview_.fail();
        this->reject_dreamview_mode_change_();
      }
#endif
      const bool music_failed = this->pending_ == 0x83 || effect_requested();
      this->pending_ = 0;
      this->dirty_ = 0;
      this->target_ = this->actual_;
      if (music_failed) {
        this->music_failed_ = true;
        this->music_requested_ = false;
        this->scene_requested_ = false;
        this->segments_requested_ = false;
        this->force_color_ = true;
        if (this->target_.power)
          this->dirty_ |= COLOR;
      }
      this->changed_ = this->ready_;
      this->failed_ = true;
      this->last_poll_ = now;
      return;
    }
    uint8_t command = 0;
    if (!this->ready_ && uint32_t(now - this->last_poll_) >= POLL_INTERVAL_MS) {
      command = 0x82;
      this->last_poll_ = now;
    } else if (!this->ready_)
      return;
    // Ending native synchronization can restore the FR's previous on-state.
    // Complete that transaction before the final power-off command.
    else if (this->dirty_ & POWER && !this->target_.power
#ifdef USE_GOVEE_DREAMVIEW
             && !this->dreamview_.operation_pending()
#endif
    )
      command = 0x86;
#ifdef USE_GOVEE_DREAMVIEW
    else if (this->dreamview_.has_packet() && this->connected(now))
      command = (this->dirty_ & POWER) && this->target_.power ? 0x86 : 0x83;
#endif
    else if (this->sleep_query_dirty_)
      command = 0x83;
    else if (this->inventory_stage_ != 0)
      command = 0x83;
    else if (this->clock_dirty_)
      command = 0x83;
    else if (this->sleep_dirty_
#ifdef USE_GOVEE_DREAMVIEW
             && !this->sleep_waiting_for_dreamview_
#endif
    )
      command = 0x83;
    else if (this->sleep_blocked_) {
      if (uint32_t(now - this->last_poll_) < POLL_INTERVAL_MS)
        return;
      // Pending light commands keep busy() true. After the full status, also reread
      // the sleep status without interrupting the connection check.
      if (this->sleep_tracking_)
        this->sleep_query_dirty_ = true;
      command = 0x82;
      this->last_poll_ = now;
    } else if (this->native_mode_protected() && (this->dirty_ & (COLOR | MUSIC))) {
      // A failed disable must not be bypassed by a queued mode/color command.
      if (uint32_t(now - this->last_poll_) < POLL_INTERVAL_MS)
        return;
      command = 0x82;
      this->last_poll_ = now;
    } else if (this->dirty_ & COLOR)
      command = 0x88;
    else if (this->dirty_ & BRIGHTNESS)
      command = 0x87;
    else if (this->dirty_ & POWER)
      command = 0x86;
    else if ((this->dirty_ & MUSIC) && this->target_.power)
      command = 0x83;
    else if (uint32_t(now - this->last_poll_) >= POLL_INTERVAL_MS) {
      command = 0x82;
      this->last_poll_ = now;
    }
    if (command == 0)
      return;
    this->sent_ = this->target_;
    uint8_t data[5]{};
    size_t length = 0;
    if (command == 0x86) {
      data[0] = this->sent_.power;
      length = 1;
    }
    if (command == 0x87) {
      data[0] = this->sent_.brightness;
      length = 1;
    }
    if (command == 0x88) {
      data[0] = this->sent_.red;
      data[1] = this->sent_.green;
      data[2] = this->sent_.blue;
      data[3] = this->sent_.temperature >> 8;
      data[4] = this->sent_.temperature & 0xFF;
      length = 5;
    }
    this->sent_sleep_ = false;
    this->sent_sleep_query_ = false;
    this->sent_inventory_ = 0;
    this->sent_clock_ = false;
    if (command == 0x83) {
#ifdef USE_GOVEE_DREAMVIEW
      if (this->dreamview_.has_packet() && this->connected(now)) {
        this->sent_dreamview_ = true;
        this->packet_ = this->dreamview_.packet(now);
      } else
#endif
      if (this->sleep_query_dirty_) {
        this->sent_sleep_query_ = true;
        this->packet_ = sleep_query_packet();
      } else if (this->inventory_stage_ != 0) {
        this->sent_inventory_ = this->inventory_stage_;
        this->packet_ = this->sent_inventory_ == 1 ? timers_query_packet() : wake_query_packet();
      } else if (this->clock_dirty_) {
        this->sent_clock_ = true;
        this->packet_ = this->clock_packet_;
      } else if (this->sleep_dirty_) {
        this->sent_sleep_ = true;
        this->sent_sleep_enabled_ = this->sleep_requested_;
        this->sent_sleep_generation_ = this->sleep_generation_;
        if (this->sent_sleep_enabled_)
          this->sleep_maybe_active_ = true;
        this->packet_ = sleep_packet(this->sent_sleep_enabled_, this->sleep_settings_);
      } else {
        this->sent_sleep_ = false;
        this->sent_sensitivity_ = this->music_sensitivity_;
        this->sent_mode_ = this->music_mode_;
        this->sent_generation_ = this->music_generation_;
        if (this->segments_requested_) {
          this->packet_ = segment_packet(this->segment_index_, this->segments_[this->segment_index_]);
        } else if (this->scene_requested_) {
          if (this->scene_body_size_ != 0 && this->scene_frame_ == 0)
            this->packet_ = scene_upload(this->scene_body_.data(), this->scene_body_size_);
          else
            this->packet_ = scene_packet(this->scene_id_, this->scene_body_size_ != 0);
        } else if (music_setup_required(this->sent_mode_) && this->music_stage_ == 0)
          // According to the FR handler, mode 0 sets a valid default palette.
          this->packet_ = initialize_music_palette(this->sent_mode_)
                              ? music_packet(0, this->sent_sensitivity_)
                              : music_palette(this->sent_mode_, this->palette_, this->music_settings_);
        else
          this->packet_ = music_packet(this->sent_mode_, this->sent_sensitivity_, this->music_settings_);
      }
    } else {
      this->packet_ = encode(command, data, length);
    }
    this->pending_ = command;
    this->sent_at_ = now;
    this->attempts_ = 1;
    sink(this->packet_);
  }

 private:
  enum : uint8_t { POWER = 1, BRIGHTNESS = 2, COLOR = 4, MUSIC = 8 };
  void end_dreamview_() {
#ifdef USE_GOVEE_DREAMVIEW
    if (this->dreamview_.protects_static())
      this->dreamview_.request(DreamView::Operation::DISABLE);
#endif
  }
#ifdef USE_GOVEE_DREAMVIEW
  void sync_dreamview_initial_state_() {
    if (this->dreamview_initial_read_ || this->dreamview_.startup_pending() || !this->dreamview_.enabled_known())
      return;
    this->dreamview_initial_read_ = true;
    if (this->dreamview_.enabled() && !this->dreamview_.operation_pending()) {
      this->music_cancelled_ = this->effect_requested();
      this->music_requested_ = this->scene_requested_ = this->segments_requested_ = false;
      this->dirty_ &= ~(COLOR | MUSIC);
      this->force_color_ = false;
    }
  }
  void restore_dreamview_power_() {
    if (this->dreamview_.take_power_restore() && this->dreamview_restore_power_allowed_) {
      this->target_.power = this->dreamview_previous_power_;
      // Uploading membership can switch on the FR. Send and acknowledge the
      // original power state even if its unsolicited report is still in flight.
      this->dirty_ |= POWER;
    }
  }
  void reject_dreamview_mode_change_() {
    if (!this->dreamview_.take_command_failure())
      return;
    if (this->sleep_waiting_for_dreamview_) {
      // A failed or unconfirmed disable must never release a queued sleep start.
      this->sleep_waiting_for_dreamview_ = false;
      this->sleep_requested_ = this->sleep_dirty_ = false;
      ++this->sleep_generation_;
    }
    if ((this->dirty_ & (COLOR | MUSIC)) == 0)
      return;
    // Reject the unsent mode change after a failed native disable. Restoring the
    // confirmed color also makes a later request for the same color retryable.
    this->dirty_ &= ~(COLOR | MUSIC);
    this->target_.red = this->actual_.red;
    this->target_.green = this->actual_.green;
    this->target_.blue = this->actual_.blue;
    this->target_.temperature = this->actual_.temperature;
    this->force_color_ = false;
    this->music_requested_ = this->scene_requested_ = this->segments_requested_ = false;
    this->music_cancelled_ = true;
    this->changed_ = this->ready_;
  }
#endif
  void clear_satisfied_() {
    // A counter-command must be retained while the previous command is still in flight.
    if (this->pending_ != 0x86 && this->target_.power == this->actual_.power)
      this->dirty_ &= ~POWER;
    if (this->pending_ != 0x87 && this->target_.brightness == this->actual_.brightness)
      this->dirty_ &= ~BRIGHTNESS;
    if (this->pending_ != 0x88 && same_color(this->target_, this->actual_) &&
        (!this->force_color_ || !this->target_.power))
      this->dirty_ &= ~COLOR;
  }
#ifdef USE_GOVEE_DREAMVIEW
  DreamView dreamview_;
  bool sent_dreamview_{false}, dreamview_initial_read_{false};
  bool dreamview_previous_power_{false}, dreamview_restore_power_allowed_{false};
  bool sleep_waiting_for_dreamview_{false};
#endif
  MusicSettings music_settings_{};
  SleepSettings sleep_settings_{};
  NativeTimers timers_{};
  Packet clock_packet_{};
  bool clock_dirty_{false}, sent_clock_{false}, clock_synced_{false}, clock_failed_{false};
  bool timers_known_{false}, wake_known_{false}, wake_enabled_{false};
  uint8_t wake_raw_{0};
  uint8_t inventory_stage_{0}, sent_inventory_{0};
  SleepStatus sleep_status_{};
  bool sleep_tracking_{false}, sleep_known_{false}, sleep_query_dirty_{false}, sent_sleep_query_{false},
      sleep_verifying_{false};
  uint32_t sleep_generation_{0}, sent_sleep_generation_{0};
  bool sleep_requested_{false}, sleep_maybe_active_{false}, sleep_dirty_{false}, sleep_blocked_{false};
  bool sent_sleep_{false}, sent_sleep_enabled_{false};
  Palette palette_{RAINBOW};
  uint32_t music_generation_{0}, sent_generation_{0};
  uint8_t music_stage_{0};
  State actual_, target_, sent_;
  Packet packet_;
  uint32_t last_poll_{0}, sent_at_{0}, last_status_{0};
  uint8_t dirty_{0}, pending_{0}, attempts_{0};
  uint8_t music_sensitivity_{99}, sent_sensitivity_{99};
  uint8_t music_mode_{0x04}, sent_mode_{0x04};
  bool ready_{false}, changed_{false}, failed_{false};
  uint16_t scene_id_{0};
  std::array<uint8_t, MAX_SCENE_BODY> scene_body_{};
  size_t scene_body_size_{0}, scene_frame_{0};
  Segments segments_{};
  size_t segment_index_{0};
  bool segments_requested_{false};
  bool scene_requested_{false};
  bool music_requested_{false}, force_color_{false};
  bool music_failed_{false}, music_cancelled_{false};
};

}  // namespace esphome::govee_h6076::protocol
