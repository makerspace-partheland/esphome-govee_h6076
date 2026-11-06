#define USE_GOVEE_DREAMVIEW
#include "../components/govee_h6076/protocol.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace esphome::govee_h6076::protocol;
static const char *current_test = "setup";
#define CHECK(x) \
  do { \
    if (!(x)) { \
      std::cerr << "FAIL " << current_test << ":" << __LINE__ << ": " #x "\n"; \
      std::exit(1); \
    } \
  } while (false)

// Omitted control fields are zero; compare the entire sealed frame, not a prefix.
std::vector<uint8_t> pt_frame(std::initializer_list<uint8_t> fields) {
  CHECK(fields.size() <= 19);
  std::vector<uint8_t> frame(fields);
  frame.resize(20);
  for (size_t i = 0; i < 19; ++i)
    frame[19] ^= frame[i];
  return frame;
}

std::vector<uint8_t> uart_frame(uint8_t command, const std::vector<uint8_t> &payload) {
  const uint32_t length = payload.size();
  std::vector<uint8_t> frame{0x55, 1, uint8_t(length), uint8_t(length >> 8), uint8_t(length >> 16),
                             uint8_t(length >> 24), command};
  frame.insert(frame.end(), payload.begin(), payload.end());
  uint8_t sum = 0;
  for (auto byte : frame)
    sum += byte;
  frame.push_back(sum);
  return frame;
}

void check_packet(const Packet &actual, uint8_t command, const std::vector<uint8_t> &payload) {
  const auto expected = uart_frame(command, payload);
  CHECK(actual.size == expected.size());
  const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.bytes.begin());
  if (mismatch.first != expected.end())
    std::cerr << "Packet byte " << (mismatch.first - expected.begin()) << ": expected "
              << unsigned(*mismatch.first) << ", got " << unsigned(*mismatch.second) << "\n";
  CHECK(mismatch.first == expected.end());
}

// Synthetic addresses; independent byte fixtures follow the qualified H6076 wire format.
const DreamViewMembers MEMBERS{{{0x02, 0x12, 0x34, 0x56, 0x78, 0x9A}, {0x02, 0x12, 0x34, 0x56, 0x78, 0x9B}}};
using Op = DreamView::Operation;
struct Harness {
  Controller c;
  Parser parser;
  uint32_t now{500};
  std::vector<Packet> sent;
  DreamViewStoredGroup flash;
  bool automatic;
  bool storage_succeeds{true};
  Harness(bool automatic = false, size_t count = 1, const DreamViewStoredGroup *stored = nullptr)
      : automatic(automatic) {
    if (stored != nullptr)
      flash = *stored;
    else if (!automatic)
      flash = {1, DreamViewStoredState::GROUP_REMOVED, uint8_t(count), MEMBERS};
    c.begin(0);
    c.dreamview().set_members(MEMBERS, count, flash);
  }
  void tick(uint32_t advance = 0) {
    now += advance;
    if (c.dreamview().storage_pending()) {
      if (storage_succeeds)
        flash = c.dreamview().stored_group();
      c.dreamview().storage_complete(storage_succeeds);
    }
    c.tick(now, [this](const Packet &p) { sent.push_back(p); });
  }
  void receive(uint8_t command, const std::vector<uint8_t> &data) {
    for (auto byte : uart_frame(command, data))
      parser.feed(byte, now, [this](uint8_t cmd, const uint8_t *p, size_t n) { c.receive(cmd, p, n, now); });
  }
  void reply(std::initializer_list<uint8_t> fields) { receive(0x63, pt_frame(fields)); }
  void power(bool on) { receive(0xC1, {0x0D, 0, uint8_t(on), 100, 0, 2, 1, 5, 255, 112, 45, 0, 0}); }
  void expect_payload(uint8_t command, const std::vector<uint8_t> &payload) {
    const auto before = sent.size();
    tick();
    CHECK(sent.size() == before + 1);
    check_packet(sent.back(), command, payload);
  }
  void expect(std::initializer_list<uint8_t> fields) { expect_payload(0x83, pt_frame(fields)); }
  void exchange(std::initializer_list<uint8_t> command, std::initializer_list<uint8_t> response) {
    expect(command);
    reply(response);
  }
  void expect_group() {
    auto payload = pt_frame({0xA3, 0, 1, 2, 0x60, 1, 1, 0, 11, 0x9A, 0x78, 0x56, 0x34, 0x12, 2, 7});
    const auto end = pt_frame({0xA3, 0xFF});
    payload.insert(payload.end(), end.begin(), end.end());
    expect_payload(0x83, payload);
  }
  void confirm_disable() {
    exchange({0x33, 0x60, 1, 0}, {0x33, 0x60, 1, 0});
    exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  }
  void clear_members() {
    exchange({0x33, 0x60, 7, 0xFF}, {0x33, 0x60, 7, 0});
    exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 0});
  }
  void confirm_music_effect(uint8_t effect, uint8_t softness, uint8_t sensitivity) {
    exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, effect, 0, softness, sensitivity});
    exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 1, 0});
    exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 2});
  }
  void connect_group() {
    boot();
    create();
    enable();
  }
  void boot() {
    tick(); power(false);
    exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
    exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 0});
    tick();
    CHECK(!c.dreamview().connected());
    if (!automatic)
      CHECK(!c.native_mode_protected());
    CHECK(c.dreamview().enabled_known() && !c.dreamview().enabled());
    CHECK(c.dreamview().connection_known());
    CHECK(!c.dreamview().failed());
  }
  void create() {
    CHECK(c.request_dreamview(Op::CREATE, now));
    confirm_disable();
    clear_members();
    expect_group();
    reply({0xA3, 0x60, 0});
    CHECK(!c.dreamview().connected());
    exchange({0xAA, 0x60, 6}, {0xAA, 0x60, 6, 1});
    CHECK(std::string(c.dreamview().status()) == "configured");
  }
  void enable() {
    c.sync_static_mode(); // Off-state deferred static restore used to kill native activation.
    CHECK(c.request_dreamview(Op::ENABLE, now));
    expect_payload(0x86, {1});
    power(true);
    exchange({0x33, 0x60, 1, 1}, {0x33, 0x60, 1, 0});
    CHECK(!c.dreamview().connected());
    exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 1, 0});
    exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 2});
    CHECK(c.dreamview().connected());
    c.sync_static_mode();
    const auto n = sent.size(); tick(); power(true); tick();
    CHECK(sent.size() == n); // No unsolicited static color ends DreamView.
  }
};

