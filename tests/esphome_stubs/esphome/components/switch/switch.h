#pragma once

// Minimal ESPHome boundary for the standalone publication regression. These are
// framework semantics (raw-value deduplication, inversion, callbacks), not the
// DreamView policy under test. The real firmware is also compiled on the Builder.
namespace esphome::switch_ {
struct Deduplicator {
  bool valid{false}, last{false};
  bool next(bool value) {
    if (valid && last == value)
      return false;
    valid = true;
    last = value;
    return true;
  }
};
class Switch {
 public:
  virtual ~Switch() = default;
  bool has_state() const { return this->known_; }
  void set_has_state(bool known) { this->known_ = known; }
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  void publish_state(bool value) {
    if (!this->publish_dedup_.next(value))
      return;
    this->state = value != this->inverted_;
    this->known_ = true;
    ++this->callbacks;
  }
  bool state{false};
  unsigned callbacks{0};

 protected:
  virtual void write_state(bool state) = 0;
  Deduplicator publish_dedup_;
  bool known_{false}, inverted_{false};
};
}  // namespace esphome::switch_
