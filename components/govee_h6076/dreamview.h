#pragma once

// Included by protocol.h after the UART packet encoder.
namespace esphome::govee_h6076::protocol {

using DreamViewMembers = std::array<std::array<uint8_t, 6>, 5>;

enum class DreamViewStoredState : uint8_t { GROUP_INVALID, GROUP_APPLIED, GROUP_REMOVED };
struct DreamViewStoredGroup {
  uint8_t version{1};
  DreamViewStoredState state{DreamViewStoredState::GROUP_INVALID};
  uint8_t count{0};
  DreamViewMembers members{};
};

inline Packet dreamview_command(uint8_t prefix, uint8_t subcommand, uint8_t value = 0) {
  uint8_t data[20]{prefix, 0x60, subcommand, value};
  for (size_t i = 0; i < 19; ++i)
    data[19] ^= data[i];
  return encode(0x83, data, sizeof(data));
}

inline Packet dreamview_group(const DreamViewMembers &members, size_t count) {
  if (count == 0 || count > members.size())
    return {};
  // H6076 profile: mark=1, MAC identity, command version 11, seven areas.
  // A3 header is version, frame count, type; its body starts with the member count.
  uint8_t body[54]{1, 0, 0x60, uint8_t(count)};
  for (size_t i = 0; i < count; ++i) {
    auto *record = body + 4 + i * 10;
    record[0] = 1;
    record[1] = 0;
    record[2] = 11;
    std::reverse_copy(members[i].begin(), members[i].end(), record + 3);
    record[9] = 7;
  }
  const size_t length = 4 + count * 10;
  // Keep a separate terminating frame, as in the qualified one-member transaction.
  const size_t count_frames = (length + 16) / 17 + 1;
  body[1] = count_frames;
  uint8_t frames[100]{};
  for (size_t i = 0; i < count_frames; ++i) {
    auto *frame = frames + i * 20;
    frame[0] = 0xA3;
    frame[1] = i + 1 == count_frames ? 0xFF : uint8_t(i);
    if (i * 17 < length)
      std::copy(body + i * 17, body + std::min(length, (i + 1) * 17), frame + 2);
    for (size_t j = 0; j < 19; ++j)
      frame[19] ^= frame[j];
  }
  return encode(0x83, frames, count_frames * 20);
}

// One transaction shares the Controller's UART command slot. No background writer,
// unbounded retry, allocation, or optimistic membership/connection confirmation.
class DreamView {
 public:
  enum class Operation : uint8_t { NONE, CREATE, ENABLE, DISABLE, REMOVE, MUSIC };
  enum class Step : uint8_t {
    IDLE, READ_ENABLED, READ_MEMBERS, DISABLE, VERIFY_DISABLED,
    UPLOAD, READ_CONFIG, READ_STORED_CONFIG, ENABLE, VERIFY_ENABLED, REMOVE, VERIFY_REMOVED,
    READ_MUSIC, WRITE_MUSIC, VERIFY_MUSIC, VERIFY_MUSIC_ENABLED
  };