void prepare_sleep_support(Harness &h) {
  h.c.enable_sleep_support();
  h.exchange({0xAA, 0x11}, {0xAA, 0x11, 0, 30, 15, 0, 0, 255, 50, 0, 0, 0});
  h.c.request_timer_inventory();
  h.exchange({0xAA, 0x23, 0xFF}, {0xAA, 0x23, 0xFF});
  h.exchange({0xAA, 0x12}, {0xAA, 0x12, 0});
  CHECK(h.c.sync_clock(20, 30, 15, 1));
  h.exchange({0x33, 9, 20, 30, 15, 1}, {0x33, 9, 0});
}

void music_effect_preserves_group() {
  // Native DreamView type-1 programs are independent of ordinary lamp effects.
  Harness group; group.connect_group();
  group.c.dreamview().enable_music_control();
  CHECK(!group.c.request_dreamview_music(12, group.now));
  CHECK(group.c.request_dreamview_music(11, group.now));
  CHECK(group.c.dreamview().connected() && group.c.dreamview().connection_known());
  group.exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, 0, 0, 1, 72});
  CHECK(group.c.dreamview().music_known() && group.c.dreamview().music_effect() == 0);
  group.expect({0x33, 0x60, 2, 1, 11, 0, 1, 72});
  CHECK(group.c.dreamview().music_effect() == 0);  // Never publish the requested value optimistically.
  group.reply({0x33, 0x60, 2, 0});
  group.confirm_music_effect(11, 1, 72);
  CHECK(group.c.dreamview().enabled() && group.c.dreamview().connected());
  CHECK(group.c.dreamview().music_effect() == 11 && !group.c.dreamview().failed());
}

void latest_queued_music_effect_wins() {
  Harness group; group.connect_group();
  group.c.dreamview().enable_music_control();
  // Preserve an in-flight choice, then apply only the latest queued choice.
  CHECK(group.c.request_dreamview_music(0, group.now));
  group.exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, 11, 0, 1, 72});
  group.expect({0x33, 0x60, 2, 1, 0, 0, 1, 72});
  CHECK(group.c.request_dreamview_music(5, group.now));
  CHECK(group.c.request_dreamview_music(6, group.now));
  CHECK(group.c.dreamview().connected() && group.c.dreamview().connection_known());
  group.reply({0x33, 0x60, 2, 0});
  group.confirm_music_effect(0, 1, 72);
  group.exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, 0, 0, 1, 72});
  group.exchange({0x33, 0x60, 2, 1, 6, 0, 1, 72}, {0x33, 0x60, 2, 0});
  group.confirm_music_effect(6, 1, 72);
  CHECK(group.c.dreamview().music_effect() == 6);
  group.tick(15001);
  CHECK(!group.c.dreamview().music_known());
}

