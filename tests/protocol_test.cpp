#include "../components/govee_h6076/protocol.h"

// ESPHome includes every component header, even when optional entities are absent.
#ifndef USE_GOVEE_DREAMVIEW_MUSIC
#include "../components/govee_h6076/dreamview_select.h"
#endif
#ifndef USE_GOVEE_DREAMVIEW
#include "../components/govee_h6076/dreamview_switch.h"
#endif

#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using namespace esphome::govee_h6076::protocol;

static unsigned checks = 0;
#define CHECK(expression) \
  do { \
    ++checks; \
    if (!(expression)) { \
      std::cerr << "FAIL line " << __LINE__ << ": " #expression "\n"; \
      std::exit(1); \
    } \
  } while (false)

// Expected packets come from an independent analysis of the original firmware.
static const std::vector<uint8_t> ON{0x55, 1, 1, 0, 0, 0, 0x86, 1, 0xDE};
static const std::vector<uint8_t> OFF{0x55, 1, 1, 0, 0, 0, 0x86, 0, 0xDD};
static const std::vector<uint8_t> HALF{0x55, 1, 1, 0, 0, 0, 0x87, 0x32, 0x10};
static const std::vector<uint8_t> RED{0x55, 1, 5, 0, 0, 0, 0x88, 0xFF, 0, 0, 0, 0, 0xE2};
static const std::vector<uint8_t> QUERY{0x55, 1, 0, 0, 0, 0, 0x82, 0xD8};
// Controlled state data: only the first 13 non-sensitive bytes of the boot status.
static const std::vector<uint8_t> BOOT{0x0D, 0, 0, 100, 0, 2, 1, 5, 255, 112, 45, 0, 0};

static std::vector<uint8_t> bytes(const Packet &p) { return {p.bytes.begin(), p.bytes.begin() + p.size}; }

// Separate fixture encoder with a full 32-bit length, including for invalid frames.
static std::vector<uint8_t> incoming(uint8_t command, const std::vector<uint8_t> &payload) {
  const uint32_t n = payload.size();
  std::vector<uint8_t> frame{0x55, 1, uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24), command};
  frame.insert(frame.end(), payload.begin(), payload.end());
  unsigned sum = 0;
  for (uint8_t byte : frame)
    sum += byte;
  frame.push_back(sum & 255);
  return frame;
}

struct Harness {
  Controller controller;
  Parser parser;
  std::vector<std::vector<uint8_t>> sent;
  uint32_t now{0};
  Harness() { controller.begin(now); }
  void tick(uint32_t advance = 0) {
    now += advance;
    controller.tick(now, [this](const Packet &p) { sent.push_back(bytes(p)); });
  }
  void receive(uint8_t command, const std::vector<uint8_t> &payload) {
    for (uint8_t byte : incoming(command, payload))
      parser.feed(byte, now,
                  [this](uint8_t cmd, const uint8_t *data, size_t n) { controller.receive(cmd, data, n, now); });
  }
  void boot() {
    tick(500);
    CHECK(sent.size() == 1 && sent.back() == QUERY);
    receive(0xC1, BOOT);
    CHECK(controller.ready());
    CHECK(controller.take_update());
    CHECK(!controller.take_update());
    sent.clear();
  }
  std::vector<uint8_t> boot_on() {
    this->boot();
    auto status = BOOT;
    status[2] = 1;
    this->receive(0xC1, status);
    return status;
  }
};

static void encoders() {
  const uint8_t on[]{1}, off[]{0}, half[]{50}, red[]{255, 0, 0, 0, 0};
  CHECK(bytes(encode(0x86, on, 1)) == ON);
  CHECK(bytes(encode(0x86, off, 1)) == OFF);
  CHECK(bytes(encode(0x87, half, 1)) == HALF);
  CHECK(bytes(encode(0x88, red, 5)) == RED);
  CHECK(bytes(encode(0x82)) == QUERY);
  CHECK(encode(0x88, nullptr, 5).size == 0);
  CHECK(encode(0x88, red, 241).size == 0);
}

static void music_encoding_and_lifecycle() {
  // Independently calculated from scroom/ha-govee-local-segments at a pinned source revision:
  // inner XOR 0x5F, outer additive checksum 0xFB.
  const std::vector<uint8_t> expected{0x55, 1, 20, 0, 0, 0, 0x83, 0x33, 5, 15, 4, 99, 1,    0,
                                      0,    0, 0,  0, 0, 0, 0,    0,    0, 0,  0, 0,  0x5F, 0xFB};
  CHECK(bytes(music_packet(0x04, 99)) == expected);
  CHECK(bytes(music_packet(0x04, 255)) == expected);
  const auto zero = bytes(music_packet(0x04, 0));
  CHECK(zero[11] == 0 && zero[26] == 0x3C && zero[27] == 0x75);
  Harness h;
  h.boot();
  auto target = h.controller.state();
  target.power = true;
  target.brightness = 50;
  h.controller.start_music(99);
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back() == HALF);  // Set brightness and turn on before the music command.
  h.receive(0xC2, {0, 50});
  h.tick();
  CHECK(h.sent.back() == ON);
  auto status = BOOT;
  status[2] = 1;
  status[3] = 50;
  h.receive(0xC1, status);
  h.tick();
  CHECK(h.sent.back() == expected && h.controller.busy());
  h.receive(0xC1, status);
  CHECK(h.controller.busy());  // A full status does not acknowledge a SetPt command.
  auto bad = std::vector<uint8_t>(expected.begin() + 7, expected.end() - 1);
  bad[19] ^= 1;
  h.receive(0x63, bad);
  CHECK(h.controller.busy());
  h.receive(0x63, {0});
  CHECK(h.controller.busy());
  h.receive(0x63, {expected.begin() + 7, expected.end() - 1});
  CHECK(!h.controller.busy() && h.controller.music_requested());
  const size_t count = h.sent.size();
  h.tick(100);
  CHECK(h.sent.size() == count);  // No continuous audio transmission.
  target = h.controller.state();
  target.red = 0;
  target.green = 255;
  target.blue = 0;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.size() == count);
  h.controller.stop_music();
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);  // The same static color also stops music.
  CHECK(!h.controller.music_requested());
  h.receive(0xC3, {0, 255, 112, 45, 0, 0});
  CHECK(!h.controller.busy() && !h.controller.take_music_failure());
}

static void music_off_and_failure() {
  Harness h;
  h.boot();
  auto target = h.controller.state();
  target.power = true;
  h.controller.request(target);
  h.tick();
  auto status = BOOT;
  status[2] = 1;
  h.receive(0xC1, status);
  h.controller.start_music(99);
  h.tick();
  CHECK(h.sent.back()[6] == 0x83);
  // None/off before the response must not trigger a later restart.
  h.controller.stop_music();
  target = h.controller.state();
  target.power = false;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back() == OFF);
  h.receive(0xC1, BOOT);
  CHECK(!h.controller.busy());
  h.receive(0x63, {});
  h.tick(500);
  CHECK(h.sent.back() == OFF);
  target = h.controller.state();
  target.power = true;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);  // Restore static mode on the next power-on.
  h.receive(0xC3, {0, 255, 112, 45, 0, 0});
  h.tick();
  CHECK(h.sent.back() == ON);
  h.receive(0xC1, status);
  h.controller.start_music(99);
  h.tick();
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(!h.controller.music_requested() && h.controller.take_music_failure());
  CHECK(!h.controller.take_music_failure());
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);
  h.receive(0xC3, {0, 255, 112, 45, 0, 0});
  CHECK(!h.controller.busy());
  h.controller.start_music(99);
  h.tick();
  h.receive(0x63, {});
  h.controller.stop_music();
  target = h.controller.state();
  target.power = false;
  h.controller.request(target);
  h.tick();
  h.receive(0xC1, BOOT);
  CHECK(!h.controller.busy());
  h.receive(0xC1, status);
  h.tick();  // Physical button; no Home Assistant power-on command.
  CHECK(h.sent.back()[6] == 0x88);
  h.receive(0xC3, {0, 255, 112, 45, 0, 0});
  CHECK(!h.controller.busy());
  // Music requested while the FR state is still unknown waits for the first status query.
  Harness early;
  early.controller.start_music(99);
  early.tick();
  CHECK(early.sent.empty());
  early.tick(500);
  CHECK(early.sent.back() == QUERY);
  early.receive(0xC1, status);
  early.tick();
  CHECK(early.sent.back()[6] == 0x83);
}

static void music_modes_and_live_sensitivity() {
  const std::vector<uint8_t> beat{0x55, 1, 20, 0, 0, 0, 0x83, 0x33, 5, 15, 6, 50, 0,    0,
                                  0,    0, 0,  0, 0, 0, 0,    0,    0, 0,  0, 0,  0x0D, 0x79};
  const std::vector<uint8_t> energetic{0x55, 1, 20, 0, 0, 0, 0x83, 0x33, 5, 15, 0, 50, 1,    255,
                                       0,    0, 0,  0, 0, 0, 0,    0,    0, 0,  0, 0,  0xF5, 0x5B};
  CHECK(bytes(music_packet(6, 50)) == beat);
  CHECK(bytes(music_packet(0, 50)) == energetic);
  CHECK(music_packet(254, 50).size == 0);
  Harness h;
  h.boot_on();
  h.controller.start_music(50, 4);
  h.tick();
  h.controller.start_music(50, 6);  // New mode while a response is still pending.
  h.receive(0x63, {});
  CHECK(h.controller.busy());
  h.tick();
  CHECK(h.sent.back() == beat);
  h.receive(0x63, {});
  CHECK(!h.controller.busy());
  h.controller.start_music(80, 6);
  h.tick();
  CHECK(h.sent.back()[10] == 6 && h.sent.back()[11] == 80);
  h.receive(0x63, {});
  CHECK(!h.controller.busy());
  h.controller.start_music(50, 0);
  h.tick();
  CHECK(h.sent.back() == energetic);
  h.receive(0x63, {});
  CHECK(!h.controller.busy());
}

