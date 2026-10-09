#include "animation_renderer.h"
#include "picture_player.h"
#include "picture_output_front.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
constexpr int W = badge_animation::Width;
constexpr int H = badge_animation::Height;
constexpr int N = W * H;
using Image = std::vector<uint16_t>;

Image makeImage(int id) {
  Image result(N);
  for (int i = 0; i < N; ++i)
    result[i] = static_cast<uint16_t>((i * 997U + id * 7919U) | 1U);
  return result;
}

void present(const Image& frame, std::array<Image, 2>& outputs, unsigned& front) {
  const bool transferred = presentOutputBuffer(front, [&](unsigned output) {
    outputs[output] = frame;
    return true;
  });
  assert(transferred);
}

Image slideFrame(const Image& oldImage, const Image& newImage, unsigned offset) {
  Image output(N);
  badge_animation::renderSlide(oldImage.data(), newImage.data(), output.data(),
                               offset, true, 4500);
  return output;
}

Image rotated(const Image& image) {
  Image output(N);
  badge_animation::renderRotation(image.data(), output.data(), 4500);
  return output;
}

void checkTransferFailureKeepsFront() {
  unsigned front = 0;
  const bool transferred = presentOutputBuffer(front, [](unsigned) { return false; });
  assert(!transferred);
  assert(front == 0);
}

void checkShowPictureSequence() {
  badge_animation::initialize();
  std::array<Image, 2> images{Image(N, 0xA55A), Image(N, 0xA55A)};
  std::array<Image, 2> outputs{Image(N, 0x5AA5), Image(N, 0x5AA5)};
  unsigned imageIndex = 0;
  unsigned front = 0;
  int current = -1;

  const int32_t ids[] = {0, 1, 2, 3};
  PicturePlayer player;
  player.reset(false, -1, ids, 4, 1);

  // Initial showPicture(0): nextSource is slot 1, prepare selects it, and
  // presentCurrent writes the first posed frame to output 1.
  images[imageIndex ^ 1U] = makeImage(0);
  imageIndex ^= 1U;
  present(rotated(images[imageIndex]), outputs, front);
  player.commitManual(0);
  current = 0;
  Image latestPresented = outputs[front];
  assert(front == 1 && latestPresented == rotated(images[imageIndex]));

  // showPicture(1) completes normally. commitImage must freeze the last
  // successfully presented outgoing pose into the old source slot.
  int slot = player.prepareNext(current);
  assert(slot == 1);
  const unsigned outgoingSlot1 = imageIndex;
  const unsigned selectedFront1 = front;
  images[imageIndex ^ 1U] = makeImage(slot);  // loadPicture(nextSource())
  imageIndex ^= 1U;                           // prepareImage()
  images[outgoingSlot1] = outputs[selectedFront1];  // commitImage()
  assert(images[outgoingSlot1] == latestPresented);
  player.commit(slot);
  current = slot;

  Image frame = slideFrame(images[imageIndex ^ 1U], images[imageIndex], 0);
  present(frame, outputs, front);             // first poll/present
  assert(outputs[front] == latestPresented);
  frame = slideFrame(images[imageIndex ^ 1U], images[imageIndex], W);
  present(frame, outputs, front);             // completed transition
  latestPresented = rotated(images[imageIndex]);
  assert(outputs[front] == latestPresented);

  // showPicture(2) is accepted, but another request arrives mid-transition.
  // Its last successful output is an intermediate I1 -> I2 slide frame.
  slot = player.prepareNext(current);
  assert(slot == 2);
  const unsigned outgoingSlot2 = imageIndex;
  images[imageIndex ^ 1U] = makeImage(slot);
  imageIndex ^= 1U;
  images[outgoingSlot2] = outputs[front];
  assert(images[outgoingSlot2] == latestPresented);
  player.commit(slot);
  current = slot;
  frame = slideFrame(images[imageIndex ^ 1U], images[imageIndex], 200);
  present(frame, outputs, front);
  latestPresented = outputs[front];

  // showPicture(3) must snapshot the most recently presented partial I1 -> I2
  // frame, not a stale output slot or the original I0 source.
  slot = player.prepareNext(current);
  assert(slot == 3);
  const unsigned outgoingSlot3 = imageIndex;
  const unsigned selectedFront3 = front;
  images[imageIndex ^ 1U] = makeImage(slot);
  imageIndex ^= 1U;
  images[outgoingSlot3] = outputs[selectedFront3];
  assert(images[outgoingSlot3] == latestPresented);
  player.commit(slot);
  current = slot;

  frame = slideFrame(images[imageIndex ^ 1U], images[imageIndex], 0);
  present(frame, outputs, front);
  assert(outputs[front] == latestPresented);
  frame = slideFrame(images[imageIndex ^ 1U], images[imageIndex], W);
  present(frame, outputs, front);
  assert(outputs[front] == rotated(images[imageIndex]));

  // A rejected staged selection restores the accepted source/output indices;
  // writing the inactive source slot cannot damage the current image/frame.
  const unsigned acceptedImage = imageIndex;
  const unsigned acceptedFront = front;
  const Image acceptedSource = images[acceptedImage];
  const Image acceptedFrame = outputs[acceptedFront];
  images[imageIndex ^ 1U] = makeImage(99);
  imageIndex ^= 1U;  // prepareImage()
  imageIndex = acceptedImage;  // rejectImage()
  front = acceptedFront;
  assert(current == 3);
  assert(images[imageIndex] == acceptedSource);
  assert(outputs[front] == acceptedFrame);
}
}  // namespace

int main() {
  checkTransferFailureKeepsFront();
  checkShowPictureSequence();
  std::cout << "PASS: showPicture source/output rollover and rollback\n";
}