void music_readback_at_startup() {
  // Startup adopts the actual saved program without changing it or enabling sync.
  Harness read; read.c.dreamview().enable_music_control();
  read.tick(); read.power(false);
  read.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  read.exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 0});
  read.exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, 7, 0, 0, 100});
  read.tick();
  CHECK(read.c.dreamview().music_known() && read.c.dreamview().music_effect() == 7);
  CHECK(!read.c.request_dreamview_music(1, read.now));
  CHECK(!read.c.dreamview().enabled() && !read.c.dreamview().failed());
}

void music_rejection_and_readback_mismatch() {
  for (bool rejected : {true, false}) {
    Harness bad; bad.connect_group(); bad.c.dreamview().enable_music_control();
    CHECK(bad.c.request_dreamview_music(1, bad.now));
    bad.exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, 0, 0, 0, 100});
    bad.expect({0x33, 0x60, 2, 1, 1, 0, 0, 100});
    if (rejected) {
      bad.reply({0x33, 0x60, 2, 1});
    } else {
      bad.reply({0x33, 0x60, 2, 0});
      bad.exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, 0, 0, 0, 100});
    }
    CHECK(bad.c.dreamview().failed() && !bad.c.dreamview().music_known());
  }
}

void stop_before_music_write() {
  // OFF takes precedence over a not-yet-sent program write.
  Harness stop; stop.connect_group(); stop.c.dreamview().enable_music_control();
  CHECK(stop.c.request_dreamview_music(2, stop.now));
  stop.expect({0xAA, 0x60, 2});
  CHECK(stop.c.request_dreamview(Op::DISABLE, stop.now));
  stop.reply({0xAA, 0x60, 2, 1, 0, 0, 0, 100});
  stop.confirm_disable();
  CHECK(!stop.c.dreamview().enabled());
}

void stop_survives_music_timeout() {
  // A lost music ACK must not swallow an OFF request queued behind it.
  Harness timeout; timeout.connect_group(); timeout.c.dreamview().enable_music_control();
  CHECK(timeout.c.request_dreamview_music(3, timeout.now));
  timeout.exchange({0xAA, 0x60, 2}, {0xAA, 0x60, 2, 1, 0, 0, 0, 100});
  timeout.expect({0x33, 0x60, 2, 1, 3, 0, 0, 100});
  CHECK(timeout.c.request_dreamview(Op::DISABLE, timeout.now));
  const auto sent = timeout.sent.size();
  timeout.tick(500); timeout.tick(500); timeout.tick(500);
  CHECK(timeout.sent.size() == sent + 2 && timeout.c.dreamview().failed());
  timeout.expect({0x33, 0x60, 1, 0});
}

void group_lifecycle() {
  Harness h; h.boot();
  h.create(); h.enable();
  CHECK(h.c.request_dreamview(Op::DISABLE, h.now));
  h.expect({0x33, 0x60, 1, 0});
  const auto n = h.sent.size();
  h.receive(0x63, {}); h.reply({0x33, 0x11, 0}); h.reply({0xEE, 0x60, 1, 0}); h.tick();
  CHECK(h.sent.size() == n); // Empty, foreign and unsolicited reports cannot acknowledge the write.
  h.reply({0x33, 0x60, 1, 0});
  h.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  CHECK(!h.c.dreamview().connected());
  CHECK(std::string(h.c.dreamview().status()) == "configured"); // Off retains membership.
  CHECK(h.c.request_dreamview(Op::REMOVE, h.now));
  h.confirm_disable();
  h.clear_members();
  CHECK(std::string(h.c.dreamview().status()) == "removed");
  CHECK(h.c.request_dreamview(Op::ENABLE, h.now)); // A single switch action recreates a removed group.
}

void stale_uart_invalidates_connection() {
  Harness timeout; timeout.connect_group();
  timeout.tick(15001);
  CHECK(!timeout.c.dreamview().connected()); // Stale UART cannot leave a confirmed group in HA.
  CHECK(!timeout.c.dreamview().enabled_known());
  CHECK(!timeout.c.dreamview().connection_known());
}

void rejected_group_creation() {
  Harness rejected; rejected.boot();
  CHECK(rejected.c.request_dreamview(Op::CREATE, rejected.now));
  rejected.exchange({0x33, 0x60, 1, 0}, {0x33, 0x60, 1, 1});
  CHECK(std::string(rejected.c.dreamview().status()) == "error");
  CHECK(!rejected.c.dreamview().connected());
}

void group_creation_retry_limit() {
  Harness lost; lost.boot();
  CHECK(lost.c.request_dreamview(Op::CREATE, lost.now)); lost.tick();
  const auto before = lost.sent.size();
  lost.tick(500); lost.tick(500); lost.tick(500);
  CHECK(lost.sent.size() == before + 2);
  CHECK(std::string(lost.c.dreamview().status()) == "error");
  CHECK(!lost.c.dreamview().connected());
}

