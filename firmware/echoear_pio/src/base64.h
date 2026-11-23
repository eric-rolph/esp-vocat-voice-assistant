// Base64 encoding/decoding for Arduino
// Simplified implementation for PCM16 audio frames

#ifndef BASE64_H
#define BASE64_H

#include <Arduino.h>
#include <cstring>

namespace base64 {

  const char encodingTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  inline int decodeSextet(char c) {
    if (c == '=') return 0;
    const char* p = strchr(encodingTable, c);
    return p ? static_cast<int>(p - encodingTable) : -1;
  }

  inline String encode(const uint8_t* data, size_t length) {
    size_t encodedLength = 4 * ((length + 2) / 3);
    String encoded;
    encoded.reserve(encodedLength);

    for (size_t i = 0; i < length; i += 3) {
      uint32_t triple = static_cast<uint32_t>(data[i]) << 16;

      if (i + 1 < length) {
        triple |= static_cast<uint32_t>(data[i + 1]) << 8;
      }

      if (i + 2 < length) {
        triple |= static_cast<uint32_t>(data[i + 2]);
      }

      encoded += encodingTable[(triple >> 18) & 0x3F];
      encoded += encodingTable[(triple >> 12) & 0x3F];
      encoded += (i + 1 < length) ? encodingTable[(triple >> 6) & 0x3F] : '=';
      encoded += (i + 2 < length) ? encodingTable[triple & 0x3F] : '=';
    }

    return encoded;
  }

  inline size_t decodeLength(const String& input) {
    size_t len = input.length();
    if (len == 0 || len % 4 != 0) return 0;
    size_t padding = 0;

    if (input[len - 1] == '=') padding++;
    if (len > 1 && input[len - 2] == '=') padding++;

    return (len / 4) * 3 - padding;
  }

  inline int decode(const String& input, uint8_t* output, size_t outputSize) {
    size_t len = input.length();
    if (len % 4 != 0) return -1;

    size_t outputLength = decodeLength(input);
    if (outputLength > outputSize) return -1;

    size_t j = 0;
    for (size_t i = 0; i < len; i += 4) {
      int a = decodeSextet(input[i]);
      int b = decodeSextet(input[i + 1]);
      int c = decodeSextet(input[i + 2]);
      int d = decodeSextet(input[i + 3]);
      if (a < 0 || b < 0 || c < 0 || d < 0) return -1;

      uint32_t triple = (static_cast<uint32_t>(a) << 18)
                      | (static_cast<uint32_t>(b) << 12)
                      | (static_cast<uint32_t>(c) << 6)
                      | static_cast<uint32_t>(d);

      if (j < outputLength) output[j++] = static_cast<uint8_t>((triple >> 16) & 0xFF);
      if (j < outputLength) output[j++] = static_cast<uint8_t>((triple >> 8) & 0xFF);
      if (j < outputLength) output[j++] = static_cast<uint8_t>(triple & 0xFF);
    }

    return static_cast<int>(outputLength);
  }
}

#endif // BASE64_H
