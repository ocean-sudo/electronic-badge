#pragma once

#include <algorithm>
#include <stdint.h>
#include <stddef.h>
#include <vector>

// Stable logical IDs, not directory indices. Preparing never consumes a round,
// history, or random state; both bag banks reserve capacity on membership changes.
class PicturePlayer {
 public:
  void reset(bool shuffle, int current, const int32_t* ids, size_t count, uint32_t seed) {
    shuffle_ = shuffle;
    random_ = seed;
    if (count) ids_.assign(ids, ids + count); else ids_.clear();
    seen_.assign(count, 0);
    bags_[0].clear(); bags_[1].clear();
    bags_[0].reserve(count); bags_[1].reserve(count);
    activeBag_ = 0;
    bagReady_ = false;
    pending_ = Pending::None;
    historyHead_ = historyCount_ = 0;
    cursor_ = -1;
    cursorGap_ = false;
    const size_t index = find(current);
    if (index < ids_.size()) { seen_[index] = 1; append(current); }
  }

  bool shuffle() const { return shuffle_; }

  void setShuffle(bool shuffle, int current) {
    if (shuffle_ == shuffle) return;
    shuffle_ = shuffle;
    pending_ = Pending::None;
    bagReady_ = false;
    bags_[0].clear(); bags_[1].clear();
    std::fill(seen_.begin(), seen_.end(), 0);
    const size_t index = find(current);
    if (index < ids_.size()) seen_[index] = 1;
  }

  void membership(const int32_t* ids, size_t count) {
    if (count == ids_.size() && std::equal(ids_.begin(), ids_.end(), ids)) return;
    std::vector<uint8_t> updated(count, 0);
    for (size_t index = 0; index < count; ++index) {
      const size_t old = find(ids[index]);
      if (old < ids_.size()) updated[index] = seen_[old];
    }
    if (count) ids_.assign(ids, ids + count); else ids_.clear();
    seen_.swap(updated);
    bags_[0].reserve(count); bags_[1].reserve(count);
    trimHistory();
    pending_ = Pending::None;
    bagReady_ = false;
    bags_[0].clear(); bags_[1].clear();
  }

  // Replacing a file under an existing ID must not replay the old image's history.
  void invalidate(int slot) {
    for (int index = 0; index < historyCount_; ++index)
      if (history(index) == slot) history(index) = -1;
    trimHistory();
    const size_t index = find(slot);
    if (index < ids_.size()) seen_[index] = 0;
    pending_ = Pending::None;
    bagReady_ = false;
  }

  int prepareNext(int current) {
    if (pending_ != Pending::None && pendingNext_ && pendingCurrent_ == current) return pendingSlot_;
    pending_ = Pending::None;
    for (int index = cursor_ + 1; index < historyCount_; ++index)
      if (available(history(index))) return prepareHistory(index, true, current);
    if (!shuffle_) {
      auto next = std::upper_bound(ids_.begin(), ids_.end(), current);
      if (next == ids_.end()) next = ids_.begin();
      if (next == ids_.end() || *next == current) return -1;
      return prepare(Pending::Append, *next, true, current);
    }
    if (ids_.empty() || (ids_.size() == 1 && available(current))) return -1;
    auto& active = bags_[activeBag_];
    if (bagReady_ && !active.empty()) return prepare(Pending::Append, active.back(), true, current);
    auto& bag = bags_[activeBag_ ^ 1];
    bag.clear();
    for (size_t index = 0; index < ids_.size(); ++index)
      if (!seen_[index]) bag.push_back(ids_[index]);
    pendingNewRound_ = bag.empty();
    if (pendingNewRound_) bag.assign(ids_.begin(), ids_.end());
    pendingRandom_ = random_;
    for (size_t remaining = bag.size(); remaining > 1; --remaining)
      std::swap(bag[remaining - 1], bag[bounded(pendingRandom_, remaining)]);
    if (bag.size() > 1 && bag.back() == current)
      std::swap(bag.back(), bag[bounded(pendingRandom_, bag.size() - 1)]);
    return prepare(Pending::NewBag, bag.back(), true, current);
  }

  int preparePrevious() {
    if (pending_ != Pending::None && !pendingNext_) return pendingSlot_;
    pending_ = Pending::None;
    for (int index = cursor_ - (cursorGap_ ? 0 : 1); index >= 0; --index)
      if (available(history(index))) return prepareHistory(index, false, -1);
    return -1;
  }

