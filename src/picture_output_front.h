#pragma once

// Run one output-buffer transfer and promote its slot only on success.
template <typename Transfer>
bool presentOutputBuffer(unsigned &front, Transfer &&transfer) {
  const unsigned back = front ^ 1U;
  if (!transfer(back)) return false;
  front = back;
  return true;
}
