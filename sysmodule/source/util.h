// Small things several parts need: SHA-1 and base64 (the websocket
// handshake), snowflakes, and cutting UTF-8 to fit. Nothing here allocates.
#pragma once
#include <stddef.h>
#include <stdint.h>

void sha1(const void* data, size_t n, uint8_t out[20]);

// n bytes as base64 into out (NUL-terminated); out needs 4 * ((n + 2) / 3) + 1 bytes
void base64(const uint8_t* in, size_t n, char* out);

// A Discord id ("snowflake", sent as a decimal string); 0 if it isn't one
uint64_t parseId(const char* s, size_t n);
// As decimal into out (21 bytes is always enough)
void idString(uint64_t id, char out[21]);

// src (n bytes of UTF-8) copied into dst of cap bytes, NUL-terminated. When
// it doesn't fit it's cut at a character boundary with "…" after it.
void utf8Copy(char* dst, size_t cap, const char* src, size_t n);

// The text between quotes in a JSON string we write: s with " and \ and
// control characters escaped, into out of cap bytes; false if it didn't fit
bool jsonEscape(char* out, size_t cap, const char* s);