void local_music_stops_synchronization() {
  Harness effect; effect.connect_group();
  effect.c.start_music(50, 4);
  effect.confirm_disable();
  effect.expect({0x33, 5, 15, 4, 50, 1}); // Native sync is confirmed off before local music starts.
}

void missing_member_connection_timeout() {
  Harness unavailable; unavailable.boot(); unavailable.create();
  CHECK(unavailable.c.request_dreamview(Op::ENABLE, unavailable.now));
  unavailable.tick(); unavailable.power(true);
  unavailable.exchange({0x33, 0x60, 1, 1}, {0x33, 0x60, 1, 0});
  unavailable.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 1});
  unavailable.exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 1});
  CHECK(!unavailable.c.dreamview().connected());
  CHECK(std::string(unavailable.c.dreamview().status()) == "connecting");
  // The toggle reflects native activation, independently of peer connectivity.
  CHECK(unavailable.c.dreamview().enabled_known() && unavailable.c.dreamview().enabled());
  CHECK(unavailable.c.dreamview().connection_known());
  CHECK(!unavailable.c.dreamview().failed());
  // Keep UART and status replies fresh: only the peer connection is missing.
  for (unsigned elapsed = 5000; elapsed <= 30000; elapsed += 5000) {
    unavailable.now += 5000;
    unavailable.power(true);
    unavailable.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 1});
    unavailable.exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 1});
    CHECK(unavailable.c.connected(unavailable.now) && unavailable.c.dreamview().enabled_known());
    CHECK(unavailable.c.dreamview().failed() == (elapsed == 30000));
  }
  CHECK(!unavailable.c.dreamview().connected());
  CHECK(std::string(unavailable.c.dreamview().status()) == "error");
  CHECK(unavailable.c.dreamview().failed());
}

void power_off_cancels_enable() {
  Harness reversal; reversal.boot(); reversal.create();
  CHECK(reversal.c.request_dreamview(Op::ENABLE, reversal.now));
  reversal.tick(); // Power-on is in flight, enable has not been sent.
  auto off = reversal.c.state(); off.power = false;
  reversal.c.request(off); reversal.power(true);
  reversal.tick(); CHECK(reversal.sent.back().bytes[6] == 0x86 && reversal.sent.back().bytes[7] == 0);
  reversal.power(false);
  reversal.expect({0x33, 0x60, 1, 0}); // Never issue the cancelled enable.
}

void enable_readback_mismatch() {
  Harness mismatch; mismatch.boot(); mismatch.create();
  CHECK(mismatch.c.request_dreamview(Op::ENABLE, mismatch.now));
  mismatch.tick(); mismatch.power(true);
  mismatch.exchange({0x33, 0x60, 1, 1}, {0x33, 0x60, 1, 0});
  mismatch.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  CHECK(std::string(mismatch.c.dreamview().status()) == "error");
  CHECK(!mismatch.c.dreamview().connected());
}

void sleep_starts_after_confirmed_disable() {
  Harness local_sleep; local_sleep.connect_group();
  prepare_sleep_support(local_sleep);
  SleepSettings invalid_sleep; invalid_sleep.minutes = 0;
  const auto before_sleep = local_sleep.sent.size();
  CHECK(!local_sleep.c.start_sleep(invalid_sleep));
  local_sleep.tick(); CHECK(local_sleep.sent.size() == before_sleep);
  CHECK(local_sleep.c.dreamview().enabled());
  CHECK(local_sleep.c.start_sleep({}));
  local_sleep.exchange({0x33, 0x60, 1, 0}, {0x33, 0x60, 1, 0});
  CHECK(!local_sleep.c.sleep_status().enabled);
  local_sleep.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  CHECK(!local_sleep.c.dreamview().enabled());
  local_sleep.exchange({0x33, 0x11, 1, 30, 15, 15, 0, 255, 50, 0, 0, 0}, {0x33, 0x11, 0});
  CHECK(!local_sleep.c.sleep_status_known());
  local_sleep.exchange({0xAA, 0x11}, {0xAA, 0x11, 1, 30, 15, 15, 0, 255, 50, 0, 0, 0});
  CHECK(local_sleep.c.sleep_status_known() && local_sleep.c.sleep_status().enabled);
  CHECK(std::string(local_sleep.c.dreamview().status()) == "configured");
}

