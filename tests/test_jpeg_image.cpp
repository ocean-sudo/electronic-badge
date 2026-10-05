#include "jpeg_image.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

namespace {
uint32_t table[256];
uint32_t reference(const std::vector<uint8_t>& bytes, bool normalized) {
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < bytes.size(); ++index) {
    crc ^= normalized && index >= 15 && index < 19 ? 0 : bytes[index];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
  }
  return crc ^ 0xffffffffU;
}
void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) bytes[offset + index] = value >> (index * 8);
}
void integrityBoundaries() {
  // This fixture tests the persistent envelope, not the entropy decoder.
  std::vector<uint8_t> bytes{0xff,0xd8,0xff,0xfe,0,15,'B','D','G','J','1',0,0,0,0,0,0,0,0,17,31,43,59,0xff,0xd9};
  put32(bytes,11,bytes.size());
  const uint32_t normalized = reference(bytes,true);
  put32(bytes,15,normalized);
  uint32_t expected = 0;
  assert(BadgeJpeg::header(bytes.data(),19,bytes.size(),expected) && expected == normalized);
  assert(!BadgeJpeg::header(bytes.data(),18,bytes.size(),expected));
  assert(!BadgeJpeg::header(bytes.data(),19,bytes.size()+1,expected));
  bytes[10] = '2';
  assert(!BadgeJpeg::header(bytes.data(),19,bytes.size(),expected));
  bytes[10] = '1';
  for (size_t chunk : {size_t(1),size_t(3),size_t(15),size_t(17),size_t(19),bytes.size()}) {
    BadgeJpeg::Integrity integrity(table);
    for (size_t offset = 0; offset < bytes.size(); offset += chunk)
      integrity.append(bytes.data()+offset, std::min(chunk,bytes.size()-offset));
    assert(!integrity.finish(bytes.size(),normalized));
    assert(integrity.storedCrc() == normalized);
    assert(integrity.transferCrc() == reference(bytes,false));
    assert(integrity.storedCrc() != integrity.transferCrc());
  }
  bytes[20] ^= 1;
  BadgeJpeg::Integrity corrupt(table); corrupt.append(bytes.data(),bytes.size());
  assert(!strcmp(corrupt.finish(bytes.size(),normalized),"stored_crc_mismatch"));
  bytes[20] ^= 1;
  bytes.resize(bytes.size()-2); // Recomputed valid CRCs do not excuse missing EOI.
  put32(bytes,11,bytes.size()); put32(bytes,15,reference(bytes,true));
  BadgeJpeg::Integrity noEoi(table); noEoi.append(bytes.data(),bytes.size());
  assert(!strcmp(noEoi.finish(bytes.size(),reference(bytes,true)),"jpeg_eoi"));
  assert(!strcmp(noEoi.finish(bytes.size()+1,reference(bytes,true)),"storage_read"));
}
void nonAlignedPackedEdges() {
  std::vector<uint8_t> guarded(466U*466U*2U+16,0xa5);
  uint8_t* pixels = guarded.data()+8;
  std::array<uint8_t,2*16*3> right{};
  for (size_t index=0; index<right.size()/3; ++index) {
    right[index*3]=index*7; right[index*3+1]=index*5; right[index*3+2]=index*3;
  }
  assert(BadgeJpeg::writePackedRect(pixels,right.data(),464,465,224,239)==32);
  for (unsigned row=0; row<16; ++row) for (unsigned column=0; column<2; ++column) {
    const size_t source=(row*2+column)*3;
    const uint16_t expected=((right[source]&0xf8)<<8)|((right[source+1]&0xfc)<<3)|(right[source+2]>>3);
    const size_t destination=((row+224)*466+column+464)*2;
    assert(pixels[destination]==(expected&0xff) && pixels[destination+1]==(expected>>8));
    assert(pixels[((row+224)*466+463)*2]==0xa5);
  }
  std::array<uint8_t,16*2*3> bottom{}; bottom.fill(255);
  assert(BadgeJpeg::writePackedRect(pixels,bottom.data(),224,239,464,465)==32);
  const auto unchanged=guarded;
  assert(!BadgeJpeg::writePackedRect(pixels,bottom.data(),464,466,224,239));
  assert(!BadgeJpeg::writePackedRect(pixels,bottom.data(),2,1,0,1));
  assert(guarded==unchanged);
  for (size_t index=0; index<8; ++index) assert(guarded[index]==0xa5 && guarded[guarded.size()-1-index]==0xa5);
}
void exactCoverageWithoutPixelAllocation() {
  for (unsigned mcu : {8U,16U}) {
    BadgeJpeg::Coverage coverage;
    assert(!coverage.complete());
    for (unsigned y=0; y<466; y+=mcu) for (unsigned x=0; x<466; x+=mcu)
      assert(coverage.accept(x,std::min(x+mcu,466U)-1,y,std::min(y+mcu,466U)-1));
    assert(coverage.complete());
    assert(!coverage.accept(0,15,0,15));
  }
  BadgeJpeg::Coverage overlap;
  assert(overlap.accept(0,15,0,15));
  assert(!overlap.accept(0,15,0,15));
  assert(!overlap.accept(32,47,0,15));
  assert(!overlap.accept(16,31,0,7));
  assert(!overlap.complete());
}
void logicalFileNames() {
  uint32_t slot;
  assert(BadgeJpeg::slotName("/slot0.jpg",slot) && slot==0);
  assert(BadgeJpeg::slotName("slot256.jpg",slot) && slot==256);
  assert(BadgeJpeg::slotName("slot2147483646.jpg",slot) && slot==2147483646U);
  for (const char* invalid : {"slot2147483647.jpg","slot42949672960.jpg","slot00.jpg","slot-1.jpg","slot1.rgb","slot.jpg"})
    assert(!BadgeJpeg::slotName(invalid,slot));
}
}
int main() {
  for (uint32_t value=0; value<256; ++value) {
    uint32_t crc=value;
    for (int bit=0; bit<8; ++bit) crc=(crc>>1)^((crc&1)?0xedb88320U:0);
    table[value]=crc;
  }
  integrityBoundaries(); nonAlignedPackedEdges(); exactCoverageWithoutPixelAllocation(); logicalFileNames();
  std::cout << "JPEG integrity and packed-edge tests passed\n";
}