static void restart_mode_sync_preserves_power() {
  auto on = BOOT;
  on[2] = 1;
  Harness restored_on;
  restored_on.controller.start_music(99, 0);
  restored_on.tick(500);
  CHECK(restored_on.sent.back() == QUERY);
  restored_on.receive(0xC1, on);
  restored_on.controller.sync_static_mode();
  restored_on.tick();
  CHECK(restored_on.sent.size() == 2 && restored_on.sent.back() == bytes(music_packet(0, 99)));
  restored_on.receive(0x63, {});
  CHECK(restored_on.controller.take_update() && restored_on.controller.state().power);
  CHECK(!restored_on.controller.take_music_cancelled());

  Harness restored_off;
  restored_off.controller.start_music(99, 0);
  restored_off.tick(500);
  restored_off.receive(0xC1, BOOT);
  CHECK(restored_off.controller.take_music_cancelled());
  CHECK(!restored_off.controller.take_music_cancelled());
  CHECK(!restored_off.controller.music_requested() && !restored_off.controller.busy());
  CHECK(restored_off.controller.take_update() && !restored_off.controller.state().power);
  CHECK(!restored_off.controller.take_music_failure());
  restored_off.controller.sync_static_mode();
  restored_off.tick();
  CHECK(restored_off.sent.size() == 1 && restored_off.sent.back() == QUERY);
  restored_off.receive(0xC1, on);
  restored_off.tick();  // Next physical button press.
  CHECK(restored_off.sent.size() == 2 && restored_off.sent.back()[6] == 0x88);
  restored_off.receive(0xC3, {0, 255, 112, 45, 0, 0});
  restored_off.tick();
  CHECK(!restored_off.controller.busy() && restored_off.sent.size() == 2);

  Harness manual;
  manual.tick(500);
  manual.receive(0xC1, on);
  manual.controller.sync_static_mode();
  manual.tick();
  const std::vector<uint8_t> original_color{0x55, 1, 5, 0, 0, 0, 0x88, 255, 112, 45, 0, 0, 127};
  CHECK(manual.sent.size() == 2 && manual.sent.back() == original_color);
  manual.receive(0xC3, {0, 255, 112, 45, 0, 0});
  manual.tick();
  CHECK(manual.sent.size() == 2 && manual.controller.take_update());
  CHECK(manual.controller.state().power && !manual.controller.busy());
}

static void parser_faults() {
  Parser parser;
  unsigned received = 0;
  auto callback = [&](uint8_t cmd, const uint8_t *, size_t n) {
    CHECK(cmd == 0x82 && n == 0);
    ++received;
  };
  const std::vector<uint8_t> garbage{0x00, 0xFF, 0x55, 0x55, 0xFE, 0xAA};
  for (auto b : garbage)
    parser.feed(b, 10, callback);
  for (size_t i = 0; i < QUERY.size(); ++i) {
    parser.feed(QUERY[i], 20 + i, callback);
    CHECK(received == (i + 1 == QUERY.size() ? 1 : 0));
  }
  auto broken = QUERY;
  broken.back() ^= 1;
  for (auto b : broken)
    parser.feed(b, 50, callback);
  for (auto b : QUERY)
    parser.feed(b, 51, callback);
  CHECK(received == 2);
  // Length 0xFFFFFFFF must trigger neither an allocation nor a buffer overflow.
  for (auto b : std::vector<uint8_t>{0x55, 1, 255, 255, 255, 255, 0xC1})
    parser.feed(b, 60, callback);
  for (auto b : QUERY)
    parser.feed(b, 61, callback);
  CHECK(received == 3);
  // An incomplete packet is discarded after 100 ms; reception can then continue.
  for (auto b : std::vector<uint8_t>{0x55, 1, 100, 0, 0, 0, 0xC1, 0x55})
    parser.feed(b, 70, callback);
  for (auto b : QUERY)
    parser.feed(b, 170, callback);
  CHECK(received == 4);
  for (auto b : QUERY)
    parser.feed(b, 171, callback);
  CHECK(received == 5);
  // An embedded 55 01 inside a valid payload remains payload.
  unsigned full = 0;
  std::vector<uint8_t> maximum(MAX_PAYLOAD, 0x55);
  maximum[500] = 1;
  for (auto b : incoming(0xC1, maximum))
    parser.feed(b, 180, [&](uint8_t c, const uint8_t *d, size_t n) {
      CHECK(c == 0xC1 && n == MAX_PAYLOAD && d[500] == 1);
      ++full;
    });
  CHECK(full == 1);
  // Deterministic noise data under AddressSanitizer/UndefinedBehaviorSanitizer.
  std::mt19937 noise(6076);
  unsigned unexpected = 0;
  for (uint32_t i = 0; i < 100000; ++i)
    parser.feed(noise() & 255, 200 + i / 16, [&](uint8_t, const uint8_t *, size_t) { ++unexpected; });
  CHECK(unexpected == 0);
  for (auto b : QUERY)
    parser.feed(b, 10000, callback);
  CHECK(received == 6);
}

static void startup_and_button() {
  Harness h;
  State restore;
  restore.power = true;
  h.controller.request(restore);
  h.tick();
  CHECK(h.sent.empty());
  h.receive(0xC2, {0, 40});
  CHECK(!h.controller.ready());
  h.boot();
  const State status = h.controller.state();
  CHECK(!status.power && status.brightness == 100 && status.red == 255 && status.green == 112 && status.blue == 45);
  auto button = BOOT;
  button[2] = 1;
  h.receive(0xC1, button);
  CHECK(h.controller.state().power && h.controller.take_update());
  h.receive(0xC2, {0, 50});
  CHECK(h.controller.state().brightness == 50 && h.controller.take_update());
  h.controller.request(h.controller.state());
  h.tick();
  CHECK(h.sent.empty());  // A response does not create a command loop.
  h.receive(0xC2, {0});
  h.receive(0xC2, {0, 101});
  h.receive(0xC1, {0});
  h.receive(0xC3, {0, 1, 2});
  h.receive(0xC6, {});         // Manufacturer reset is ignored.
  h.receive(0xC4, {1, 2, 3});  // Provisioning is ignored.
  CHECK(h.controller.state().brightness == 50 && !h.controller.take_update());
  h.tick(POLL_INTERVAL_MS);
  CHECK(h.sent.size() == 1 && h.sent.back() == QUERY);
  button[3] = 20;
  h.receive(0xC1, button);
  CHECK(h.controller.take_update() && h.controller.state().brightness == 20);
  CHECK(h.controller.connected(h.now));
  CHECK(!h.controller.connected(h.now + LINK_TIMEOUT_MS));
}

static void commands_and_ack() {
  Harness h;
  h.boot();
  auto target = h.controller.state();
  target.power = true;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back() == ON);
  h.receive(0xC2, {0, 100});
  CHECK(h.controller.busy());  // An unrelated response does not acknowledge a switching command.
  h.receive(0xC1, BOOT);
  CHECK(h.controller.busy());  // An old full status does not acknowledge power-on either.
  h.tick(ACK_TIMEOUT_MS);
  CHECK(h.sent.size() == 2 && h.sent.back() == ON);
  auto status = BOOT;
  status[2] = 1;
  h.receive(0xC1, status);
  CHECK(!h.controller.busy() && h.controller.take_update());
  target = h.controller.state();
  target.brightness = 50;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back() == HALF);
  h.receive(0xC2, {0, 50});
  CHECK(!h.controller.busy() && h.controller.take_update());
  target = h.controller.state();
  target.red = 255;
  target.green = 0;
  target.blue = 0;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back() == RED);
  h.receive(0xC3, {0, 255, 0, 0, 0, 0});
  CHECK(!h.controller.busy());
  target = h.controller.state();
  target.red = target.green = target.blue = 0;
  target.temperature = 2200;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back()[10] == 0x08 && h.sent.back()[11] == 0x98);
  h.receive(0xC3, {0, 0, 0, 0, 0x08, 0x98});
  CHECK(h.controller.state().temperature == 2200 && !h.controller.busy());
}

static void coalescing_and_cancel() {
  Harness h;
  h.boot();
  auto target = h.controller.state();
  target.power = true;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back() == ON);
  target.power = false;
  h.controller.request(target);
  auto status = BOOT;
  status[2] = 1;
  h.receive(0xC1, status);
  CHECK(h.controller.busy());
  h.tick();
  CHECK(h.sent.back() == OFF);  // Switch to the opposite state while power-on is in flight.
  h.receive(0xC1, BOOT);
  CHECK(!h.controller.busy());
  target = h.controller.state();
  target.brightness = 10;
  h.controller.request(target);
  h.tick();
  target.brightness = 20;
  h.controller.request(target);
  target.brightness = 50;
  h.controller.request(target);
  h.receive(0xC2, {0, 10});
  h.tick();
  CHECK(h.sent.back() == HALF);  // Intermediate value 20 is not sent.
  h.receive(0xC2, {0, 50});
  CHECK(!h.controller.busy());
  // Set color and brightness before turning on a dim state.
  target = h.controller.state();
  target.power = true;
  target.red = 255;
  target.green = target.blue = 0;
  target.brightness = 30;
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.back() == RED);
  h.receive(0xC3, {0, 255, 0, 0, 0, 0});
  h.tick();
  CHECK(h.sent.back()[6] == 0x87 && h.sent.back()[7] == 30);
  h.receive(0xC2, {0, 30});
  h.tick();
  CHECK(h.sent.back() == ON);
}

static void timeouts_and_clock_wrap() {
  Harness h;
  h.boot();
  auto target = h.controller.state();
  target.power = true;
  h.controller.request(target);
  h.tick();
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(h.sent.size() == 3 && h.controller.failed() && !h.controller.busy());
  CHECK(!h.controller.state().power);
  h.tick(1000);
  CHECK(h.sent.size() == 3);
  h.tick(4000);
  CHECK(h.sent.size() == 4 && h.sent.back() == QUERY);
  h.receive(0xC1, BOOT);
  CHECK(!h.controller.failed());
  Harness wrap;
  wrap.now = UINT32_MAX - 250;
  wrap.controller.begin(wrap.now);
  wrap.tick(499);
  CHECK(wrap.sent.empty());
  wrap.tick(1);
  CHECK(wrap.sent.size() == 1 && wrap.sent.back() == QUERY);
  wrap.receive(0xC1, BOOT);
  CHECK(wrap.controller.connected(wrap.now));
}

static void palette_transaction() {
  CHECK(music_palette(0x5A, RAINBOW).bytes[38] ==
        6);  // Visible rendered value instead of an ineffective initial value.
  for (uint8_t mode :
       {0x00, 0x04, 0x06, 0x08, 0x02, 0x07, 0x4D, 0x50, 0x4E, 0x4F, 0x51, 0x32, 0x16, 0x8F, 0x8E, 0x5A, 0x71}) {
    CHECK(music_packet(mode, 99).size == 28);
  }
  const auto p = music_palette(0x32, RAINBOW);
  CHECK(p.size == 48 && p.bytes[2] == 40 && p.bytes[7] == 0xA3 && p.bytes[28] == 0xFF);
  CHECK(p.bytes[38] == 2 && p.bytes[39] == 0 && p.bytes[40] == 0x61);
  Harness h;
  h.boot_on();
  h.controller.start_music(99, 0x32);
  h.tick();
  CHECK(h.sent.back() == bytes(p));
  h.tick(500);
  CHECK(h.sent.back() == bytes(p));  // The repeated command uses exactly the same palette.
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(music_packet(0x32, 99)));
  h.receive(0x63, {});
  CHECK(!h.controller.busy());
  h.controller.start_music(99, 0x4D);
  h.tick();
  h.controller.start_music(70, 0x50);
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(music_palette(0x50, RAINBOW)));
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(music_packet(0x50, 70)));
  h.controller.stop_music();
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);
}

