//- Copyright 2014 the Neutrino authors (see AUTHORS).
//- Licensed under the Apache License, Version 2.0 (see LICENSE).

#include "test/asserts.hh"
#include "test/unittest.hh"
#include "plankton-binary.hh"

using namespace plankton;

#define DEBUG_PRINT 0

#define CHECK_BINARY(VAR) do {                                                 \
  variant_t input = (VAR);                                                     \
  BinaryWriter writer;                                                         \
  writer.write(input);                                                         \
  arena_t arena;                                                               \
  BinaryReader reader(&arena);                                                 \
  variant_t decoded = reader.parse(*writer, writer.size());                    \
  TextWriter input_writer;                                                     \
  input_writer.write(input);                                                   \
  TextWriter decoded_writer;                                                   \
  decoded_writer.write(decoded);                                               \
  if (DEBUG_PRINT)                                                             \
    fprintf(stderr, "%s -> %s\n", *input_writer, *decoded_writer);             \
  ASSERT_EQ(0, strcmp(*input_writer, *decoded_writer));                        \
} while (false)

TEST(binary, simple) {
  CHECK_BINARY(variant_t::null());
  CHECK_BINARY(variant_t::yes());
  CHECK_BINARY(variant_t::no());
  CHECK_BINARY(variant_t::integer(0));
  CHECK_BINARY(-1);
  CHECK_BINARY(3);
  CHECK_BINARY(0xFFFFFFFFULL);
}

#define CHECK_ENCODED(EXP, N, ...) do {                                        \
  arena_t arena;                                                               \
  BinaryReader reader(&arena);                                                 \
  uint8_t data[N] = {__VA_ARGS__};                                             \
  variant_t found = reader.parse(data, (N));                                   \
  ASSERT_TRUE(variant_t(EXP) == found);                                        \
} while (false)

TEST(binary, zigzag) {
  CHECK_ENCODED(variant_t::integer(0), 2, BinaryImplUtils::boInteger, 0x00);
  CHECK_ENCODED(-1, 2, BinaryImplUtils::boInteger, 0x01);
  CHECK_ENCODED(1, 2, BinaryImplUtils::boInteger, 0x02);
  CHECK_ENCODED(63, 2, BinaryImplUtils::boInteger, 0x7E);
  CHECK_ENCODED(-64, 2, BinaryImplUtils::boInteger, 0x7F);
  CHECK_ENCODED(64, 3, BinaryImplUtils::boInteger, 0x80, 0x00);
  CHECK_ENCODED(-65, 3, BinaryImplUtils::boInteger, 0x81, 0x00);
  CHECK_ENCODED(65, 3, BinaryImplUtils::boInteger, 0x82, 0x00);
  CHECK_ENCODED(-8256, 3, BinaryImplUtils::boInteger, 0xFF, 0x7F);
  CHECK_ENCODED(8256, 4, BinaryImplUtils::boInteger, 0x80, 0x80, 0x00);
  CHECK_ENCODED(1056832, 5, BinaryImplUtils::boInteger, 0x80, 0x80, 0x80, 0x00);
  CHECK_ENCODED(65536, 4, BinaryImplUtils::boInteger, 0x80, 0xff, 0x06);
}

TEST(binary, integers) {
  for (int i = -655; i < 655; i += 1)
    CHECK_BINARY(variant_t::integer(i));
  for (int i = -6553; i < 6553; i += 12)
    CHECK_BINARY(variant_t::integer(i));
  for (int i = -65536; i < 65536; i += 112)
    CHECK_BINARY(variant_t::integer(i));
  for (int i = -6553600; i < 6553600; i += 11112)
    CHECK_BINARY(variant_t::integer(i));
  CHECK_BINARY(variant_t::integer(0xFFFFFFFFULL));
}
