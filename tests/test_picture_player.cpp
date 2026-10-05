#include "picture_player.h"

#include <cassert>
#include <initializer_list>
#include <iostream>
#include <set>
#include <vector>

namespace {
std::vector<int32_t> idsFrom(const bool (&occupied)[50]) {
  std::vector<int32_t> ids;
  for (int slot = 0; slot < 50; ++slot) if (occupied[slot]) ids.push_back(slot);
  return ids;
}
void resetPlayer(PicturePlayer& player, bool shuffle, int current, const bool (&occupied)[50], uint32_t seed) {
  const auto ids = idsFrom(occupied);
  player.reset(shuffle, current, ids.data(), ids.size(), seed);
}
void updateMembership(PicturePlayer& player, const bool (&occupied)[50]) {
  const auto ids = idsFrom(occupied);
  player.membership(ids.data(), ids.size());
}
int next(PicturePlayer& player, int& current) {
  const int candidate = player.prepareNext(current);
  assert(candidate >= 0 && candidate < INT32_MAX);
  assert(player.prepareNext(current) == candidate);
  player.commit(candidate);
  current = candidate;
  return candidate;
}

int previous(PicturePlayer& player, int& current) {
  assert(player.previousAvailable());
  const int candidate = player.preparePrevious();
  assert(candidate >= 0 && candidate < INT32_MAX);
  assert(player.preparePrevious() == candidate);
  player.commit(candidate);
  current = candidate;
  return candidate;
}

void shuffleRoundsAndAllFiftySlots() {
  bool occupied[50];
  for (int slot = 0; slot < 50; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 27;
  resetPlayer(player, true, current, occupied, 0x12345678U);
  assert(player.shuffle());
  assert(!player.previousAvailable());
  bool firstSeen[50] = {};
  firstSeen[current] = true;
  for (int step = 0; step < 49; ++step) {
    const int before = current;
    const int slot = next(player, current);
    assert(slot != before && !firstSeen[slot]);
    firstSeen[slot] = true;
  }
  for (bool seen : firstSeen) assert(seen);
  for (int round = 0; round < 8; ++round) {
    bool seen[50] = {};
    for (int step = 0; step < 50; ++step) {
      const int before = current;
      const int slot = next(player, current);
      assert(slot != before && !seen[slot]);
      seen[slot] = true;
    }
    for (bool visited : seen) assert(visited);
  }

  // Two-member rounds must not repeat at the boundary, either.
  for (int slot = 0; slot < 50; ++slot) occupied[slot] = slot == 4 || slot == 49;
  current = 49;
  resetPlayer(player, true, current, occupied, 0);
  for (int step = 0; step < 100; ++step)
    assert(next(player, current) == (step % 2 == 0 ? 4 : 49));
}

void sequentialHistoryCapacityReplayAndBranch() {
  bool occupied[50];
  for (int slot = 0; slot < 50; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 0;
  resetPlayer(player, false, current, occupied, 9);
  for (int expected = 1; expected <= 9; ++expected)
    assert(next(player, current) == expected);
  // Exactly five prior displays survive in the six-entry history.
  for (int expected = 8; expected >= 4; --expected)
    assert(previous(player, current) == expected);
  assert(!player.previousAvailable());
  assert(player.preparePrevious() == -1);
  player.commit(-1);
  for (int expected = 5; expected <= 9; ++expected)
    assert(next(player, current) == expected);
  assert(next(player, current) == 10);
  assert(previous(player, current) == 9);
  assert(previous(player, current) == 8);
  // SHOW branches at the cursor rather than replaying the old forward tail.
  player.commitManual(42);
  current = 42;
  assert(next(player, current) == 43);
  assert(previous(player, current) == 42);
  assert(previous(player, current) == 8);
  assert(next(player, current) == 42);
  assert(next(player, current) == 43);

  resetPlayer(player, false, 49, occupied, 9);
  current = 49;
  assert(next(player, current) == 0);
}

void shuffleHistoryReplaysBeforeFreshBag() {
  bool occupied[50] = {};
  for (int slot = 0; slot < 12; ++slot) occupied[slot] = true;
  PicturePlayer player, uninterrupted;
  int current = 0, other = 0;
  resetPlayer(player, true, current, occupied, 123);
  resetPlayer(uninterrupted, true, other, occupied, 123);
  int actual[6] = {0};
  for (int index = 1; index < 6; ++index) {
    actual[index] = next(player, current);
    assert(next(uninterrupted, other) == actual[index]);
  }
  assert(previous(player, current) == actual[4]);
  assert(previous(player, current) == actual[3]);
  assert(next(player, current) == actual[4]);
  assert(next(player, current) == actual[5]);
  for (int step = 0; step < 60; ++step)
    assert(next(player, current) == next(uninterrupted, other));
}

void manualConsumesUnseenWithoutResettingRound() {
  bool occupied[50] = {};
  for (int slot = 0; slot < 5; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 0;
  resetPlayer(player, true, current, occupied, 71);
  const int automatic = next(player, current);
  // Selecting the currently prepared slot manually consumes exactly that entry.
  const int manual = player.prepareNext(current);
  assert(manual != automatic && manual != 0);
  player.commitManual(manual);
  current = manual;
  player.commitManual(manual); // An explicit seen repeat must not reset the bag.
  bool seen[50] = {};
  seen[0] = seen[automatic] = seen[manual] = true;
  for (int step = 0; step < 2; ++step) {
    const int slot = next(player, current);
    assert(!seen[slot]);
    seen[slot] = true;
  }
  for (int slot = 0; slot < 5; ++slot) assert(seen[slot]);
  bool round[50] = {};
  for (int step = 0; step < 5; ++step) {
    const int before = current;
    const int slot = next(player, current);
    assert(slot != before && !round[slot]);
    round[slot] = true;
  }

  // A manual upload can also consume an unseen entry before a bag is prepared.
  resetPlayer(player, true, 0, occupied, 71);
  player.commitManual(4);
  current = 4;
  bool uploadedRound[50] = {};
  uploadedRound[0] = uploadedRound[4] = true;
  for (int step = 0; step < 3; ++step) {
    const int slot = next(player, current);
    assert(!uploadedRound[slot]);
    uploadedRound[slot] = true;
  }
}

void membershipPreservesSurvivorsAndAddsUnseen() {
  bool occupied[50] = {};
  for (int slot = 0; slot < 5; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 0;
  resetPlayer(player, true, current, occupied, 314);
  const int visited = next(player, current);
  const int pending = player.prepareNext(current);
  // Same-ID replacement does not invalidate history, visits, or a prepared next.
  updateMembership(player, occupied);
  assert(player.prepareNext(current) == pending);
  assert(previous(player, current) == 0);
  assert(next(player, current) == visited);
  occupied[0] = false;
  occupied[49] = true;
  updateMembership(player, occupied);
  bool remaining[50] = {};
  int expectedCount = 0;
  for (int slot = 0; slot < 50; ++slot) {
    remaining[slot] = occupied[slot] && slot != visited;
    expectedCount += remaining[slot];
  }
  for (int step = 0; step < expectedCount; ++step) {
    const int slot = next(player, current);
    assert(remaining[slot]);
    remaining[slot] = false;
  }
  for (bool unseen : remaining) assert(!unseen);

  // Newly occupied slots extend an exhausted round before survivors repeat.
  occupied[48] = true;
  updateMembership(player, occupied);
  assert(next(player, current) == 48);
  occupied[48] = false;
  updateMembership(player, occupied);
  for (int step = 0; step < 30; ++step) {
    const int before = current;
    const int slot = next(player, current);
    assert(occupied[slot] && slot != before);
  }
}

void deletedHistoryIsSkippedAndDoesNotRevive() {
  bool occupied[50] = {};
  for (int slot = 0; slot < 10; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 0;
  resetPlayer(player, false, current, occupied, 0);
  for (int expected = 1; expected <= 5; ++expected)
    assert(next(player, current) == expected);
  occupied[3] = false;
  updateMembership(player, occupied);
  assert(previous(player, current) == 4);
  assert(previous(player, current) == 2);
  occupied[3] = true; // A new occupant must not revive the deleted history entry.
  updateMembership(player, occupied);
  assert(previous(player, current) == 1);
  assert(previous(player, current) == 0);
  assert(!player.previousAvailable());
  for (int expected : {1, 2, 4, 5}) assert(next(player, current) == expected);
  occupied[5] = false; // Deleting the current history entry still allows Previous.
  updateMembership(player, occupied);
  assert(previous(player, current) == 4);
  assert(next(player, current) == 6);
}

void modeChangePreservesHistoryAndRestartsRound() {
  bool occupied[50] = {};
  for (int slot = 0; slot < 5; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 0;
  resetPlayer(player, false, current, occupied, 401);
  for (int expected = 1; expected <= 3; ++expected)
    assert(next(player, current) == expected);
  assert(previous(player, current) == 2);
  player.setShuffle(true, current);
  assert(player.shuffle() && player.previousAvailable());
  assert(next(player, current) == 3); // Existing forward history takes precedence.
  bool seen[50] = {};
  seen[2] = true; // Forward replay is history-only, not a new-round visit.
  for (int step = 0; step < 4; ++step) {
    const int slot = next(player, current);
    assert(!seen[slot]);
    seen[slot] = true;
    player.setShuffle(true, current); // No-op mode updates keep the round.
  }
  assert(player.previousAvailable());
  const int atTail = current;
  const int prior = previous(player, current);
  player.setShuffle(false, current);
  assert(!player.shuffle());
  assert(next(player, current) == atTail);
  assert(previous(player, current) == prior);
}

void zeroAndOneDoNotLoop() {
  for (bool shuffle : {false, true}) {
    bool occupied[50] = {};
    PicturePlayer player;
    int current = -1;
    resetPlayer(player, shuffle, current, occupied, 0);
    assert(player.prepareNext(current) == -1);
    assert(player.preparePrevious() == -1 && !player.previousAvailable());
    occupied[49] = true;
    updateMembership(player, occupied);
    assert(next(player, current) == 49);
    assert(player.prepareNext(current) == -1);
    assert(!player.previousAvailable());
    resetPlayer(player, shuffle, current, occupied, 0);
    assert(player.prepareNext(current) == -1);
    occupied[49] = false;
    updateMembership(player, occupied);
    assert(player.prepareNext(current) == -1);
    assert(!player.previousAvailable());
  }
}

void failedPreparationDoesNotConsumeRandomBagOrHistory() {
  bool occupied[50] = {};
  for (int slot = 0; slot < 8; ++slot) occupied[slot] = true;
  PicturePlayer failed, clean;
  int current = 0, other = 0;
  resetPlayer(failed, true, current, occupied, 0);
  resetPlayer(clean, true, other, occupied, 0);
  for (int step = 0; step < 160; ++step) {
    const int candidate = failed.prepareNext(current);
    for (int attempt = 0; attempt < 6; ++attempt)
      assert(failed.prepareNext(current) == candidate);
    // A commit without the successfully prepared image must do nothing.
    failed.commit((candidate + 1) % 8);
    assert(failed.prepareNext(current) == candidate);
    if (failed.previousAvailable()) {
      const int back = failed.preparePrevious();
      assert(back >= 0);
      assert(failed.preparePrevious() == back);
      // Failed Previous cancels the candidate but consumes neither cursor nor RNG.
    } else {
      assert(failed.preparePrevious() == -1);
    }
    assert(failed.prepareNext(current) == candidate);
    assert(candidate == clean.prepareNext(other));
    // Repeated same-mode settings must retain even a pending fresh full round.
    for (int update = 0; update < 3; ++update)
      failed.setShuffle(true, current);
    failed.commit(candidate);
    clean.commit(candidate);
    current = other = candidate;
  }

  // Abandoning a freshly prepared bag before a manual selection cannot advance
  // the seed or publish the speculative round.
  resetPlayer(failed, true, 0, occupied, 55);
  resetPlayer(clean, true, 0, occupied, 55);
  assert(failed.prepareNext(0) >= 0);
  failed.commitManual(7);
  clean.commitManual(7);
  current = other = 7;
  for (int step = 0; step < 32; ++step)
    assert(next(failed, current) == next(clean, other));

  // Membership invalidates speculative candidates without committing their RNG.
  resetPlayer(failed, true, 0, occupied, 99);
  resetPlayer(clean, true, 0, occupied, 99);
  const int removed = failed.prepareNext(0);
  occupied[removed] = false;
  occupied[49] = true;
  updateMembership(failed, occupied);
  updateMembership(clean, occupied);
  failed.commit(removed);
  current = other = 0;
  for (int step = 0; step < 32; ++step)
    assert(next(failed, current) == next(clean, other));
}

void seedIsInjectableAndResetIsDeterministic() {
  bool occupied[50];
  for (int slot = 0; slot < 50; ++slot) occupied[slot] = true;
  PicturePlayer a, b, different;
  int first = 17, second = 17, third = 17;
  resetPlayer(a, true, first, occupied, 98765);
  resetPlayer(b, true, second, occupied, 98765);
  resetPlayer(different, true, third, occupied, 98766);
  bool differs = false;
  int recorded[80];
  for (int step = 0; step < 80; ++step) {
    recorded[step] = next(a, first);
    assert(next(b, second) == recorded[step]);
    differs |= next(different, third) != recorded[step];
  }
  assert(differs);
  resetPlayer(a, true, 17, occupied, 98765);
  first = 17;
  for (int step = 0; step < 80; ++step) assert(next(a, first) == recorded[step]);
}

void crossRoundHistoryDoesNotConsumeTheFreshBag() {
  bool occupied[50] = {};
  for (int slot = 0; slot < 10; ++slot) occupied[slot] = true;
  PicturePlayer rewound, uninterrupted;
  int current = 0, other = 0;
  resetPlayer(rewound, true, current, occupied, 2468);
  resetPlayer(uninterrupted, true, other, occupied, 2468);
  int actual[11] = {0};
  // Exhaust the initial round, then commit the first entry of the new round.
  for (int index = 1; index <= 10; ++index) {
    actual[index] = next(rewound, current);
    assert(next(uninterrupted, other) == actual[index]);
  }
  for (int index = 9; index >= 5; --index)
    assert(previous(rewound, current) == actual[index]);
  for (int index = 6; index <= 10; ++index)
    assert(next(rewound, current) == actual[index]);
  // Old-round history entries are unseen in this round. Replaying them must
  // leave all nine remaining entries, and the following full round, untouched.
  for (int step = 0; step < 19; ++step)
    assert(next(rewound, current) == next(uninterrupted, other));
}

void deletedHistoryCapacityIsReclaimed() {
  bool occupied[50] = {};
  for (int slot = 0; slot <= 8; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 0;
  resetPlayer(player, false, current, occupied, 0);
  for (int expected = 1; expected <= 8; ++expected)
    assert(next(player, current) == expected);
  // The wrapped ring contains 3,4,5,6,7,8. Compact once, retaining 3,5,7,8.
  occupied[4] = occupied[6] = false;
  occupied[10] = occupied[11] = true;
  updateMembership(player, occupied);
  player.commitManual(10);
  player.commitManual(11);
  current = 11;
  for (int expected : {10, 8, 7, 5, 3})
    assert(previous(player, current) == expected);
  assert(!player.previousAvailable());
  assert(player.preparePrevious() == -1);
}

void showingCurrentBranchesWithoutDuplicateHistory() {
  bool occupied[50];
  for (int slot = 0; slot < 50; ++slot) occupied[slot] = true;
  PicturePlayer player;
  int current = 0;
  resetPlayer(player, false, current, occupied, 0);
  for (int expected = 1; expected <= 5; ++expected)
    assert(next(player, current) == expected);
  for (int repeat = 0; repeat < 8; ++repeat) player.commitManual(current);
  for (int expected = 4; expected >= 0; --expected)
    assert(previous(player, current) == expected);
  assert(!player.previousAvailable());

  resetPlayer(player, false, 0, occupied, 0);
  for (int selected : {10, 20, 30}) player.commitManual(selected);
  current = 30;
  assert(previous(player, current) == 20);
  player.commitManual(current); // Drop 30, but do not duplicate 20.
  assert(previous(player, current) == 10);
  assert(previous(player, current) == 0);
  assert(next(player, current) == 10);
  assert(next(player, current) == 20);
  assert(next(player, current) == 21); // No obsolete forward entry 30.
  player.commitManual(10); // A nonconsecutive explicit repeat is a real branch.
  current = 10;
  assert(previous(player, current) == 21);
  assert(next(player, current) == 10);
}
void sparseIdsAndUnboundedRounds() {
  std::vector<int32_t> ids{0, 128, 256, 65537, INT32_MAX - 1};
  PicturePlayer player, clean;
  int current = 0, other = 0;
  player.reset(false, current, ids.data(), ids.size(), 1);
  for (int expected : {128, 256, 65537, INT32_MAX - 1, 0}) assert(next(player, current) == expected);
  assert(previous(player, current) == INT32_MAX - 1);
  assert(previous(player, current) == 65537);
  ids.erase(ids.begin() + 2); // Deleted/reused 256 cannot revive its prior history.
  player.membership(ids.data(), ids.size());
  ids.insert(ids.begin() + 2, 256);
  player.membership(ids.data(), ids.size());
  assert(previous(player, current) == 128);
  player.invalidate(128);
  assert(previous(player, current) == 0);
  assert(!player.previousAvailable());
  ids.clear();
  for (int index = 0; index < 512; ++index) ids.push_back(index * 4099);
  current = other = ids[0];
  player.reset(true, current, ids.data(), ids.size(), 23);
  clean.reset(true, other, ids.data(), ids.size(), 23);
  std::set<int> first{current};
  for (size_t index = 1; index < ids.size(); ++index) {
    const int candidate = player.prepareNext(current);
    for (int failure = 0; failure < 3; ++failure) assert(player.prepareNext(current) == candidate);
    assert(next(player, current) == next(clean, other));
    assert(first.insert(current).second);
  }
  for (int round = 0; round < 3; ++round) {
    std::set<int> seen;
    for (size_t index = 0; index < ids.size(); ++index) {
      const int before = current;
      assert(next(player, current) == next(clean, other));
      assert(current != before && seen.insert(current).second);
    }
    assert(seen.size() == ids.size());
  }
}
} // namespace

int main() {
  sparseIdsAndUnboundedRounds();
  shuffleRoundsAndAllFiftySlots();
  sequentialHistoryCapacityReplayAndBranch();
  shuffleHistoryReplaysBeforeFreshBag();
  manualConsumesUnseenWithoutResettingRound();
  membershipPreservesSurvivorsAndAddsUnseen();
  deletedHistoryIsSkippedAndDoesNotRevive();
  modeChangePreservesHistoryAndRestartsRound();
  zeroAndOneDoNotLoop();
  failedPreparationDoesNotConsumeRandomBagOrHistory();
  seedIsInjectableAndResetIsDeterministic();
  crossRoundHistoryDoesNotConsumeTheFreshBag();
  deletedHistoryCapacityIsReclaimed();
  showingCurrentBranchesWithoutDuplicateHistory();
  std::cout << "picture player tests passed\n";
}