static void scenes_and_mode_switching() {
  const std::vector<uint8_t> rainbow{0x55, 1, 20, 0, 0, 0, 0x83, 0x33, 5, 4, 22, 0, 0,    0,
                                     0,    0, 0,  0, 0, 0, 0,    0,    0, 0, 0,  0, 0x24, 0x63};
  CHECK(bytes(scene_packet(22)) == rainbow);
  CHECK(scene_packet(255).size == 0);
  Harness h;
  h.boot_on();
  h.controller.start_music(99, 0x4D);
  h.tick();
  h.controller.start_scene(22);
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == rainbow);
  h.receive(0x63, {});
  CHECK(!h.controller.busy() && h.controller.effect_requested() && !h.controller.music_requested());
  auto target = h.controller.state();
  target.red = 0;
  target.blue = 255;
  auto count = h.sent.size();
  h.controller.request(target);
  h.tick();
  CHECK(h.sent.size() == count);
  h.controller.start_music(99, 0x4D);
  h.tick();
  CHECK(h.sent.back() == bytes(music_palette(0x4D, RAINBOW)));
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(music_packet(0x4D, 99)));
  h.receive(0x63, {});
  h.controller.start_scene(22);
  h.tick();
  h.receive(0x63, {});
  h.controller.stop_music();
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);
  h.receive(0xC3, {0, 255, 112, 45, 0, 0});
  CHECK(!h.controller.effect_requested() && !h.controller.busy());
  h.controller.start_scene(22);
  h.tick();
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(!h.controller.effect_requested() && h.controller.take_music_failure());
  Harness off;
  off.controller.start_scene(22);
  off.tick(500);
  off.receive(0xC1, BOOT);
  CHECK(off.controller.take_music_cancelled() && !off.controller.effect_requested());
  CHECK(off.controller.take_update() && !off.controller.state().power);
}

static void scene_upload_lifecycle() {
  const uint8_t body[]{1, 3, 10, 20, 30};
  CHECK(valid_scene_body(body, sizeof(body)));
  CHECK(!valid_scene_body(nullptr, 5));
  CHECK(!valid_scene_body(body, 4));
  const uint8_t malformed[]{2, 1, 4};
  CHECK(!valid_scene_body(malformed, sizeof(malformed)));
  CHECK(scene_upload_packet(body, sizeof(body), 2).size == 0);
  auto first = scene_upload_packet(body, sizeof(body), 0);
  CHECK(first.bytes[7] == 0xA3 && first.bytes[8] == 0 && first.bytes[9] == 1 && first.bytes[10] == 2 &&
        first.bytes[11] == 2);
  CHECK(scene_upload_packet(body, sizeof(body), 1).bytes[8] == 0xFF);
  CHECK(scene_packet(2143, true).bytes[10] == 0x5F && scene_packet(2143, true).bytes[11] == 8);
  Harness h;
  h.boot_on();
  h.controller.start_scene(2143, body, sizeof(body));
  h.tick();
  CHECK(h.sent.back() == bytes(scene_upload(body, sizeof(body))));
  h.tick(500);
  CHECK(h.sent.back() == bytes(scene_upload(body, sizeof(body))));
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(scene_packet(2143, true)));
  h.receive(0x63, {});
  CHECK(!h.controller.busy());
  h.controller.start_scene(2143, body, sizeof(body));
  h.tick();
  h.controller.start_scene(22);
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(scene_packet(22)));
  h.receive(0x63, {});
  h.controller.start_scene(2143, body, sizeof(body));
  h.tick();
  h.controller.stop_music();
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);
  std::array<uint8_t, 196> largest{};
  largest[0] = 1;
  largest[1] = 194;
  CHECK(valid_scene_body(largest.data(), largest.size()) && scene_frame_count(largest.size()) == 12);
  std::vector<uint8_t> joined;
  for (size_t j = 0; j < 12; ++j) {
    auto frame = scene_upload_packet(largest.data(), largest.size(), j);
    uint8_t x = 0;
    for (size_t k = 7; k < 27; ++k)
      x ^= frame.bytes[k];
    CHECK(x == 0);
    joined.insert(joined.end(), frame.bytes.begin() + 9, frame.bytes.begin() + 26);
  }
  CHECK(joined[0] == 1 && joined[1] == 12 && joined[2] == 2);
  CHECK(std::equal(largest.begin(), largest.end(), joined.begin() + 3));
}

static void segment_encoders() {
  const std::vector<uint8_t> red_bottom{0x33, 5, 0x15, 1, 255, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xDC};
  const auto color = segment_color(1, 255, 0, 0);
  CHECK(std::equal(red_bottom.begin(), red_bottom.end(), color.bytes.begin() + 7));
  CHECK(segment_color(0, 1, 2, 3).size == 0);
  CHECK(segment_color(128, 1, 2, 3).size == 0);
}

static void segments_lifecycle() {
  Segments segments{};
  for (size_t i = 0; i < 7; ++i)
    segments[i] = {uint8_t(i * 30), uint8_t(255 - i * 30), 0, uint8_t(i * 15)};
  CHECK(segment_packet(7, segments[0]).size == 0);
  CHECK(bytes(segment_packet(0, {255, 255, 255, 0})) == bytes(segment_color(1, 0, 0, 0)));
  CHECK(bytes(segment_packet(3, {200, 100, 50, 50})) == bytes(segment_color(8, 100, 50, 25)));
  CHECK(bytes(segment_packet(6, {255, 0, 0, 100})) == bytes(segment_color(64, 255, 0, 0)));
  Harness h;
  auto on = h.boot_on();
  h.controller.start_segments(segments);
  for (size_t i = 0; i < 7; ++i) {
    const auto expected = segment_packet(i, segments[i]);
    h.tick();
    CHECK(h.sent.back() == bytes(expected));
    if (i == 3) {
      h.tick(500);
      CHECK(h.sent.back() == bytes(expected));
    }
    h.receive(0x63, {});
  }
  CHECK(!h.controller.busy() && h.controller.effect_requested());
  h.controller.start_segments(segments);
  h.tick();
  h.controller.start_scene(22);
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(scene_packet(22)));
  h.receive(0x63, {});
  h.controller.start_segments(segments);
  h.tick();
  segments[0].red = 99;
  h.controller.start_segments(segments);
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(segment_packet(0, segments[0])));
  h.controller.stop_music();
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);
  Harness failing;
  failing.boot();
  failing.receive(0xC1, on);
  failing.controller.start_segments(segments);
  failing.tick();
  failing.tick(500);
  failing.tick(500);
  failing.tick(500);
  CHECK(failing.controller.take_music_failure() && !failing.controller.effect_requested());
}

static void music_settings_encoding() {
  MusicSettings fixed{true, true, false, 12, 34, 56};
  CHECK(music_packet(0, 50, fixed).size == 0);
  CHECK(music_packet(0, 50).size == 28);
  auto p = music_packet(8, 99, fixed);
  CHECK(p.size == 28 && p.bytes[12] == 1 && p.bytes[13] == 12 && p.bytes[14] == 34 && p.bytes[15] == 56);
  fixed.calm = true;
  p = music_packet(4, 50, fixed);
  CHECK(p.bytes[12] == 0 && p.bytes[13] == 1 && p.bytes[14] == 12 && p.bytes[15] == 34 && p.bytes[16] == 56);
  CHECK(music_packet(8, 50, fixed).size == 0);
  CHECK(music_packet(0x4D, 50, fixed).size == 0);
  fixed.fixed_color = false;
  p = music_packet(4, 50, fixed);
  CHECK(p.bytes[12] == 0 && p.bytes[13] == 0 && p.bytes[14] == 0);
  Harness h;
  h.boot_on();
  h.controller.start_music(50, 4, fixed);
  h.tick();
  CHECK(h.sent.back() == bytes(p));
  h.receive(0x63, {});
  h.controller.set_palette(RAINBOW);
  h.tick();
  CHECK(h.sent.back() == bytes(p));
}

static void extended_palette_parameters() {
  MusicSettings bars;
  bars.piano_bars = 4;
  CHECK(music_palette(0x34, RAINBOW, bars).bytes[39] == 4);
  bars.piano_bars = 0;
  CHECK(music_palette(0x34, RAINBOW, bars).size == 0);
  bars.piano_bars = 15;
  CHECK(music_palette(0x34, RAINBOW, bars).size == 0);
  auto bloom = music_palette(0x30, RAINBOW);
  CHECK(bloom.size == 48 && bloom.bytes[38] == 10 && bloom.bytes[39] == 80 && bloom.bytes[40] == 0);
  auto bright = music_palette(0x31, RAINBOW);
  CHECK(bright.bytes[38] == 5 && bright.bytes[39] == 100 && bright.bytes[40] == 10);
  auto flow = music_palette(0x59, RAINBOW);
  CHECK(flow.bytes[38] == 0 && flow.bytes[39] == 2 && flow.bytes[40] == 5);
  auto star = music_palette(0x98, RAINBOW);
  CHECK(star.bytes[38] == 0 && star.bytes[39] == 0 && star.bytes[40] == 255 && star.bytes[41] == 3);
  auto piano = music_palette(0x34, RAINBOW);
  CHECK(piano.bytes[39] == 14 && piano.bytes[40] == 10 && piano.bytes[41] == 4 && piano.bytes[42] == 7);
  auto dyn = music_palette(0x33, RAINBOW);
  CHECK(dyn.size == 68 && dyn.bytes[10] == 3 && dyn.bytes[48] == 0xFF && dyn.bytes[49] == 6);
  auto fountain = music_palette(0x35, RAINBOW);
  CHECK(fountain.bytes[38] == 0 && fountain.bytes[39] == 1 && fountain.bytes[40] == 5 && fountain.bytes[41] == 80);
  CHECK(music_palette(0x4A, RAINBOW).bytes[38] == 0);
  CHECK(music_palette(0x5D, RAINBOW).bytes[38] == 8);
  auto day = music_palette(0x37, RAINBOW);
  CHECK(day.bytes[38] == 1 && day.bytes[39] == 10 && day.bytes[40] == 0);
  for (uint8_t mode : {0x30, 0x31, 0x37, 0x59, 0x98, 0x33, 0x34, 0x35, 0x4A, 0x5D}) {
    Harness h;
    h.boot_on();
    h.controller.start_music(99, mode);
    h.tick();
    CHECK(h.sent.back() == bytes(music_palette(mode, RAINBOW)));
    h.receive(0x63, {});
    h.tick();
    CHECK(h.sent.back() == bytes(music_packet(mode, 99)));
    h.receive(0x63, {});
    CHECK(!h.controller.busy());
  }
}