  void commit(int slot) {
    if (pending_ == Pending::None || pendingSlot_ != slot || !available(slot)) return;
    const Pending operation = pending_;
    pending_ = Pending::None;
    if (operation == Pending::NewBag) {
      activeBag_ ^= 1;
      bagReady_ = true;
      random_ = pendingRandom_;
      if (pendingNewRound_) std::fill(seen_.begin(), seen_.end(), 0);
    }
    if (operation == Pending::History) { cursor_ = pendingCursor_; cursorGap_ = false; }
    else { append(slot); consume(slot); }
  }

  void commitManual(int slot) {
    if (!available(slot)) return;
    pending_ = Pending::None;
    if (!cursorGap_ && cursor_ >= 0 && history(cursor_) == slot) historyCount_ = cursor_ + 1;
    else append(slot);
    consume(slot);
  }

  bool previousAvailable() const {
    for (int index = cursor_ - (cursorGap_ ? 0 : 1); index >= 0; --index)
      if (available(history(index))) return true;
    return false;
  }

 private:
  enum class Pending : uint8_t { None, Append, History, NewBag };
  bool shuffle_ = false;
  std::vector<int32_t> ids_;
  std::vector<uint8_t> seen_;
  std::vector<int32_t> bags_[2];
  uint8_t activeBag_ = 0;
  bool bagReady_ = false;
  uint32_t random_ = 0;
  int32_t history_[6] = {};
  uint8_t historyHead_ = 0, historyCount_ = 0;
  int8_t cursor_ = -1;
  bool cursorGap_ = false;
  Pending pending_ = Pending::None;
  bool pendingNext_ = false;
  int32_t pendingCurrent_ = -1, pendingSlot_ = -1;
  int8_t pendingCursor_ = -1;
  bool pendingNewRound_ = false;
  uint32_t pendingRandom_ = 0;

  size_t find(int slot) const {
    auto entry = std::lower_bound(ids_.begin(), ids_.end(), slot);
    return entry != ids_.end() && *entry == slot ? entry - ids_.begin() : ids_.size();
  }
  bool available(int slot) const { return slot >= 0 && find(slot) < ids_.size(); }
  int32_t& history(int index) { return history_[(historyHead_ + index) % 6]; }
  int history(int index) const { return history_[(historyHead_ + index) % 6]; }
  void trimHistory() {
    int retained = 0, prior = 0, selected = -1;
    for (int index = 0; index < historyCount_; ++index) {
      const int slot = history(index);
      if (!available(slot)) continue;
      if (index < cursor_ || (cursorGap_ && index == cursor_)) ++prior;
      if (!cursorGap_ && index == cursor_) selected = retained;
      history(retained++) = slot;
    }
    historyCount_ = retained;
    cursorGap_ = selected < 0;
    cursor_ = cursorGap_ ? prior - 1 : selected;
  }
  static uint32_t bounded(uint32_t& state, uint32_t bound) {
    const uint32_t threshold = (uint32_t(0) - bound) % bound;
    uint32_t value;
    do { state = state * 1664525U + 1013904223U; value = state; } while (value < threshold);
    return value % bound;
  }
  int prepare(Pending operation, int slot, bool next, int current) {
    pending_ = operation; pendingNext_ = next; pendingCurrent_ = current; pendingSlot_ = slot;
    return slot;
  }
  int prepareHistory(int index, bool next, int current) {
    pendingCursor_ = index;
    return prepare(Pending::History, history(index), next, current);
  }
  void append(int slot) {
    historyCount_ = cursor_ + 1;
    if (historyCount_ == 6) { historyHead_ = (historyHead_ + 1) % 6; --historyCount_; }
    history(historyCount_) = slot;
    ++historyCount_; cursor_ = historyCount_ - 1; cursorGap_ = false;
  }
  void consume(int slot) {
    const size_t index = find(slot);
    if (index == ids_.size() || seen_[index]) return;
    seen_[index] = 1;
    if (!bagReady_) return;
    auto& bag = bags_[activeBag_];
    if (!bag.empty() && bag.back() == slot) { bag.pop_back(); return; }
    auto entry = std::find(bag.begin(), bag.end(), slot);
    if (entry != bag.end()) bag.erase(entry);
  }
};