void failed_disable_prevents_sleep() {
  // Neither a rejected disable, an unchanged enable flag nor a timeout may start sleep.
  for (unsigned failure = 0; failure < 3; ++failure) {
    Harness refused; refused.connect_group(); prepare_sleep_support(refused);
    CHECK(refused.c.start_sleep({}));
    const auto before = refused.sent.size();
    refused.expect({0x33, 0x60, 1, 0});
    if (failure == 0) {
      refused.reply({0x33, 0x60, 1, 1});
    } else if (failure == 1) {
      refused.reply({0x33, 0x60, 1, 0});
      refused.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 1});
    } else {
      refused.tick(500); refused.tick(500); refused.tick(500);
    }
    CHECK(refused.c.dreamview().failed());
    CHECK(!refused.c.sleep_status().enabled);
    refused.tick();
    for (size_t i = before; i < refused.sent.size(); ++i)
      CHECK(refused.sent[i].bytes[6] != 0x83 || refused.sent[i].bytes[8] != 0x11);
  }
}

void cancel_sleep_during_transition() {
  // Cancelling while DreamView is stopping must cancel the pending sleep start too.
  Harness cancelled_sleep; cancelled_sleep.connect_group();
  prepare_sleep_support(cancelled_sleep);
  CHECK(cancelled_sleep.c.start_sleep({}));
  cancelled_sleep.expect({0x33, 0x60, 1, 0});
  cancelled_sleep.c.cancel_sleep();
  cancelled_sleep.reply({0x33, 0x60, 1, 0});
  cancelled_sleep.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  cancelled_sleep.expect({0x33, 0x11, 0, 30, 15, 15, 0, 255, 50, 0, 0, 0});
}

void malformed_ack_is_ignored() {
  Harness malformed; malformed.boot(); malformed.create();
  CHECK(malformed.c.request_dreamview(Op::ENABLE, malformed.now));
  malformed.tick(); malformed.power(true);
  malformed.expect({0x33, 0x60, 1, 1});
  malformed.receive(0x63, {0x33, 0x60, 1, 0});
  std::vector<uint8_t> invalid(20); invalid[0] = 0x33; invalid[1] = 0x60; invalid[2] = 1;
  malformed.receive(0x63, invalid);
  const auto malformed_count = malformed.sent.size(); malformed.tick();
  CHECK(malformed.sent.size() == malformed_count);
  malformed.reply({0x33, 0x60, 1, 0});
  malformed.expect({0xAA, 0x60, 1});
}

void restart_preserves_native_session() {
  // A saved ESP effect must not override an FR session that survived an ESP restart.
  Controller reboot;
  reboot.begin(0); reboot.start_music(40, 4);
  const DreamViewStoredGroup restored_group{1, DreamViewStoredState::GROUP_APPLIED, 1, MEMBERS};
  reboot.dreamview().set_members(MEMBERS, 1, restored_group);
  const uint8_t on_state[]{0x0D, 0, 1, 100, 0, 2, 1, 5, 255, 112, 45, 0, 0};
  std::vector<Packet> reboot_sent;
  reboot.tick(500, [&](const Packet &p) { reboot_sent.push_back(p); });
  reboot.receive(0xC1, on_state, sizeof(on_state), 500);
  reboot.tick(500, [&](const Packet &p) { reboot_sent.push_back(p); });
  uint8_t active[20]{0xAA, 0x60, 1, 1};
  for (size_t i = 0; i < 19; ++i) active[19] ^= active[i];
  reboot.receive(0x63, active, 20, 500);
  reboot.tick(501, [&](const Packet &p) { reboot_sent.push_back(p); });
  CHECK(reboot_sent.back().bytes[7] == 0xAA && reboot_sent.back().bytes[9] == 6);
  uint8_t configured[20]{0xAA, 0x60, 6, 1};
  for (size_t i = 0; i < 19; ++i) configured[19] ^= configured[i];
  reboot.receive(0x63, configured, 20, 501);
  reboot.tick(502, [&](const Packet &p) { reboot_sent.push_back(p); });
  uint8_t connected[20]{0xAA, 0x60, 5, 2};
  for (size_t i = 0; i < 19; ++i) connected[19] ^= connected[i];
  reboot.receive(0x63, connected, 20, 502);
  reboot.tick(503, [&](const Packet &p) { reboot_sent.push_back(p); });
  CHECK(reboot_sent.size() == 4); // Read-only restart, no group upload or mode write.
  CHECK(!reboot.effect_requested());
  CHECK(reboot.take_music_cancelled());
  CHECK(reboot.native_mode_protected());
  CHECK(std::string(reboot.dreamview().status()) == "connected");
  CHECK(reboot.dreamview().enabled_known() && reboot.dreamview().enabled());
  CHECK(reboot.dreamview().connection_known());
  CHECK(reboot.dreamview().connected());
}

void disconnected_member_clears_connection() {
  Harness stale; stale.connect_group();
  stale.tick(5000); stale.reply({0xAA, 0x60, 1, 1});
  stale.exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 0});
  CHECK(!stale.c.dreamview().connected());
}