static void legacy_music_initialization() {
  for (uint8_t mode : {5, 0x90, 0x91}) {
    Harness h;
    h.boot_on();
    h.controller.start_music(77, mode);
    h.tick();
    CHECK(h.sent.back() == bytes(music_packet(0, 77)));
    h.receive(0x63, {});
    h.tick();
    CHECK(h.sent.back() == bytes(music_packet(mode, 77)));
    h.receive(0x63, {});
    CHECK(!h.controller.busy());
    h.controller.start_music(77, mode);
    h.tick();
    h.controller.start_scene(22);
    h.receive(0x63, {});
    h.tick();
    CHECK(h.sent.back() == bytes(scene_packet(22)));
  }
}

static void live_palette_update() {
  Harness h;
  h.boot_on();
  h.controller.start_music(65, 0x4D);
  h.tick();
  auto palette = RAINBOW;
  palette[0] = 12;
  palette[1] = 34;
  palette[2] = 56;
  h.controller.set_palette(palette);
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(music_palette(0x4D, palette)));
  h.receive(0x63, {});
  h.tick();
  CHECK(h.sent.back() == bytes(music_packet(0x4D, 65)));
  h.receive(0x63, {});
  h.controller.start_scene(22);
  h.tick();
  h.receive(0x63, {});
  auto count = h.sent.size();
  h.controller.set_palette(RAINBOW);
  h.tick();
  CHECK(h.sent.size() == count);
  CHECK(h.controller.effect_requested() && !h.controller.music_requested());
}

static void music_background_parameters() {
  MusicSettings settings;
  settings.has_background = true;
  settings.background_color = 0x123456;
  settings.background_brightness = 7;
  auto p = music_palette(0x33, RAINBOW, settings);
  CHECK(p.bytes[38] == 0x12 && p.bytes[39] == 0x34 && p.bytes[40] == 0x56 && p.bytes[41] == 7);
  auto star = music_palette(0x98, RAINBOW, settings);
  CHECK(star.bytes[38] == 0x12 && star.bytes[39] == 0x34 && star.bytes[40] == 0x56 && star.bytes[41] == 3);
  Harness h;
  h.boot_on();
  h.controller.start_music(60, 0x33, settings);
  h.tick();
  CHECK(h.sent.back() == bytes(p));
  h.receive(0x63, {});
  h.tick();
  h.receive(0x63, {});
  h.controller.set_palette(RAINBOW);
  h.tick();
  CHECK(h.sent.back() == bytes(p));
  settings.background_brightness = 51;
  CHECK(music_palette(0x33, RAINBOW, settings).size == 0);
}

static void music_gradient_parameters() {
  MusicSettings settings;
  settings.gradient = true;
  auto separation = music_palette(0x32, RAINBOW, settings);
  const auto interpolated = separation;
  CHECK(separation.bytes[39] == 1 && separation.bytes[40] == 0x61);
  CHECK(music_palette(0x34, RAINBOW, settings).bytes[38] == 1);
  CHECK(music_palette(0x37, RAINBOW, settings).bytes[40] == 1);
  settings.gradient = false;
  separation = music_palette(0x32, RAINBOW, settings);
  CHECK(separation.bytes[39] == 0 && separation.bytes[40] == 0x61);
  CHECK(separation.size == interpolated.size);
  for (size_t i = 0; i < separation.size; ++i)
    if (i != 39 && i != 46 && i != 47)
      CHECK(separation.bytes[i] == interpolated.bytes[i]);
}

static std::vector<uint8_t> palette_reply(uint8_t mode, uint8_t status) {
  std::vector<uint8_t> data(20);
  data[0] = 0xA3;
  data[1] = 0x41;
  data[2] = mode;
  data[3] = status;
  for (size_t i = 0; i < 19; ++i)
    data[19] ^= data[i];
  return data;
}
static void palette_rejection_and_stale_replies() {
  Harness h;
  auto on = h.boot_on();
  h.controller.start_music(99, 0x4D);
  h.tick();
  auto n = h.sent.size();
  h.receive(0x63, palette_reply(0x50, 0));
  h.tick();
  CHECK(h.sent.size() == n && h.controller.busy());
  h.receive(0x63, palette_reply(0x4D, 1));
  CHECK(h.controller.take_music_failure() && !h.controller.effect_requested() && h.controller.failed());
  h.tick();
  CHECK(h.sent.back()[6] == 0x88);
  Harness newer;
  newer.boot();
  newer.receive(0xC1, on);
  newer.controller.start_music(99, 0x4D);
  newer.tick();
  newer.controller.start_music(70, 0x50);
  newer.receive(0x63, palette_reply(0x4D, 1));
  CHECK(!newer.controller.take_music_failure());
  newer.tick();
  CHECK(newer.sent.back() == bytes(music_palette(0x50, RAINBOW)));
  newer.receive(0x63, palette_reply(0x50, 0));
  newer.tick();
  CHECK(newer.sent.back() == bytes(music_packet(0x50, 70)));
  n = newer.sent.size();
  newer.receive(0x63, palette_reply(0x50, 1));
  newer.tick();
  CHECK(newer.sent.size() == n && !newer.controller.take_music_failure());
  newer.receive(0x63, {});
  CHECK(!newer.controller.busy());
}

static void custom_profile_brightness_parameters() {
  const auto defaults = custom_music_profile(RAINBOW);
  CHECK(defaults.bytes[23] == 20 && defaults.bytes[24] == 100);
  CHECK(bytes(defaults) == bytes(custom_music_profile(RAINBOW, 1, 5, 0, 1, 0, 0, 0, 0, 0, 20, 100)));
  for (const auto &limits : {std::array<uint8_t, 2>{0, 0}, {0, 100}, {20, 100}, {40, 60}, {100, 100}}) {
    MusicSettings settings;
    settings.custom_brightness_min = limits[0];
    settings.custom_brightness_max = limits[1];
    const auto packet = music_palette(0xFF, RAINBOW, settings);
    CHECK(packet.size == 88 && packet.bytes[23] == limits[0] && packet.bytes[24] == limits[1]);
  }
  for (const auto &limits : {std::array<uint8_t, 2>{101, 101}, {20, 101}, {100, 20}}) {
    MusicSettings settings;
    settings.custom_brightness_min = limits[0];
    settings.custom_brightness_max = limits[1];
    CHECK(music_palette(0xFF, RAINBOW, settings).size == 0);
  }

  for (uint8_t shape = 0; shape <= 5; ++shape) {
    MusicSettings options;
    options.custom_brightness_shape = shape;
    const auto packet = music_palette(0xFF, RAINBOW, options);
    CHECK(packet.size == 88 && packet.bytes[22] == shape);
  }
  MusicSettings invalid_shape;
  invalid_shape.custom_brightness_shape = 6;
  CHECK(music_palette(0xFF, RAINBOW, invalid_shape).size == 0);

  MusicSettings length;
  length.custom_spot_length = 4;
  const auto p = music_palette(0xFF, RAINBOW, length);
  CHECK(p.size == 88 && p.bytes[31] == 4 && p.bytes[32] == 4);
  length.custom_spot_length = 15;
  CHECK(music_palette(0xFF, RAINBOW, length).size == 0);
}

static void custom_profile_color_parameters() {
  CHECK(custom_music_profile(RAINBOW, 1, 5, 0, 1, 0, 0, 1).size == 0);
  CHECK(custom_music_profile(RAINBOW, 2, 5, 0, 1, 0, 0, 2).size == 0);
  CHECK(custom_music_profile(RAINBOW, 1, 5, 0, 1, 0, 0, 2).bytes[33] == 2);
  CHECK(custom_music_profile(RAINBOW, 1, 5, 0, 1, 0, 0, 3).bytes[33] == 3);
  CHECK(custom_music_profile(RAINBOW, 1, 5, 0, 1, 0, 2).size == 0);
  CHECK(custom_music_profile(RAINBOW, 2, 5, 0, 1, 0, 1).size == 0);
  const auto timed = custom_music_profile(RAINBOW, 1, 5, 0, 1, 0, 1);
  CHECK(timed.bytes[59] == 1);
  CHECK(custom_music_profile(RAINBOW, 1, 5, 0, 1, 2).size == 0);
  CHECK(custom_music_profile(RAINBOW, 2, 5, 0, 1, 1).size == 0);
  MusicSettings random_order;
  random_order.custom_color_order = 1;
  const auto sequential_packet = music_palette(0xFF, RAINBOW);
  const auto random_packet = music_palette(0xFF, RAINBOW, random_order);
  CHECK(random_packet.size == sequential_packet.size);
  // After the 3-byte header, body[44] is at payload index 47:
  // third A3 fragment, byte 15; only the value and both checksums change.
  CHECK(random_packet.bytes[62] == 1 && sequential_packet.bytes[62] == 0);
  for (size_t i = 0; i < random_packet.size; ++i) {
    if (i == 62 || i == 66)
      CHECK((random_packet.bytes[i] ^ sequential_packet.bytes[i]) == 1);
    else if (i == 87) {
      uint8_t sum = 0;
      for (size_t n = 0; n < 87; ++n)
        sum += random_packet.bytes[n];
      CHECK(random_packet.bytes[i] == sum);
    } else
      CHECK(random_packet.bytes[i] == sequential_packet.bytes[i]);
  }
}

static void custom_profile_motion_parameters() {
  CHECK(custom_music_profile(RAINBOW, 1, 0).size == 0);
  CHECK(custom_music_profile(RAINBOW, 1, 5, 101).size == 0);
  CHECK(custom_music_profile(RAINBOW, 1, 5, 0, 0).size == 0);
  CHECK(custom_music_profile(RAINBOW, 1, 5, 0, 6).size == 0);
  CHECK(custom_music_profile(RAINBOW, 2, 4).size == 0);
  CHECK(custom_music_profile(RAINBOW, 2, 5, 20).size == 0);
  CHECK(custom_music_profile(RAINBOW, 2, 5, 0, 2).size == 0);
  // Vary independent fields once each, plus one combined non-default profile.
  for (const auto &values : {std::array<uint8_t, 3>{4, 0, 1}, {5, 0, 1}, {5, 20, 1},
                             {5, 100, 1}, {5, 0, 5}, {4, 100, 5}}) {
    MusicSettings options;
    options.custom_direction = values[0];
    options.custom_idle_brightness = values[1];
    options.custom_decay_speed = values[2];
    const auto candidate = music_palette(0xFF, RAINBOW, options);
    CHECK(candidate.size == 88);
    std::vector<uint8_t> body;
    for (size_t i = 0; i < 4; ++i) {
      const size_t offset = 7 + i * 20;
      uint8_t checksum = 0;
      for (size_t n = 0; n < 20; ++n)
        checksum ^= candidate.bytes[offset + n];
      CHECK(checksum == 0);
      body.insert(body.end(), candidate.bytes.begin() + offset + 2, candidate.bytes.begin() + offset + 19);
    }
    CHECK(body[6] == values[0] && body[48] == values[1] && body[50] == values[2]);
  }
}

