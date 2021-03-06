#!/usr/bin/python
# Copyright 2014 the Neutrino authors (see AUTHORS).
# Licensed under the Apache License, Version 2.0 (see LICENSE).


import plankton
from plankton import StringCodec
import unittest
import StringIO
import codecs


class StringEncodingTest(unittest.TestCase):

  def test_codecs(self):
    utf8_codec = StringCodec(StringCodec.UTF_8)
    ascii_codec = StringCodec(StringCodec.US_ASCII)
    sjis_codec = StringCodec(StringCodec.SHIFT_JIS)
    # The character U+FF83 is a katakana te which has a one-byte encoding in
    # shift-jis but a multibyte one in utf8.
    utf8 = u"foo \uff83 bar"
    utf8_bytes = bytearray(utf8.encode("utf8"))
    self.assertEquals(11, len(utf8_bytes))
    self.assertEquals((utf8_bytes, None), utf8_codec.encode(utf8))
    self.assertEquals((utf8_bytes, StringCodec.UTF_8), ascii_codec.encode(utf8))
    self.assertEquals(utf8, utf8_codec.decode(utf8_bytes))
    ascii = u"foo bar"
    ascii_bytes = bytearray(ascii.encode("ascii"))
    self.assertEquals((ascii_bytes, None), ascii_codec.encode(ascii))
    self.assertEquals(ascii, ascii_codec.decode(ascii_bytes))
    self.assertEquals((ascii_bytes, None), utf8_codec.encode(ascii))
    self.assertEquals(ascii, utf8_codec.decode(ascii_bytes))
    sjis_bytes = bytearray(utf8.encode("shift_jis"))
    self.assertEquals(9, len(sjis_bytes))
    self.assertEquals((sjis_bytes, None), sjis_codec.encode(utf8))
    self.assertEquals(utf8, sjis_codec.decode(sjis_bytes))

  def test_serializing(self):
    ascii = "foo bar"
    utf8 = u"foo \uff83 bar"
    value = [ascii, utf8]
    enc = plankton.Encoder()
    enc.set_default_string_encoding(StringCodec.US_ASCII)
    encoded = enc.encode(value)
    dec = plankton.Decoder()
    decoded = dec.decode(encoded)
    self.assertEquals(value, decoded)


if __name__ == '__main__':
  runner = unittest.TextTestRunner(verbosity=0)
  unittest.main(testRunner=runner)