  void set_members(const DreamViewMembers &members, size_t count, const DreamViewStoredGroup &stored = {}) {
    this->members_ = members;
    this->count_ = count;
    this->tracking_ = this->startup_pending_ = true;
    this->stored_ = stored;
    this->stored_matches_ = stored.version == 1 && stored.count == count &&
                            (stored.state == DreamViewStoredState::GROUP_REMOVED ||
                             (count != 0 && stored.state == DreamViewStoredState::GROUP_APPLIED));
    for (size_t i = 0; this->stored_matches_ && i < count; ++i)
      this->stored_matches_ = stored.members[i] == members[i];
    this->removed_ = this->stored_matches_ && stored.state == DreamViewStoredState::GROUP_REMOVED;
    this->step_ = Step::READ_ENABLED;
  }
  bool tracking() const { return this->tracking_; }
  void enable_music_control() { this->music_tracking_ = true; }
  bool music_known() const { return this->music_known_; }
  uint8_t music_effect() const { return this->music_[1]; }
  bool request_music_effect(uint8_t effect) {
    if (!this->music_tracking_ || effect >= 12 || !this->enabled() || !this->group_uploaded_ ||
        this->startup_pending_ || this->queued_operation_ == Operation::DISABLE ||
        this->queued_operation_ == Operation::REMOVE || this->queued_operation_ == Operation::CREATE ||
        this->operation_ == Operation::DISABLE || this->operation_ == Operation::REMOVE ||
        this->operation_ == Operation::CREATE)
      return false;
    this->queued_music_effect_ = effect;
    return this->request(Operation::MUSIC);
  }
  bool busy() const { return this->step_ != Step::IDLE || this->storage_pending_; }
  bool has_packet() const { return this->step_ != Step::IDLE && !this->storage_pending_; }
  bool startup_pending() const { return this->startup_pending_; }
  bool automatic_configuration() const { return this->automatic_configuration_; }
  bool storage_pending() const { return this->storage_pending_; }
  const DreamViewStoredGroup &stored_group() const { return this->stored_; }
  void storage_complete(bool success) {
    this->storage_pending_ = false;
    if (!success) {
      this->group_uploaded_ = this->stored_matches_ = false;
      this->fail();
    }
  }
  bool take_power_restore() {
    const bool restore = this->restore_power_;
    this->restore_power_ = false;
    return restore;
  }
  bool connection_known() const {
    return this->enabled_known_ &&
           (!this->enabled_ ||
            (this->group_uploaded_ && this->members_known_ &&
             (this->operation_ == Operation::NONE || this->operation_ == Operation::MUSIC) &&
             (this->queued_operation_ == Operation::NONE || this->queued_operation_ == Operation::MUSIC)));
  }
  bool failed() const { return this->error_; }
  bool take_command_failure() {
    const bool failed = this->command_failed_;
    this->command_failed_ = false;
    return failed;
  }
  bool enabled() const { return this->enabled_known_ && this->enabled_; }
  bool operation_pending() const {
    return this->operation_ != Operation::NONE || this->queued_operation_ != Operation::NONE;
  }
  bool enabled_known() const { return this->enabled_known_; }
  bool protects_static() const {
    return this->tracking_ &&
           (this->startup_pending_ || !this->enabled_known_ || this->enabled_ || this->operation_pending());
  }
  bool connected() const {
    if (!this->enabled_known_ || !this->enabled_ || !this->members_known_ || !this->group_uploaded_ ||
        (this->queued_operation_ != Operation::NONE && this->queued_operation_ != Operation::MUSIC))
      return false;
    for (size_t i = 0; i < this->count_; ++i)
      if (this->connections_[i] != 2)
        return false;
    return this->count_ != 0;
  }
  const char *status() const {
    if (this->error_)
      return "error";
    if (this->startup_pending_ || this->operation_ != Operation::NONE)
      return "pending";
    if (!this->enabled_known_)
      return "unknown";
    if (this->removed_)
      return "removed";
    if (!this->enabled_)
      return this->group_uploaded_ ? "configured" : "disabled";
    if (!this->group_uploaded_)
      return "unverified_group";
    if (!this->members_known_)
      return "unknown";
    return this->connected() ? "connected" : "connecting";
  }
  bool request(Operation operation) {
    if (!this->tracking_ || operation == Operation::NONE ||
        (this->count_ == 0 && (operation == Operation::ENABLE || operation == Operation::CREATE)))
      return false;
    if (operation == Operation::MUSIC && (!this->enabled() || !this->group_uploaded_))
      return false;
    if ((operation == Operation::DISABLE || operation == Operation::REMOVE) &&
        this->step_ == Step::WRITE_MUSIC && this->sent_step_ == Step::IDLE)
      this->finish_();
    if (operation == Operation::DISABLE && this->step_ == Step::ENABLE && this->sent_step_ == Step::IDLE)
      this->finish_();
    if (this->busy()) {
      this->queued_operation_ = operation;
      return true;
    }
    if (operation == Operation::MUSIC) {
      this->music_effect_target_ = this->queued_music_effect_;
      this->operation_ = operation;
      this->error_ = false;
      this->step_ = Step::READ_MUSIC;
      return true;
    }
    if (operation == Operation::ENABLE && !this->group_uploaded_) {
      this->queued_operation_ = Operation::ENABLE;
      operation = Operation::CREATE;
    }
    if (operation == Operation::CREATE || operation == Operation::REMOVE) {
      this->startup_pending_ = false;
      this->group_uploaded_ = false;
      // Persist an invalid receipt before modifying native membership. A reset
      // during the transaction must not adopt a partially replaced group.
      this->store_(DreamViewStoredState::GROUP_INVALID);
    }
    this->error_ = false;
    this->removed_ = false;
    this->members_known_ = false;
    this->operation_ = operation;
    this->step_ = operation == Operation::ENABLE ? Step::ENABLE : Step::DISABLE;
    return true;
  }
  void service(uint32_t now, bool link, bool can_configure = true) {
    if (!this->tracking_)
      return;
    if (!link && this->operation_pending())
      this->fail();
    if (!link || (this->enabled_known_ && uint32_t(now - this->last_read_) >= LINK_TIMEOUT_MS)) {
      this->enabled_known_ = this->members_known_ = false;
      this->music_known_ = false;
    }
    if (!this->busy() && link && this->queued_operation_ != Operation::NONE) {
      const auto operation = this->queued_operation_;
      this->queued_operation_ = Operation::NONE;
      this->request(operation);
    }
    if (!this->busy() && link && this->startup_pending_) {
      const bool kept_removed = this->removed_ && !this->enabled_ && this->members_known_ &&
                                std::all_of(this->connections_.begin(), this->connections_.end(),
                                            [](uint8_t status) { return status == 0; });
      if (this->stored_matches_ && (this->group_uploaded_ || kept_removed)) {
        this->startup_pending_ = false;
      } else if (can_configure && this->enabled_known_ && this->members_known_) {
        this->request(this->count_ == 0 ? Operation::REMOVE : Operation::CREATE);
        this->automatic_configuration_ = true;
      }
    }
    if (!this->busy() && link && uint32_t(now - this->last_poll_) >= POLL_INTERVAL_MS) {
      this->last_poll_ = now;
      this->step_ = Step::READ_ENABLED;
    }
    if (this->waiting_for_members_ && uint32_t(now - this->started_at_) >= 30000) {
      this->waiting_for_members_ = false;
      this->error_ = true;
    }
  }
  Packet packet(uint32_t now) {
    this->sent_step_ = this->step_;
    if (this->automatic_configuration_ &&
        (this->step_ == Step::DISABLE || this->step_ == Step::REMOVE || this->step_ == Step::UPLOAD))
      this->automatic_write_sent_ = true;
    switch (this->step_) {
      case Step::UPLOAD:
        return dreamview_group(this->members_, this->count_);
      case Step::DISABLE:
        return dreamview_command(0x33, 1, 0);
      case Step::ENABLE:
        this->started_at_ = now;
        this->waiting_for_members_ = true;
        return dreamview_command(0x33, 1, 1);
      case Step::REMOVE:
        return dreamview_command(0x33, 7, 0xFF);
      case Step::WRITE_MUSIC: {
        uint8_t data[20]{0x33, 0x60, 2};
        std::copy(this->music_target_.begin(), this->music_target_.end(), data + 3);
        for (size_t i = 0; i < 19; ++i)
          data[19] ^= data[i];
        return encode(0x83, data, sizeof(data));
      }
      case Step::READ_MUSIC:
      case Step::VERIFY_MUSIC:
        return dreamview_command(0xAA, 2);
      case Step::READ_CONFIG:
      case Step::READ_STORED_CONFIG:
        return dreamview_command(0xAA, 6);
      case Step::READ_MEMBERS:
      case Step::VERIFY_REMOVED:
        return dreamview_command(0xAA, 5);
      default:
        return dreamview_command(0xAA, 1);
    }
  }
  // Caller validates the entire ptReal batch before dispatching individual frames.
  bool receive(const uint8_t *data, uint32_t now) {
    const auto step = this->sent_step_;
    if (step == Step::IDLE)
      return false;
    if (step == Step::WRITE_MUSIC) {
      if (data[0] != 0x33 || data[1] != 0x60 || data[2] != 2)
        return false;
      if (data[3] != 0)
        this->fail();
      else
        this->step_ = Step::VERIFY_MUSIC;
    } else if (step == Step::READ_MUSIC || step == Step::VERIFY_MUSIC) {
      if (data[0] != 0xAA || data[1] != 0x60 || data[2] != 2 || data[3] < 1 || data[3] > 3 ||
          data[7] > 100 || (data[3] == 1 && data[6] > 1))
        return false;
      std::copy(data + 3, data + 8, this->music_.begin());
      this->music_known_ = this->music_[0] == 1 && this->music_[1] < 12 && this->music_[2] == 0;
      if (step == Step::VERIFY_MUSIC) {
        if (this->music_ != this->music_target_)
          this->fail();
        else
          this->step_ = Step::VERIFY_MUSIC_ENABLED;
      } else if (this->operation_ == Operation::MUSIC &&
                 (this->queued_operation_ == Operation::NONE || this->queued_operation_ == Operation::MUSIC)) {
        // Native type 1 has twelve group programs, separate from the lamp's
        // ordinary music modes. Preserve softness and microphone sensitivity.
        this->music_target_ = {1, this->music_effect_target_, 0,
                               uint8_t(this->music_[0] == 1 ? this->music_[3] : 0), this->music_[4]};
        this->step_ = Step::WRITE_MUSIC;
      } else {
        this->finish_();
      }
    } else if (step == Step::UPLOAD) {
      if (data[0] != 0xA3 || data[1] != 0x60)
        return false;
      if (data[2] != 0) {
        this->fail();
      } else {
        this->step_ = Step::READ_CONFIG;
      }
    } else if (step == Step::DISABLE || step == Step::ENABLE || step == Step::REMOVE) {
      if (data[0] != 0x33 || data[1] != 0x60 || data[2] != (step == Step::REMOVE ? 7 : 1))
        return false;
      if (data[3] != 0) {
        this->fail();
      } else {
        this->step_ = step == Step::REMOVE ? Step::VERIFY_REMOVED
                     : step == Step::ENABLE ? Step::VERIFY_ENABLED : Step::VERIFY_DISABLED;
      }
    } else {
      const uint8_t sub = (step == Step::READ_CONFIG || step == Step::READ_STORED_CONFIG) ? 6
                          : (step == Step::READ_MEMBERS || step == Step::VERIFY_REMOVED) ? 5 : 1;
      if (data[0] != 0xAA || data[1] != 0x60 || data[2] != sub)
        return false;
      if (sub == 6) {
        if (step == Step::READ_STORED_CONFIG) {
          if (data[3] > 1)
            return false;
          // The flag alone proves no membership. Adoption additionally requires
          // an exact local receipt for this configured list and profile version.
          this->group_uploaded_ = data[3] == 1;
          this->step_ = Step::READ_MEMBERS;
        } else if (data[3] != 1) {
          this->fail();
        } else {
          // Configuration flag plus the matching upload ACK, never a member count.
          this->group_uploaded_ = true;
          this->store_(DreamViewStoredState::GROUP_APPLIED);
          this->finish_();
        }
      } else if (sub == 5) {
        for (size_t i = 0; i < 5; ++i)
          if (data[3 + i] > 2)
            return false;
        std::copy(data + 3, data + 8, this->connections_.begin());
        this->members_known_ = true;
        if (step == Step::VERIFY_REMOVED) {
          if (std::any_of(this->connections_.begin(), this->connections_.end(), [](uint8_t v) { return v != 0; })) {
            return false;  // Let the existing bounded query retry wait for disconnection.
          } else {
            this->group_uploaded_ = false;
            if (this->operation_ == Operation::CREATE) {
              this->step_ = Step::UPLOAD;
            } else {
              this->removed_ = true;
              this->store_(DreamViewStoredState::GROUP_REMOVED);
              this->finish_();
            }
          }
        } else {
          if (this->connected()) {
            this->waiting_for_members_ = false;
            this->error_ = false;
          }
          if (this->music_tracking_ &&
              (this->operation_ == Operation::NONE || this->operation_ == Operation::ENABLE))
            this->step_ = Step::READ_MUSIC;
          else
            this->finish_();
        }
      } else {
        if (data[3] > 1 || data[4] > 1)
          return false;
        this->enabled_ = data[3] != 0;
        this->enabled_known_ = true;
        this->last_read_ = now;
        if (step == Step::VERIFY_DISABLED) {
          if (this->enabled_) {
            this->fail();
          } else {
            this->waiting_for_members_ = false;
            this->step_ = (this->operation_ == Operation::CREATE || this->operation_ == Operation::REMOVE)
                              ? Step::REMOVE : Step::IDLE;
            if (this->step_ == Step::IDLE)
              this->finish_();
          }
        } else if ((step == Step::VERIFY_ENABLED || step == Step::VERIFY_MUSIC_ENABLED) &&
                   (!this->enabled_ || data[4] != 0)) {
          this->fail();
        } else {
          this->step_ = this->startup_pending_ && this->stored_matches_ &&
                                this->stored_.state == DreamViewStoredState::GROUP_APPLIED
                            ? Step::READ_STORED_CONFIG : Step::READ_MEMBERS;
        }
      }
    }
    this->sent_step_ = Step::IDLE;
    return true;
  }
  void fail() {
    const auto queued = this->queued_operation_;
    const bool keep_stop = this->operation_ == Operation::MUSIC &&
                           (queued == Operation::DISABLE || queued == Operation::REMOVE);
    if (this->operation_ == Operation::CREATE || this->operation_ == Operation::REMOVE)
      this->group_uploaded_ = false;
    this->enabled_known_ = this->members_known_ = false;
    this->music_known_ = false;
    this->startup_pending_ = this->storage_pending_ = false;
    this->waiting_for_members_ = false;
    this->queued_operation_ = Operation::NONE;
    this->error_ = true;
    this->command_failed_ = true;
    this->finish_();
    if (keep_stop)
      this->queued_operation_ = queued;
  }