static void custom_profile_action_types() {
  CHECK(custom_music_profile(RAINBOW, 0).size == 0);
  CHECK(custom_music_profile(RAINBOW, 7).size == 0);
  for (uint8_t type = 1; type <= 6; ++type) {
    MusicSettings settings;
    settings.custom_action_type = type;
    const auto candidate = music_palette(0xFF, RAINBOW, settings);
    CHECK(candidate.size == 88 && candidate.bytes[12] == type);
    Harness selected;
    selected.boot_on();
    selected.controller.start_music(99, 0xFF, settings);
    selected.tick();
    CHECK(selected.sent.back() == bytes(candidate));
    auto ack = palette_reply(type, 0);
    ack[1] = 0x42;
    ack[19] ^= 3;
    selected.receive(0x63, ack);
    selected.tick();
    CHECK(selected.sent.back() == bytes(music_packet(0xFF, 99, settings)));
    selected.receive(0x63, {});
    CHECK(!selected.controller.busy());
  }
}

static void custom_profile_framing_and_rejection() {
  const auto packet = custom_music_profile(RAINBOW);
  CHECK(packet.size == 88 && packet.bytes[2] == 80);
  std::vector<uint8_t> payload;
  for (size_t n = 0; n < 4; ++n) {
    const size_t offset = 7 + n * 20;
    uint8_t sum = 0;
    for (size_t i = 0; i < 20; ++i)
      sum ^= packet.bytes[offset + i];
    CHECK(sum == 0 && packet.bytes[offset] == 0xA3 && packet.bytes[offset + 1] == (n == 3 ? 255 : n));
    payload.insert(payload.end(), packet.bytes.begin() + offset + 2, packet.bytes.begin() + offset + 19);
  }
  CHECK(payload[0] == 1 && payload[1] == 4 && payload[2] == 0x42);
  CHECK(payload[3] == 1 && payload[4] == 56 && payload[22] == 7 && payload[58] == 1);
  // Direction 5 moves the spots; minimum brightness 0 lets them fade out completely.
  CHECK(payload[6] == 5 && payload[48] == 0 && payload[49] == 100);
  Harness h;
  h.boot_on();
  h.controller.start_music(99, 0xFF);
  h.tick();
  CHECK(h.sent.back() == bytes(packet));
  auto reply = palette_reply(1, 0);
  reply[1] = 0x42;
  reply[19] ^= 3;
  h.receive(0x63, reply);
  h.tick();
  CHECK(h.sent.back() == bytes(music_packet(0xFF, 99)));
  h.receive(0x63, {});
  CHECK(!h.controller.busy());
  h.controller.start_music(99, 0xFF);
  h.tick();
  reply[3] = 1;
  reply[19] ^= 1;
  h.receive(0x63, reply);
  CHECK(h.controller.take_music_failure() && !h.controller.effect_requested());
}

static void custom_profile_transport() {
  custom_profile_brightness_parameters();
  custom_profile_color_parameters();
  custom_profile_motion_parameters();
  custom_profile_action_types();
  custom_profile_framing_and_rejection();
}

static void sleep_packet_boundaries() {
  SleepSettings settings;
  settings.custom_color = true;
  settings.rgb = 0x123456;
  settings.temperature = 2200;
  const auto packet = sleep_packet(true, settings);
  CHECK(packet.size == 28 && packet.bytes[6] == 0x83);
  const uint8_t expected[]{0x33, 0x11, 1, 30, 15, 15, 1, 0x12, 0x34, 0x56, 0x08, 0x98};
  CHECK(std::equal(std::begin(expected), std::end(expected), packet.bytes.begin() + 7));
  const auto cancelled = sleep_packet(false, settings);
  CHECK(cancelled.bytes[9] == 0);
  CHECK(std::equal(packet.bytes.begin() + 10, packet.bytes.begin() + 26, cancelled.bytes.begin() + 10));
  uint8_t xor_sum = 0;
  for (size_t i = 7; i < 27; ++i)
    xor_sum ^= packet.bytes[i];
  CHECK(xor_sum == 0 && packet.bytes[27] == checksum(packet.bytes.data(), 27));
  for (uint16_t minutes : {10, 11, 73, 239, 240}) {
    settings.minutes = minutes;
    const auto valid = sleep_packet(true, settings);
    CHECK(valid.size == 28 && valid.bytes[11] == minutes && valid.bytes[12] == minutes);
  }
  for (uint16_t minutes : {0, 9, 241, 256}) {
    settings.minutes = minutes;
    CHECK(sleep_packet(true, settings).size == 0);
    // Cancellation must not depend on invalid settings or remaining values.
    const auto cancel = sleep_packet(false, settings);
    CHECK(cancel.size == 28 && cancel.bytes[9] == 0 && cancel.bytes[11] == cancel.bytes[12]);
  }
  settings = SleepSettings{};
  for (uint8_t brightness : {0, 9, 101, 255}) {
    settings.brightness = brightness;
    CHECK(sleep_packet(true, settings).size == 0);
  }
  settings = SleepSettings{};
  for (uint16_t kelvin : {0, 2000, 9000}) {
    settings.temperature = kelvin;
    CHECK(sleep_packet(true, settings).size == 28);
  }
  for (uint16_t kelvin : {1, 1999, 9001}) {
    settings.temperature = kelvin;
    CHECK(sleep_packet(true, settings).size == 0);
  }
  settings = SleepSettings{};
  settings.rgb = 0x1000000;
  CHECK(sleep_packet(true, settings).size == 0);
  settings.rgb = 0;
  settings.custom_color = true;
  CHECK(sleep_packet(true, settings).size == 0);
}

static void sleep_queue_lifecycle() {
  const std::vector<uint8_t> ack{0x33, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x22};
  SleepSettings settings;
  Harness h;
  CHECK(!h.controller.start_sleep(settings));
  h.boot();
  CHECK(!h.controller.start_sleep(settings));
  auto on = BOOT;
  on[2] = 1;
  h.receive(0xC1, on);
  CHECK(h.controller.start_sleep(settings));
  h.tick();
  CHECK(h.sent.back() == bytes(sleep_packet(true, settings)) && h.controller.sleep_may_be_active());
  h.receive(0x63, {});
  CHECK(h.controller.busy());
  h.receive(0x63, ack);
  CHECK(!h.controller.busy());
  h.controller.start_music(99, 0x04);
  h.tick();
  CHECK(h.sent.back() == bytes(sleep_packet(false, settings)));
  h.receive(0x63, {});
  CHECK(h.controller.busy() && h.controller.sleep_may_be_active());
  h.receive(0x63, ack);
  h.tick();
  CHECK(!h.controller.sleep_may_be_active() && h.sent.back() == bytes(music_packet(0x04, 99)));
  h.receive(0x63, {});

  CHECK(h.controller.start_sleep(settings));
  h.tick();
  settings.minutes = 20;
  CHECK(h.controller.start_sleep(settings));
  h.receive(0x63, ack);
  CHECK(h.controller.busy());
  h.tick();
  CHECK(h.sent.back() == bytes(sleep_packet(true, settings)));
  h.receive(0x63, ack);
  h.controller.cancel_sleep();
  h.tick();
  h.receive(0x63, ack);
  CHECK(!h.controller.busy() && !h.controller.sleep_may_be_active());

  CHECK(h.controller.start_sleep(settings));
  h.tick();
  h.tick(500);
  h.tick(500);
  h.tick(500);
  h.tick();
  CHECK(h.sent.back() == bytes(sleep_packet(false, settings)));
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(h.controller.sleep_cancel_failed() && h.controller.sleep_may_be_active());
  const auto sent_count = h.sent.size();
  h.tick();
  CHECK(h.sent.size() == sent_count);
  h.controller.cancel_sleep();
  h.tick();
  h.receive(0x63, ack);
  CHECK(!h.controller.sleep_cancel_failed() && !h.controller.sleep_may_be_active());

  CHECK(h.controller.start_sleep(settings));
  h.tick();
  h.receive(0xC1, BOOT);
  CHECK(!h.controller.sleep_may_be_active() && !h.controller.busy());
  const auto off_count = h.sent.size();
  h.tick(500);
  CHECK(h.sent.size() == off_count);
}