void power_timeout_does_not_cancel_next_request() {
  Harness power_timeout; power_timeout.boot(); power_timeout.create();
  CHECK(power_timeout.c.request_dreamview(Op::ENABLE, power_timeout.now));
  power_timeout.tick();
  power_timeout.tick(500); power_timeout.tick(500); power_timeout.tick(500);
  CHECK(std::string(power_timeout.c.dreamview().status()) == "error");
  CHECK(!power_timeout.c.dreamview().connected());
  // A new action can arrive between the failed power command and the next loop.
  // The old failure must already be consumed, not cancel this new selection.
  power_timeout.c.start_music(50, 4);
  power_timeout.expect({0x33, 0x60, 1, 0});
  CHECK(power_timeout.c.effect_requested());
}

void disable_queued_during_poll() {
  Harness queued; queued.connect_group();
  queued.tick(5000); // A read-only poll is in flight.
  CHECK(queued.c.request_dreamview(Op::DISABLE, queued.now));
  queued.reply({0xAA, 0x60, 1, 1});
  queued.exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 2});
  CHECK(!queued.c.dreamview().connected());
  queued.expect({0x33, 0x60, 1, 0});
}

void upload_rejects_foreign_ack() {
  Harness upload_rejected; upload_rejected.boot();
  CHECK(upload_rejected.c.request_dreamview(Op::CREATE, upload_rejected.now));
  upload_rejected.confirm_disable();
  upload_rejected.clear_members();
  upload_rejected.expect_group();
  upload_rejected.reply({0xA3, 0x41, 0, 0});
  const auto upload_count = upload_rejected.sent.size(); upload_rejected.tick();
  CHECK(upload_rejected.sent.size() == upload_count);
  upload_rejected.reply({0xA3, 0x60, 1});
  CHECK(std::string(upload_rejected.c.dreamview().status()) == "error");
  CHECK(!upload_rejected.c.dreamview().connected());
}

void local_mode_retry_after_failed_disable() {
  // Failed native exit must reject the local request, then allow the same value
  // to be selected again. Exercise both timeout and explicit rejection paths.
  for (bool music : {false, true}) {
    Harness recovery; recovery.connect_group();
    recovery.c.take_music_cancelled();
    auto requested = recovery.c.state();
    requested.red = 17; requested.green = 23; requested.blue = 31;
    if (music)
      recovery.c.start_music(50, 4);
    else
      recovery.c.request(requested);
    recovery.expect({0x33, 0x60, 1, 0});
    if (music) {
      recovery.reply({0x33, 0x60, 1, 1});
    } else {
      recovery.tick(500); recovery.tick(500); recovery.tick(500);
    }
    CHECK(recovery.c.dreamview().failed());
    CHECK(!recovery.c.busy());
    CHECK(!recovery.c.effect_requested());
    CHECK(recovery.c.take_music_cancelled());
    CHECK(recovery.c.take_update());
    const auto failed_count = recovery.sent.size(); recovery.tick();
    CHECK(recovery.sent.size() == failed_count); // No automatic retry loop or unsent color.
    if (music)
      recovery.c.start_music(50, 4);
    else
      recovery.c.request(requested);
    recovery.confirm_disable();
    CHECK(!recovery.c.dreamview().failed());
    if (music) {
      recovery.expect({0x33, 5, 15, 4, 50, 1});
    } else {
      recovery.expect_payload(0x88, {17, 23, 31, 0, 0});
    }
  }
}

void automatic_setup_restores_power() {
  // First configuration is installed without a CREATE request from Home Assistant.
  Harness initial(true); initial.boot();
  initial.expect({0x33, 0x60, 1, 0});
  CHECK(initial.flash.state == DreamViewStoredState::GROUP_INVALID);
  initial.reply({0x33, 0x60, 1, 0});
  initial.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  initial.clear_members();
  initial.expect_group(); initial.reply({0xA3, 0x60, 0});
  initial.power(true); // Native group creation may power on the lamp.
  initial.exchange({0xAA, 0x60, 6}, {0xAA, 0x60, 6, 1});
  initial.tick();
  CHECK(initial.sent.back().bytes[6] == 0x86 && initial.sent.back().bytes[7] == 0);
  initial.power(false);
  CHECK(initial.flash.state == DreamViewStoredState::GROUP_APPLIED && initial.flash.count == 1);
  CHECK(!initial.c.dreamview().enabled());
  CHECK(!initial.c.busy());
}

