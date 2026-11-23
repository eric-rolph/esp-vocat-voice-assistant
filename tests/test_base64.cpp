#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "Arduino.h"
#include "base64.h"

int main() {
  // 1. Round-trip PCM16-like binary audio frame
  const uint8_t sample[] = {0x00, 0x7F, 0x80, 0xFF, 0x12, 0x34, 0xAB, 0xCD, 0xEF, 0x01};
  String enc = base64::encode(sample, sizeof(sample));
  assert(enc == "AH+A/xI0q83vAQ==");

  uint8_t out[32] = {};
  int decLen = base64::decode(enc, out, sizeof(out));
  assert(decLen == static_cast<int>(sizeof(sample)));
  assert(std::memcmp(sample, out, sizeof(sample)) == 0);

  // 2. Reject invalid base64 characters safely without null-pointer UB
  assert(base64::decode("AB!D", out, sizeof(out)) == -1);
  assert(base64::decode("ABC", out, sizeof(out)) == -1);

  // 3. Reject undersized output buffer safely
  assert(base64::decode(enc, out, 4) == -1);

  std::cout << "C++ base64 audio framing tests passed!\n";
  return 0;
}