static void sleep_start_confirmation() {
  // The write response alone does not confirm acceptance. A timer that is still running
  // must be detected from settings read back independently.
  for (unsigned mismatch = 0; mismatch < 6; ++mismatch) {
    Harness h;
    h.boot_on();
    h.controller.enable_sleep_support();
    h.tick();
    std::vector<uint8_t> status{0xAA, 0x11, 0, 30, 15, 0, 0, 255, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    auto seal = [&]() {
      status[19] = 0;
      for (size_t i = 0; i < 19; ++i)
        status[19] ^= status[i];
    };
    seal();
    h.receive(0x63, status);
    h.controller.request_timer_inventory();
    h.tick();
    std::vector<uint8_t> timers(20);
    timers[0] = 0xAA;
    timers[1] = 0x23;
    timers[2] = 0xFF;
    timers[19] = 0x76;
    h.receive(0x63, timers);
    h.tick();
    std::vector<uint8_t> wake(20);
    wake[0] = 0xAA;
    wake[1] = 0x12;
    wake[19] = 0xB8;
    h.receive(0x63, wake);
    CHECK(h.controller.sync_clock(20, 30, 15, 1));
    h.tick();
    std::vector<uint8_t> clock_ack(20);
    clock_ack[0] = 0x33;
    clock_ack[1] = 9;
    clock_ack[19] = 0x3A;
    h.receive(0x63, clock_ack);

    SleepSettings requested;
    requested.minutes = 73;
    requested.brightness = 42;
    requested.custom_color = mismatch >= 3;
    requested.rgb = 0x123456;
    requested.temperature = 2200;
    CHECK(h.controller.start_sleep(requested));
    h.tick();
    CHECK(h.sent.back() == bytes(sleep_packet(true, requested)));
    std::vector<uint8_t> ack(20);
    ack[0] = 0x33;
    ack[1] = 0x11;
    ack[19] = 0x22;
    h.receive(0x63, ack);
    h.tick();
    CHECK(h.sent.back() == bytes(sleep_query_packet()));
    status[2] = 1;
    status[3] = mismatch == 2 ? 30 : 42;
    status[4] = mismatch == 1 ? 15 : 73;
    status[6] = mismatch == 3 ? 0 : requested.custom_color;
    status[7] = 0x12;
    status[8] = 0x34;
    status[9] = mismatch == 4 ? 0x57 : 0x56;
    status[10] = 0x08;
    status[11] = mismatch == 5 ? 0x99 : 0x98;
    status[5] = status[4] - 1;
    seal();
    h.receive(0x63, status);
    CHECK(!h.controller.busy() && h.controller.sleep_status_known());
    CHECK(h.controller.sleep_status().settings.minutes == status[4]);
    CHECK(h.controller.failed() == (mismatch != 0));
    CHECK(h.controller.sleep_cancel_failed() == (mismatch != 0));
    if (mismatch != 0)
      CHECK(!h.controller.start_sleep(requested));
  }
}

static void brightness_recovers_after_sleep_status_timeout() {
  for (bool sleep_active : {false, true}) {
    Harness h;
    auto on = h.boot_on();
    h.controller.enable_sleep_support();
    h.tick();
    std::vector<uint8_t> status{0xAA, 0x11, 0, 30, 15, 0, 0, 255, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto seal_status = [&]() {
      status[19] = 0;
      for (size_t i = 0; i < 19; ++i)
        status[19] ^= status[i];
    };
    seal_status();
    h.receive(0x63, status);
    CHECK(h.controller.sleep_status_known() && !h.controller.busy());

    // A missed periodic sleep status response must not permanently block
    // a brightness value requested afterward.
    h.controller.refresh_sleep_status();
    h.tick();
    auto target = h.controller.state();
    target.brightness = 50;
    h.controller.request(target);
    h.tick(500);
    h.tick(500);
    h.tick(500);
    CHECK(h.controller.sleep_cancel_failed() && h.controller.busy());
    CHECK(h.controller.state().brightness == 100);
    for (unsigned retry = 0; retry < 3; ++retry) {
      const auto count = h.sent.size();
      h.tick(4999);
      CHECK(h.sent.size() == count);
      h.tick(1);
      CHECK(h.sent.size() == count + 1 && h.sent.back() == QUERY);
      h.receive(0xC1, on);
      CHECK(h.controller.connected(h.now));
      CHECK(h.controller.sleep_cancel_failed() && h.controller.busy());
      h.tick();
      CHECK(h.sent.back()[6] == 0x83 && h.sent.back()[7] == 0xAA && h.sent.back()[8] == 0x11);
      if (retry < 2) {
        // Even when sleep responses are missing for more than 15 seconds, the full status
        // and connection stay current; brightness remains locked.
        h.tick(500);
        h.tick(500);
        h.tick(500);
        CHECK(h.controller.connected(h.now));
        CHECK(h.controller.state().brightness == 100 && h.controller.busy());
      }
    }
    CHECK(h.now > 15000 && h.controller.connected(h.now));

    status[2] = sleep_active;
    status[5] = sleep_active ? 7 : 0;
    seal_status();
    h.receive(0x63, status);
    CHECK(h.controller.state().brightness == 100 && h.controller.busy());
    if (sleep_active) {
      // A timer that is still active must be canceled before changing brightness
      // and independently read back as ended.
      h.tick();
      CHECK(h.sent.back()[6] == 0x83 && h.sent.back()[7] == 0x33 && h.sent.back()[8] == 0x11 && h.sent.back()[9] == 0);
      std::vector<uint8_t> ack(20);
      ack[0] = 0x33;
      ack[1] = 0x11;
      ack[19] = 0x22;
      h.receive(0x63, ack);
      h.tick();
      CHECK(h.sent.back()[6] == 0x83 && h.sent.back()[7] == 0xAA && h.sent.back()[8] == 0x11);
      CHECK(h.controller.state().brightness == 100 && h.controller.busy());
      status[2] = 0;
      status[5] = 0;
      seal_status();
      h.receive(0x63, status);
    }
    h.tick();
    CHECK(h.sent.back() == HALF);  // Independent UART target packet for 50 percent.
    CHECK(h.controller.state().brightness == 100 && h.controller.busy());
    h.receive(0xC2, {0, 50});
    CHECK(!h.controller.busy() && !h.controller.sleep_cancel_failed());
    CHECK(h.controller.state().brightness == 50 && h.controller.take_update());
  }
}

static bool same_values(const RgbLightValues &a, const RgbLightValues &b) {
  return a.brightness == b.brightness && a.color_brightness == b.color_brightness && a.red == b.red &&
         a.green == b.green && a.blue == b.blue;
}

static void static_rgb_command_lifecycle() {
  StaticRgbBrightness mapping;
  Harness h;
  auto on = h.boot_on();
  RgbLightValues logical{0.01f, 1.0f, 1.0f, 0.5f, 0.0f};
  auto target = mapping.request(h.controller.state(), logical);
  h.controller.request(target);
  h.tick();
  // RGB 255/128/0 at one percent independently calculates to 3/1/0.
  const std::vector<uint8_t> dimmed{0x55, 1, 5, 0, 0, 0, 0x88, 3, 1, 0, 0, 0, 0xE7};
  CHECK(h.sent.back() == dimmed);
  h.receive(0xC3, {0, 3, 1, 0, 0, 0});
  h.tick();
  const std::vector<uint8_t> one_percent{0x55, 1, 1, 0, 0, 0, 0x87, 1, 0xDF};
  CHECK(h.sent.back() == one_percent);
  h.receive(0xC2, {0, 1});
  CHECK(h.controller.take_update());
  CHECK(same_values(mapping.resolve(h.controller.state()), logical));

  // Only the first wire value is in flight; three new targets are coalesced
  // without deriving the logical color from intermediate responses.
  logical.brightness = 0.1f;
  target = mapping.request(h.controller.state(), logical);
  h.controller.request(target);
  h.tick();
  logical.brightness = 0.3f;
  h.controller.request(mapping.request(h.controller.state(), logical));
  logical.brightness = 0.8f;
  h.controller.request(mapping.request(h.controller.state(), logical));
  h.receive(0xC3, {0, 26, 13, 0, 0, 0});
  CHECK(!h.controller.take_update());
  h.tick();
  CHECK(h.sent.back()[6] == 0x88 && h.sent.back()[7] == 204 && h.sent.back()[8] == 102);
  h.receive(0xC3, {0, 204, 102, 0, 0, 0});
  h.tick();
  CHECK(h.sent.back()[6] == 0x87 && h.sent.back()[7] == 80);
  h.receive(0xC2, {0, 80});
  CHECK(h.controller.take_update());
  CHECK(same_values(mapping.resolve(h.controller.state()), logical));
  CHECK(same_color(mapping.request(h.controller.state(), logical), h.controller.state()));
  auto unconfirmed = logical;
  unconfirmed.brightness = 0.3f;
  mapping.request(h.controller.state(), unconfirmed);
  CHECK(same_values(mapping.resolve(h.controller.state()), logical));

  for (bool power : {false, true}) {
    target = h.controller.state();
    target.power = power;
    h.controller.request(mapping.request(target, logical));
    h.tick();
    CHECK(h.sent.back() == (power ? ON : OFF));
    on[2] = power;
    on[3] = 80;
    on[8] = 204;
    on[9] = 102;
    on[10] = 0;
    h.receive(0xC1, on);
    CHECK(h.controller.take_update());
    CHECK(same_values(mapping.resolve(h.controller.state()), logical));
  }

  // The visible RGB amplitude has already been accepted, even if the following
  // requested native percentage value fails due to missing confirmation.
  logical.brightness = 0.3f;
  h.controller.request(mapping.request(h.controller.state(), logical));
  h.tick();
  CHECK(h.sent.back()[6] == 0x88 && h.sent.back()[7] == 77 && h.sent.back()[8] == 38);
  h.receive(0xC3, {0, 77, 38, 0, 0, 0});
  h.tick();
  CHECK(h.sent.back()[6] == 0x87 && h.sent.back()[7] == 30);
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(h.controller.failed() && h.controller.take_update());
  CHECK(h.controller.state().brightness == 80);
  CHECK(same_values(mapping.resolve(h.controller.state()), logical));

  // The native value 80 remained after the percentage command failed;
  // sleep must still start using the confirmed visible RGB brightness of 30.
  CHECK(mapping.brightness_percent(h.controller.state()) == 30);
  SleepSettings sleep;
  sleep.brightness = mapping.brightness_percent(h.controller.state());
  CHECK(h.controller.start_sleep(sleep));
  h.tick();
  CHECK(h.sent.back()[6] == 0x83 && h.sent.back()[7] == 0x33 && h.sent.back()[8] == 0x11);
  CHECK(h.sent.back()[10] == 30);
}

static void static_rgb_rounding() {
  StaticRgbBrightness mapping;
  RgbLightValues logical;
  State target;
  const std::array<std::array<uint8_t, 3>, 3> rounded{{{0, 0, 0}, {1, 1, 0}, {128, 51, 26}}};
  size_t index = 0;
  for (float brightness : {0.0f, 0.01f, 1.0f}) {
    logical = {brightness, 0.5f, 1.0f, 0.4f, 0.2f};
    target = mapping.request({}, logical);
    CHECK(target.red == rounded[index][0] && target.green == rounded[index][1] && target.blue == rounded[index][2]);
    CHECK(same_values(mapping.resolve(target), logical));
    ++index;
  }
  logical = {25.0f / 255.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  target = mapping.request({}, logical);
  CHECK(target.brightness == 10 && target.red == 25);
  CHECK(same_values(mapping.resolve(target), logical));
}

static void static_rgb_restore_and_external_color() {
  RgbLightValues logical;
  // After an ESP reboot, both the newly scaled color and the
  // unscaled starting state from the previous firmware remain associated with restore.
  logical = {224.0f / 255.0f, 1.0f, 1.0f, 254.0f / 255.0f, 250.0f / 255.0f};
  StaticRgbBrightness reboot;
  reboot.restore(logical);
  State restored;
  restored.brightness = 88;
  restored.red = 224;
  restored.green = 223;
  restored.blue = 220;
  CHECK(same_values(reboot.resolve(restored), logical));
  StaticRgbBrightness migration;
  migration.restore(logical);
  restored.red = 255;
  restored.green = 254;
  restored.blue = 250;
  CHECK(same_values(migration.resolve(restored), logical));

  // A color set directly on the FR is accepted and can be transmitted unchanged
  // without dimming again; restore does not replace it.
  restored.red = 0;
  restored.green = 44;
  restored.blue = 88;
  const auto external = migration.resolve(restored);
  CHECK(external.red == 0 && external.green == 0.5f && external.blue == 1.0f);
  CHECK(same_color(migration.request(restored, external), restored));

  StaticRgbBrightness unknown;
  State bright_external;
  bright_external.brightness = 10;
  bright_external.red = 255;
  bright_external.green = 0;
  bright_external.blue = 0;
  const auto full_red = unknown.resolve(bright_external);
  CHECK(full_red.brightness == 1.0f && full_red.color_brightness == 1.0f);
  CHECK(same_color(unknown.request(bright_external, full_red), bright_external));
}

static void static_rgb_native_brightness() {
  RgbLightValues logical;
  StaticRgbBrightness phase;
  logical = {0.01f, 1.0f, 1.0f, 0.5f, 0.0f};
  auto phase_wire = phase.request({}, logical);
  phase.resolve(phase_wire);
  // C2 changes the effect’s native brightness. The
  // stored static color must not be dimmed again as a result.
  phase_wire.brightness = 80;
  const auto effect_update = phase.for_publication(phase_wire, true);
  CHECK(effect_update.brightness == 0.8f && effect_update.color_brightness == 1.0f);
  CHECK(effect_update.red == 1.0f && effect_update.green == 0.5f && effect_update.blue == 0.0f);
  const auto resumed_static = phase.request(phase_wire, effect_update);
  CHECK(resumed_static.red == 204 && resumed_static.green == 102 && resumed_static.blue == 0);
  // With native sleep mode active, the same reporting boundary preserves native
  // brightness while the logical RGB color remains unchanged.
  phase_wire.brightness = 30;
  const auto sleep_update = phase.for_publication(phase_wire, true);
  CHECK(sleep_update.brightness == 0.3f && sleep_update.color_brightness == 1.0f);
  CHECK(sleep_update.red == 1.0f && sleep_update.green == 0.5f && sleep_update.blue == 0.0f);
}

static void static_rgb_brightness_mapping() {
  static_rgb_command_lifecycle();
  static_rgb_rounding();
  static_rgb_restore_and_external_color();
  static_rgb_native_brightness();
}

static void gamma_brightness_mapping() {
  const auto correct = [](float value) { return value * value; };
  const auto uncorrect = [](float value) { return std::sqrt(value); };
  const RgbLightValues logical{0.5f, 1.0f, 1.0f, 0.5f, 0.0f};
  StaticRgbBrightness mapping;
  State on;
  on.power = true;
  const auto wire = mapping.request(on, logical, correct);
  // Gamma 2: (0.5 * 1)^2 * 255 = 63.75; (0.5 * 0.5)^2 * 255 = 15.9375.
  CHECK(wire.power && wire.red == 64 && wire.green == 16 && wire.blue == 0);
  CHECK(wire.brightness == 50);
  auto published = mapping.resolve(wire, uncorrect);
  CHECK(published.brightness == 0.5f && published.red == 1.0f && published.green == 0.5f);
  CHECK(mapping.brightness_percent(wire, uncorrect) == 50);
  CHECK(same_color(mapping.request(wire, published, correct), wire));

  on.power = false;
  const auto off = mapping.request(on, logical, correct);
  CHECK(!off.power && same_color(off, wire));

  StaticRgbBrightness reboot;
  reboot.restore(logical, correct);
  published = reboot.resolve(wire, uncorrect);
  CHECK(published.brightness == 0.5f && published.green == 0.5f);

  // Older firmware accepted gamma but transmitted linear RGB amplitudes.
  StaticRgbBrightness previous;
  const auto old_wire = previous.request(on, logical);
  StaticRgbBrightness migration;
  migration.restore(logical, correct);
  published = migration.resolve(old_wire, uncorrect);
  CHECK(published.brightness == 0.5f && published.green == 0.5f);

  // An unsolicited hardware color must survive decode/re-encode without double gamma.
  StaticRgbBrightness external;
  State incoming_color;
  incoming_color.brightness = 10;
  incoming_color.red = 64;
  incoming_color.green = 16;
  incoming_color.blue = 4;
  published = external.resolve(incoming_color, uncorrect);
  CHECK(std::abs(published.brightness - std::sqrt(64.0f / 255.0f)) < 0.00001f);
  CHECK(std::abs(published.green - 0.5f) < 0.00001f);
  CHECK(std::abs(published.blue - 0.25f) < 0.00001f);
  CHECK(same_color(external.request(incoming_color, published, correct), incoming_color));

  auto native = wire;
  native.brightness = 25;
  published = mapping.for_publication(native, true, uncorrect);
  CHECK(published.brightness == 0.5f);
  CHECK(published.red == 1.0f && published.green == 0.5f);
  CHECK(same_color(mapping.request(native, published, correct), wire));

  const RgbLightValues dim_color{0.5f, 0.5f, 1.0f, 0.5f, 0.0f};
  const auto dim_wire = mapping.request(on, dim_color, correct);
  CHECK(dim_wire.red == 16 && dim_wire.green == 4);
  published = mapping.resolve(dim_wire, uncorrect);
  CHECK(published.brightness == 0.5f && published.color_brightness == 0.5f);
}

static void timer_summary_format() {
  NativeTimers timers{};
  auto summary = format_timer_summary(timers);
  CHECK(std::string(summary.data()) ==
        "0:disabled,00:00,00,off;1:disabled,00:00,00,off;2:disabled,00:00,00,off;3:disabled,00:00,00,off");
  timers[0] = {true, true, 23, 59, 0x7F};
  timers[3] = {true, false, 8, 5, 1};
  summary = format_timer_summary(timers);
  CHECK(std::string(summary.data()) ==
        "0:enabled,23:59,7F,on;1:disabled,00:00,00,off;2:disabled,00:00,00,off;3:enabled,08:05,01,off");
  timers.fill({false, false, 255, 255, 255});
  summary = format_timer_summary(timers);
  CHECK(std::string(summary.data()) ==
        "0:disabled,255:255,FF,off;1:disabled,255:255,FF,off;2:disabled,255:255,FF,off;3:disabled,255:255,FF,off");
  CHECK(summary.back() == '\0');
}

static void timer_inventory_and_clock_sync() {
  Harness h;
  h.boot();
  h.controller.request_timer_inventory();
  h.tick();
  CHECK(h.sent.back() == bytes(timers_query_packet()) && !h.controller.clock_inventory_clear());
  std::vector<uint8_t> timers(20);
  timers[0] = 0xAA;
  timers[1] = 0x23;
  timers[2] = 0xFF;
  timers[19] = 0x76;
  h.receive(0x63, timers);
  h.tick();
  CHECK(h.sent.back() == bytes(wake_query_packet()));
  std::vector<uint8_t> wake(20);
  wake[0] = 0xAA;
  wake[1] = 0x12;
  wake[19] = 0xB8;
  h.receive(0x63, wake);
  CHECK(h.controller.clock_inventory_clear());
  CHECK(!h.controller.sync_clock(24, 0, 0, 1));
  CHECK(h.controller.sync_clock(20, 30, 15, 1));
  h.tick();
  CHECK(h.sent.back() == bytes(clock_packet(20, 30, 15, 1)));
  h.receive(0x63, {});
  CHECK(!h.controller.clock_synced());
  std::vector<uint8_t> clock_ack(20);
  clock_ack[0] = 0x33;
  clock_ack[1] = 9;
  clock_ack[19] = 0x3A;
  h.receive(0x63, clock_ack);
  CHECK(h.controller.clock_synced());
  h.controller.request_timer_inventory();
  h.tick();
  timers[3] = 0x80;
  timers[19] ^= 0x80;
  h.receive(0x63, timers);
  h.tick();
  h.receive(0x63, wake);
  CHECK(h.controller.active_timer_count() == 1 && !h.controller.clock_inventory_clear());
  CHECK(!h.controller.sync_clock(20, 30, 15, 1));
  h.controller.request_timer_inventory();
  h.tick();
  timers[3] = 0;
  timers[19] ^= 0x80;
  h.receive(0x63, timers);
  h.tick();
  wake[2] = 0xFF;
  wake[19] ^= 0xFF;
  h.receive(0x63, wake);
  CHECK(h.controller.wake_enabled() == 0 && h.controller.clock_inventory_clear());
  CHECK(h.controller.wake_raw_status() == 255);
  h.controller.request_timer_inventory();
  h.tick();
  h.receive(0x63, timers);
  h.tick();
  wake[2] = 1;
  wake[19] ^= 0xFE;
  h.receive(0x63, wake);
  CHECK(h.controller.wake_enabled() == 1 && !h.controller.clock_inventory_clear());
  h.controller.request_timer_inventory();
  h.tick();
  h.receive(0x63, {});
  CHECK(h.controller.busy());
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(!h.controller.clock_inventory_clear() && h.controller.active_timer_count() == -1);
  // Reading the inventory again after it is lost must restore availability without a reboot.
  h.controller.request_timer_inventory();
  h.tick();
  h.receive(0x63, timers);
  h.tick();
  wake[2] = 0;
  wake[19] ^= 1;
  h.receive(0x63, wake);
  CHECK(h.controller.clock_inventory_clear() && !h.controller.busy());
  CHECK(h.controller.sync_clock(20, 30, 20, 1));
  h.tick();
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(h.controller.clock_sync_failed() && !h.controller.clock_synced());
  h.controller.request_timer_inventory();
  h.tick();
  h.receive(0x63, {});
  CHECK(h.controller.clock_sync_failed());
  h.receive(0x63, timers);
  h.tick();
  h.receive(0x63, wake);
  CHECK(!h.controller.clock_sync_failed() && h.controller.sync_clock(20, 30, 25, 1));
  h.tick();
  h.receive(0x63, clock_ack);
  CHECK(h.controller.clock_synced());
}

static void clock_and_timer_packets() {
  CHECK(clock_packet(23, 59, 59, 7).size == 28);
  CHECK(clock_packet(0, 0, 0, 1).size == 28);
  CHECK(clock_packet(-1, 0, 0, 1).size == 0 && clock_packet(24, 0, 0, 1).size == 0);
  CHECK(clock_packet(0, 60, 0, 1).size == 0 && clock_packet(0, 0, 60, 1).size == 0);
  CHECK(clock_packet(0, 0, 0, 0).size == 0 && clock_packet(0, 0, 0, 8).size == 0);
  const auto clock = clock_packet(12, 34, 56, 7);
  CHECK(clock.bytes[7] == 0x33 && clock.bytes[8] == 9 && clock.bytes[9] == 12 && clock.bytes[10] == 34 &&
        clock.bytes[11] == 56 && clock.bytes[12] == 7);
  for (size_t i = 13; i < 26; ++i)
    CHECK(clock.bytes[i] == 0);
  uint8_t sum = 0;
  for (size_t i = 7; i < 27; ++i)
    sum ^= clock.bytes[i];
  CHECK(sum == 0);
  const auto query = timers_query_packet();
  sum = 0;
  for (size_t i = 7; i < 27; ++i)
    sum ^= query.bytes[i];
  CHECK(sum == 0);
  std::array<uint8_t, 20> reply{0xAA, 0x23, 0xFF, 0x81, 6, 30, 0xFF, 0x80, 23, 59, 0x80, 0, 0, 0, 0, 1, 12, 0, 0};
  auto seal = [&]() {
    reply[19] = 0;
    for (size_t i = 0; i < 19; ++i)
      reply[19] ^= reply[i];
  };
  seal();
  NativeTimers timers;
  CHECK(decode_timers(reply.data(), 20, timers));
  CHECK(timers[0].enabled && timers[0].power && timers[0].hour == 6 && timers[0].minute == 30 &&
        timers[0].repeat_mask == 255);
  CHECK(timers[1].enabled && !timers[1].power && timers[1].repeat_mask == 128 && !timers[2].enabled &&
        !timers[3].enabled);
  for (size_t n = 0; n < 20; ++n)
    CHECK(!decode_timers(reply.data(), n, timers));
  reply[4] = 24;
  seal();
  CHECK(!decode_timers(reply.data(), 20, timers) && timers[0].hour == 6);
  reply[4] = 6;
  reply[19] ^= 1;
  CHECK(!decode_timers(reply.data(), 20, timers));
}

static void sleep_status_refresh() {
  Harness h;
  h.boot_on();
  h.controller.enable_sleep_support();
  h.tick();
  std::vector<uint8_t> status{0xAA, 0x11, 1, 30, 15, 7, 0, 255, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < 19; ++i)
    status[19] ^= status[i];
  h.receive(0x63, status);
  CHECK(h.controller.sleep_may_be_active() && !h.controller.busy());
  h.controller.refresh_sleep_status();
  h.tick();
  CHECK(h.sent.back() == bytes(sleep_query_packet()));
  h.receive(0x63, status);
  CHECK(h.controller.sleep_status().enabled && !h.controller.busy());
  const auto count = h.sent.size();
  h.tick();
  CHECK(h.sent.size() == count);
}

static void sleep_cancellation_readback() {
  Harness h;
  auto on = h.boot_on();
  h.controller.enable_sleep_support();
  CHECK(!h.controller.sleep_status_known() && !h.controller.start_sleep(SleepSettings{}));
  h.controller.start_music(99, 0x04);
  h.tick();
  CHECK(h.sent.back() == bytes(sleep_query_packet()));
  h.receive(0x63, {});
  CHECK(!h.controller.sleep_status_known());
  std::vector<uint8_t> status{0xAA, 0x11, 1, 30, 15, 7, 0, 255, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  auto seal = [&]() {
    status[19] = 0;
    for (size_t i = 0; i < 19; ++i)
      status[19] ^= status[i];
  };
  seal();
  h.receive(0x63, status);
  CHECK(h.controller.sleep_status_known() && h.controller.sleep_status().remaining_minutes == 7);
  h.tick();
  CHECK(h.sent.back()[7] == 0x33 && h.sent.back()[8] == 0x11 && h.sent.back()[9] == 0);
  std::vector<uint8_t> ack(20);
  ack[0] = 0x33;
  ack[1] = 0x11;
  ack[19] = 0x22;
  h.receive(0x63, ack);
  h.tick();
  CHECK(h.sent.back() == bytes(sleep_query_packet()));
  h.receive(0x63, status);
  CHECK(h.controller.sleep_cancel_failed());
  const auto sent = h.sent.size();
  h.tick();
  CHECK(h.sent.size() == sent);
  h.controller.cancel_sleep();
  h.tick();
  h.receive(0x63, ack);
  h.tick();
  status[2] = 0;
  status[5] = 0;
  seal();
  h.receive(0x63, status);
  h.tick();
  CHECK(!h.controller.sleep_cancel_failed() && h.sent.back() == bytes(music_packet(0x04, 99)));
  h.receive(0x63, status);
  CHECK(h.controller.busy());  // A status report for an unrelated timer is not a music acknowledgment.
  h.receive(0x63, {});
  CHECK(!h.controller.busy());
  h.tick(LINK_TIMEOUT_MS);
  h.receive(0xC1, on);
  h.tick();
  CHECK(!h.controller.sleep_status_known() && h.sent.back() == bytes(sleep_query_packet()));
  h.tick(500);
  h.tick(500);
  h.tick(500);
  CHECK(h.controller.sleep_cancel_failed() && !h.controller.start_sleep(SleepSettings{}));
}

static void sleep_status_decoding() {
  const auto query = sleep_query_packet();
  CHECK(query.size == 28 && query.bytes[7] == 0xAA && query.bytes[8] == 0x11 && query.bytes[26] == 0xBB);
  std::array<uint8_t, 20> reply{0xAA, 0x11, 1, 30, 15, 7, 1, 0x12, 0x34, 0x56, 0x08, 0x98};
  auto seal = [&]() {
    reply[19] = 0;
    for (size_t i = 0; i < 19; ++i)
      reply[19] ^= reply[i];
  };
  seal();
  SleepStatus status;
  CHECK(decode_sleep_status(reply.data(), reply.size(), status));
  CHECK(status.enabled && status.remaining_minutes == 7 && status.settings.minutes == 15);
  CHECK(status.settings.brightness == 30 && status.settings.rgb == 0x123456 && status.settings.temperature == 2200);
  CHECK(status.settings.custom_color);
  for (size_t n = 0; n < 20; ++n)
    CHECK(!decode_sleep_status(reply.data(), n, status));
  reply[0] = 0x33;
  seal();
  CHECK(!decode_sleep_status(reply.data(), 20, status));
  reply[0] = 0xAA;
  reply[2] = 2;
  seal();
  CHECK(!decode_sleep_status(reply.data(), 20, status));
  reply[2] = 1;
  reply[3] = 101;
  seal();
  CHECK(!decode_sleep_status(reply.data(), 20, status));
  reply[3] = 30;
  reply[19] ^= 1;
  CHECK(!decode_sleep_status(reply.data(), 20, status));
  CHECK(status.settings.rgb == 0x123456);  // Invalid responses do not change the last known state.
  reply[5] = 255;
  reply[7] = reply[8] = reply[9] = 0;
  seal();
  CHECK(decode_sleep_status(reply.data(), 20, status) && status.remaining_minutes == 255 &&
        status.settings.custom_color);
}

static void foreign_sleep_reply_during_music() {
  Harness h;
  h.boot_on();
  h.controller.start_music(99, 0x04);
  h.tick();
  std::vector<uint8_t> foreign(20);
  foreign[0] = 0x33;
  foreign[1] = 0x11;
  foreign[19] = 0x22;
  h.receive(0x63, foreign);
  CHECK(h.controller.busy());
  foreign[1] = 0x05;
  foreign[19] = 0x36;
  h.receive(0x63, foreign);
  CHECK(!h.controller.busy());
}

static void invalid_music_settings_preserve_effect() {
  Harness h;
  h.boot_on();
  h.controller.start_music(99, 0x04);
  h.tick();
  h.receive(0x63, {});
  const auto count = h.sent.size();
  MusicSettings bad;
  bad.custom_action_type = 0;
  h.controller.start_music(99, 0xFF, bad);
  h.tick();
  CHECK(h.controller.music_requested() && !h.controller.busy() && h.sent.size() == count);
  bad = MusicSettings{};
  bad.custom_direction = 0;
  h.controller.start_music(99, 0xFF, bad);
  h.tick();
  CHECK(h.controller.music_requested() && !h.controller.busy() && h.sent.size() == count);
  bad = MusicSettings{};
  bad.background_brightness = 51;
  h.controller.start_music(99, 0x33, bad);
  h.tick();
  CHECK(h.controller.music_requested() && !h.controller.busy() && h.sent.size() == count);
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "dreamview-regression") {
    startup_and_button();
    commands_and_ack();
    coalescing_and_cancel();
    timeouts_and_clock_wrap();
    restart_mode_sync_preserves_power();
    music_encoding_and_lifecycle();
    music_off_and_failure();
    foreign_sleep_reply_during_music();
    sleep_cancellation_readback();
    sleep_queue_lifecycle();
    sleep_start_confirmation();
    std::cout << checks << " assertions passed; DreamView-adjacent light, music and sleep regression\n";
    return 0;
  }

  if (argc == 2 && std::string(argv[1]) == "timer-summary") {
    timer_summary_format();
    std::cout << checks << " assertions passed; fixed-buffer timer summary\n";
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "gamma") {
    gamma_brightness_mapping();
    std::cout << checks << " assertions passed; gamma brightness mapping\n";
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "static-rgb-brightness") {
    static_rgb_brightness_mapping();
    std::cout << checks << " assertions passed; static RGB brightness and logical color feedback\n";
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "brightness-recovery") {
    brightness_recovers_after_sleep_status_timeout();
    std::cout << checks << " assertions passed; brightness control recovers after a missing sleep status response\n";
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "custom-music") {
    custom_profile_transport();
    std::cout << checks << " assertions passed; custom music transport\n";
    return 0;
  }
  timer_summary_format();
  gamma_brightness_mapping();
  timer_inventory_and_clock_sync();
  clock_and_timer_packets();
  sleep_status_refresh();
  sleep_cancellation_readback();
  sleep_status_decoding();
  sleep_queue_lifecycle();
  sleep_start_confirmation();
  brightness_recovers_after_sleep_status_timeout();
  static_rgb_brightness_mapping();
  foreign_sleep_reply_during_music();
  invalid_music_settings_preserve_effect();
  sleep_packet_boundaries();
  custom_profile_transport();
  palette_rejection_and_stale_replies();
  music_gradient_parameters();
  music_background_parameters();
  live_palette_update();
  legacy_music_initialization();
  extended_palette_parameters();
  music_settings_encoding();
  segments_lifecycle();
  segment_encoders();
  scene_upload_lifecycle();
  scenes_and_mode_switching();
  palette_transaction();
  encoders();
  parser_faults();
  startup_and_button();
  commands_and_ack();
  coalescing_and_cancel();
  timeouts_and_clock_wrap();
  music_encoding_and_lifecycle();
  music_off_and_failure();
  music_modes_and_live_sensitivity();
  restart_mode_sync_preserves_power();
  std::cout << checks << " assertions passed; 100000 noise bytes; parser/controller\n";
}