void removed_member_replaces_native_list() {
  // Removing one YAML entry replaces the whole native list, not just its first slot.
  DreamViewStoredGroup previous{1, DreamViewStoredState::GROUP_APPLIED, 2, MEMBERS};
  Harness fewer(true, 1, &previous); fewer.boot();
  fewer.confirm_disable();
  fewer.exchange({0x33, 0x60, 7, 0xFF}, {0x33, 0x60, 7, 0});
  fewer.expect({0xAA, 0x60, 5});
  fewer.reply({0xAA, 0x60, 5, 1}); // Still disconnecting: no replacement upload yet.
  const auto waiting_count = fewer.sent.size(); fewer.tick();
  CHECK(fewer.sent.size() == waiting_count);
  fewer.tick(500); CHECK(fewer.sent.back().bytes[7] == 0xAA && fewer.sent.back().bytes[9] == 5);
  fewer.reply({0xAA, 0x60, 5, 0});
  fewer.expect_group(); fewer.reply({0xA3, 0x60, 0});
  fewer.exchange({0xAA, 0x60, 6}, {0xAA, 0x60, 6, 1});
  fewer.tick(); fewer.power(false);
  CHECK(fewer.flash.count == 1 && fewer.flash.members[0] == MEMBERS[0]);
}

void empty_list_removes_group() {
  const DreamViewStoredGroup previous{1, DreamViewStoredState::GROUP_APPLIED, 2, MEMBERS};
  Harness empty(true, 0, &previous); empty.boot();
  empty.confirm_disable();
  empty.clear_members();
  empty.tick(); empty.power(false);
  CHECK(empty.flash.state == DreamViewStoredState::GROUP_REMOVED && empty.flash.count == 0);
  CHECK(std::string(empty.c.dreamview().status()) == "removed");
  CHECK(!empty.c.request_dreamview(Op::ENABLE, empty.now));
  for (const auto &packet : empty.sent)
    CHECK(packet.bytes[6] != 0x83 || packet.bytes[7] != 0xA3);
}

void storage_failure_prevents_writes() {
  Harness no_storage(true); no_storage.boot();
  no_storage.storage_succeeds = false;
  const auto before_storage_error = no_storage.sent.size(); no_storage.tick();
  CHECK(no_storage.sent.size() == before_storage_error);
  CHECK(no_storage.c.dreamview().failed());
  CHECK(!no_storage.c.dreamview().connected());
}

void interrupted_setup_is_retried() {
  // A reset between invalidating the receipt and finishing the upload must retry.
  DreamViewStoredGroup interrupted{1, DreamViewStoredState::GROUP_INVALID, 1, MEMBERS};
  Harness retry(true, 1, &interrupted); retry.boot();
  retry.expect({0x33, 0x60, 1, 0});
  CHECK(!retry.c.dreamview().connected());
}

void single_switch_action_recreates_group() {
  // A single ON command recreates an explicitly removed group, then enables it.
  Harness single_on; single_on.boot();
  CHECK(single_on.c.request_dreamview(Op::ENABLE, single_on.now));
  single_on.tick(); single_on.power(true);
  single_on.confirm_disable();
  single_on.clear_members();
  single_on.expect_group(); single_on.reply({0xA3, 0x60, 0});
  single_on.exchange({0xAA, 0x60, 6}, {0xAA, 0x60, 6, 1});
  single_on.exchange({0x33, 0x60, 1, 1}, {0x33, 0x60, 1, 0});
  single_on.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 1});
  single_on.exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 2});
  CHECK(single_on.c.dreamview().connected());
}

void changed_address_replaces_group() {
  DreamViewStoredGroup changed_address{1, DreamViewStoredState::GROUP_APPLIED, 1, MEMBERS};
  changed_address.members[0][5] = 0xA0;
  Harness address_change(true, 1, &changed_address); address_change.boot();
  address_change.expect({0x33, 0x60, 1, 0});
  CHECK(address_change.flash.state == DreamViewStoredState::GROUP_INVALID);
  CHECK(!address_change.c.dreamview().connected());
}

void sleep_delays_setup_and_preserves_current_power() {
  // An active sleep timer delays automatic membership writes, not status reads.
  Harness sleeping(true);
  sleeping.c.enable_sleep_support();
  sleeping.tick(); sleeping.power(true);
  sleeping.exchange({0xAA, 0x60, 1}, {0xAA, 0x60, 1, 0});
  sleeping.exchange({0xAA, 0x60, 5}, {0xAA, 0x60, 5, 0});
  sleeping.expect({0xAA, 0x11});
  sleeping.reply({0xAA, 0x11, 1, 30, 15, 7, 0, 255, 50, 0, 0, 0});
  const auto during_sleep = sleeping.sent.size(); sleeping.tick();
  CHECK(sleeping.sent.size() == during_sleep);
  CHECK(sleeping.c.dreamview().startup_pending());
  sleeping.c.refresh_sleep_status();
  sleeping.expect({0xAA, 0x11});
  sleeping.reply({0xAA, 0x11, 0, 30, 15, 0, 0, 255, 50, 0, 0, 0});
  sleeping.power(false); // Sleep expiry changed power after the initial boot status.
  sleeping.tick(); // Automatic operation now waits for durable invalidation.
  sleeping.confirm_disable();
  sleeping.clear_members();
  sleeping.expect_group(); sleeping.reply({0xA3, 0x60, 0});
  sleeping.power(true);
  sleeping.exchange({0xAA, 0x60, 6}, {0xAA, 0x60, 6, 1});
  sleeping.tick();
  CHECK(sleeping.sent.back().bytes[6] == 0x86 && sleeping.sent.back().bytes[7] == 0);
}