 protected:
  void store_(DreamViewStoredState state) {
    this->stored_ = {1, state, uint8_t(this->count_), this->members_};
    this->storage_pending_ = true;
  }
  void finish_() {
    if (this->automatic_configuration_) {
      this->restore_power_ = this->automatic_write_sent_;
      this->automatic_configuration_ = this->automatic_write_sent_ = false;
    }
    this->step_ = this->sent_step_ = Step::IDLE;
    this->operation_ = Operation::NONE;
  }
  DreamViewStoredGroup stored_{};
  std::array<uint8_t, 5> music_{}, music_target_{};
  bool music_tracking_{false}, music_known_{false};
  uint8_t music_effect_target_{0}, queued_music_effect_{0};
  DreamViewMembers members_{};
  bool stored_matches_{false}, startup_pending_{false}, storage_pending_{false};
  bool automatic_configuration_{false}, automatic_write_sent_{false}, restore_power_{false};
  std::array<uint8_t, 5> connections_{};
  size_t count_{0};
  Operation operation_{Operation::NONE}, queued_operation_{Operation::NONE};
  Step step_{Step::IDLE}, sent_step_{Step::IDLE};
  bool tracking_{false}, enabled_known_{false}, enabled_{false}, members_known_{false};
  bool group_uploaded_{false}, removed_{false}, error_{false}, waiting_for_members_{false};
  bool command_failed_{false};
  uint32_t last_read_{0}, last_poll_{0}, started_at_{0};
};

}  // namespace esphome::govee_h6076::protocol