void group_encoding_limits() {
  auto all = MEMBERS;
  for (size_t i = 1; i < 5; ++i) { all[i] = all[0]; all[i][5] += i; }
  // Five fixed records cross frame boundaries; verify every address and padding byte.
  const std::vector<std::vector<uint8_t>> frames{
      pt_frame({0xA3, 0, 1, 5, 0x60, 5, 1, 0, 11, 0x9A, 0x78, 0x56, 0x34, 0x12, 2, 7, 1, 0, 11}),
      pt_frame({0xA3, 1, 0x9B, 0x78, 0x56, 0x34, 0x12, 2, 7, 1, 0, 11, 0x9C, 0x78, 0x56, 0x34, 0x12, 2, 7}),
      pt_frame({0xA3, 2, 1, 0, 11, 0x9D, 0x78, 0x56, 0x34, 0x12, 2, 7, 1, 0, 11, 0x9E, 0x78, 0x56, 0x34}),
      pt_frame({0xA3, 3, 0x12, 2, 7}),
      pt_frame({0xA3, 0xFF}),
  };
  std::vector<uint8_t> payload;
  for (const auto &frame : frames)
    payload.insert(payload.end(), frame.begin(), frame.end());
  check_packet(dreamview_group(all, 5), 0x83, payload);
  CHECK(dreamview_group(all, 0).size == 0 && dreamview_group(all, 6).size == 0);
}

int main() {
  const struct {
    const char *name;
    void (*run)();
  } cases[]{
      {"music_effect_preserves_group", music_effect_preserves_group},
      {"latest_queued_music_effect_wins", latest_queued_music_effect_wins},
      {"music_readback_at_startup", music_readback_at_startup},
      {"music_rejection_and_readback_mismatch", music_rejection_and_readback_mismatch},
      {"stop_before_music_write", stop_before_music_write},
      {"stop_survives_music_timeout", stop_survives_music_timeout},
      {"group_lifecycle", group_lifecycle},
      {"stale_uart_invalidates_connection", stale_uart_invalidates_connection},
      {"rejected_group_creation", rejected_group_creation},
      {"group_creation_retry_limit", group_creation_retry_limit},
      {"local_music_stops_synchronization", local_music_stops_synchronization},
      {"missing_member_connection_timeout", missing_member_connection_timeout},
      {"power_off_cancels_enable", power_off_cancels_enable},
      {"enable_readback_mismatch", enable_readback_mismatch},
      {"sleep_starts_after_confirmed_disable", sleep_starts_after_confirmed_disable},
      {"failed_disable_prevents_sleep", failed_disable_prevents_sleep},
      {"cancel_sleep_during_transition", cancel_sleep_during_transition},
      {"malformed_ack_is_ignored", malformed_ack_is_ignored},
      {"restart_preserves_native_session", restart_preserves_native_session},
      {"disconnected_member_clears_connection", disconnected_member_clears_connection},
      {"power_timeout_does_not_cancel_next_request", power_timeout_does_not_cancel_next_request},
      {"disable_queued_during_poll", disable_queued_during_poll},
      {"upload_rejects_foreign_ack", upload_rejects_foreign_ack},
      {"local_mode_retry_after_failed_disable", local_mode_retry_after_failed_disable},
      {"automatic_setup_restores_power", automatic_setup_restores_power},
      {"removed_member_replaces_native_list", removed_member_replaces_native_list},
      {"empty_list_removes_group", empty_list_removes_group},
      {"storage_failure_prevents_writes", storage_failure_prevents_writes},
      {"interrupted_setup_is_retried", interrupted_setup_is_retried},
      {"single_switch_action_recreates_group", single_switch_action_recreates_group},
      {"changed_address_replaces_group", changed_address_replaces_group},
      {"sleep_delays_setup_and_preserves_current_power", sleep_delays_setup_and_preserves_current_power},
      {"group_encoding_limits", group_encoding_limits},
  };
  for (const auto &test : cases) {
    current_test = test.name;
    test.run();
  }
  std::cout << std::size(cases) << " DreamView test cases passed\n";
}
