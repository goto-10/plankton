//- Copyright 2014 the Neutrino authors (see AUTHORS).
//- Licensed under the Apache License, Version 2.0 (see LICENSE).

/// C++ implementation of the plankton serialization format.

#ifndef _PLANKTON_HH
#define _PLANKTON_HH

#include "stdc.h"

BEGIN_C_INCLUDES
#include "plankton.h"
END_C_INCLUDES

namespace plankton {

typedef ::pton_arena_t arena_t;

class variant_t;

// An iterator that allows you to scan through all the mappings in a map.
class map_iterator_t {
public:
  // If there are more mappings in this map sets the key and value in the
  // given out parameters and returns true. Otherwise returns false.
  bool advance(variant_t *key, variant_t *value);

  // Returns true iff the next call to advance will return true.
  bool has_next();

private:
  friend class variant_t;
  map_iterator_t(pton_arena_map_t *data);
  map_iterator_t() : data_(NULL), cursor_(0), limit_(0) { }

  pton_arena_map_t *data_;
  uint32_t cursor_;
  uint32_t limit_;
};


// A plankton variant. A variant can represent any of the plankton data types.
// Some variant values, like integers and external strings, can be constructed
// without allocation whereas others, like arrays and maps, must be allocated in
// an arena. Some variant types can be mutable, such as strings and arrays, to
// allow values to be built incrementally. All variant types can be frozen such
// that any further modification will be rejected.
//
// Variants can be handled in two equivalent but slightly different ways,
// depending on what's convenient. The basic variant_t type has methods for
// interacting with all the different types. For instance you can ask for the
// array length of any variant by calling variant_t::array_length, regardless of
// whether you're statically sure it's an array. For arrays you'll get the
// actual length back, for any other type there's a fallback result which in
// this case is 0.
//
// Alternatively there are specialized types such as array_t that provide the
// same functionality but in a more convenient form. So instead of calling
// variant_t::array_length you can convert the value to an array and use
// array_t::length. Semantically this is equivalent but it makes your
// assumptions clear and the code more concise.
class variant_t {
private:
public:
  // The different types of variants. The values are the corresponding
  // representation tags downshifted by 4.
  enum type_t {
    vtInteger = 0x01,
    vtString = 0x02,
    vtBlob = 0x03,
    vtNull = 0x04,
    vtBool = 0x05,
    vtArray = 0x06,
    vtMap = 0x07
  };

  // Initializes a variant representing null.
  inline variant_t() {
    value_.repr_tag_ = variant_p::rtNull;
    payload()->as_integer_ = 0;
  }

  // Static method that returns a variant representing null. Equivalent to
  // the no-arg constructor but more explicit.
  static inline variant_t null() { return variant_t(); }

  // Returns a variant representing the boolean true. Called 'yes' because
  // 'true' is a keyword.
  static inline variant_t yes() { return variant_t(variant_p::rtTrue); }

  // Returns a variant representing the boolean false. Called 'no' because
  // 'false' is a keyword.
  static inline variant_t no() { return variant_t(variant_p::rtFalse); }

  // Returns a variant representing a bool, false if the value is 0, true
  // otherwise.
  static inline variant_t boolean(int value) { return variant_t(value ? variant_p::rtTrue : variant_p::rtFalse); }

  // Initializes a variant representing an integer with the given value. Note
  // that this is funky when used with a literal 0 because it also matches the
  // pointer constructors.
  inline variant_t(int64_t integer);

  // Static constructor for integer variants that doesn't rely on overloading,
  // unlike the constructor.
  static inline variant_t integer(int64_t value);

  // Initializes a variant representing a string with the given contents, using
  // strlen to determine the string's length. This does not copy the string so
  // it has to stay alive for as long as the variant is used.
  inline variant_t(const char *string);

  // Explicit constructor for string-valued variants. Note that the variant does
  // not take ownership of the string so it must stay alive as long as the
  // variant does. Use an arena to create a variant that does take ownership.
  static inline variant_t string(const char *string);

  // Initializes a variant representing a string with the given contents. This
  // does not copy the string so it has to stay alive for as long as the
  // variant is used. Use an arena to create a variant that does copy the string.
  inline variant_t(const char *string, size_t length);

  // Explicit constructor for string-valued variants. Note that the variant does
  // not take ownership of the string so it must stay alive as long as the
  // variant does. Use an arena to create a variant that does take ownership.
  static inline variant_t string(const char *string, size_t length);

  // Explicit constructor for a binary blob. The size is in bytes. This
  // does not copy the string so it has to stay alive for as long as the
  // variant is used. Use an arena to create a variant that does copy the string.
  static variant_t blob(const void *data, size_t size);

  // Returns this value't type.
  type_t type() const;

  // Returns the integer value of this variant if it is an integer, otherwise
  // 0.
  inline int64_t integer_value() const;

  // Returns the length of this string if it is a string, otherwise 0.
  uint32_t string_length() const;

  // Returns the characters of this string if it is a string, otherwise NULL.
  const char *string_chars() const;

  // Returns the index'th character in this string if this is a string with
  // at least index characters, otherwise 0.
  char string_get(size_t index) const;

  // Sets the index'th character if this is a mutable string with at least
  // index characters. Returns true if setting succeeded.
  bool string_set(size_t index, char value);

  // If this variant is a blob, returns the number of bytes. If not, returns 0.
  uint32_t blob_size() const;

  // If this variant is a blob returns the blob data. If not returns NULL.
  const void *blob_data() const;

  // Returns the index'th byte in this blob if this is a blob of size at least
  // index, otherwise 0.
  uint8_t blob_get(size_t index) const;

  // Sets the index'th byte if this is a mutable blob with size at least index.
  // Returns true if setting succeeded.
  bool blob_set(size_t index, uint8_t b);

  // Returns the length of this array.
  uint32_t array_length() const;

  // Returns the index'th element, null if the index is greater than the array's
  // length.
  variant_t array_get(size_t index) const;

  // Adds the given value at the end of this array if it is mutable. Returns
  // true if adding succeeded.
  bool array_add(variant_t value);

  // Returns the number of mappings in this map, if this is a map, otherwise
  // 0.
  uint32_t map_size() const;

  // Adds a mapping from the given key to the given value if this map is
  // mutable. Returns true if setting succeeded.
  bool map_set(variant_t key, variant_t value);

  // Returns the mapping for the given key in this map if this contains the
  // key, otherwise null.
  variant_t map_get(variant_t key) const;

  // Returns an iterator for iterating this map, if this is a map, otherwise an
  // empty iterator. The first call to advance will yield the first mapping, if
  // there is one.
  map_iterator_t map_iter() const;

  // Returns the value of this boolean if it is a boolean, otherwise false. In
  // other words, true iff this is the boolean true value. Note that this is
  // different from the bool() operator which returns true for all values except
  // null. Think of this as an accessor for the value of something you know is
  // a boolean, whereas the bool() operator tests whether the variant is a
  // nontrivial value.
  inline bool bool_value() const;

  // Returns true if this is a truthy value, that is, not the null value. This
  // is useful in various conversion which return a truthy value on success and
  // null on failure. Note that this is different from the bool_value() function
  // which returns true only for the true value. Think of this as a test of
  // whether the variant is a nontrivial value, similar to testing whether a
  // pointer is NULL, whereas bool_value is an accessor for the value of
  // something you know is a boolean.
  inline operator bool() const;

  // Returns true if this value is identical to the given value. Integers and
  // strings are identical if their contents are the same, the singletons are
  // identical to themselves, and structured values are identical if they were
  // created by the same new_... call. So two arrays with the same values are
  // not necessarily considered identical.
  bool operator==(variant_t that);

  // Returns true iff this value is locally immutable. Note that even if this
  // returns true it doesn't mean that nothing about this value can change -- it
  // may contain references to other values that are mutable.
  bool is_frozen();

  // Renders this value locally immutable. Values referenced from this one may
  // be mutable so it may still change indirectly, just not this concrete
  // object.
  void ensure_frozen();

  inline bool is_integer() const { return repr_tag() == variant_p::rtInteger; }

  inline bool is_map() const { return repr_tag() == variant_p::rtArenaMap; }

  inline bool is_array() const { return repr_tag() == variant_p::rtArenaArray; }

  inline bool is_string() const { return type() == vtString; }

  inline bool is_blob() const { return type() == vtBlob; }

protected:
  variant_p value_;

  // Convenience accessor for the representation tag.
  variant_p::repr_tag_t repr_tag() const { return value_.repr_tag_; }

  pton_variant_payload_t *payload() { return &value_.payload_; }

  const pton_variant_payload_t *payload() const { return &value_.payload_; }

private:
  friend class ::pton_arena_t;

  // Creates a variant with no payload and the given type.
  variant_t(variant_p::repr_tag_t tag) {
    value_.repr_tag_ = tag;
    payload()->as_integer_ = 0;
  }

  variant_t(variant_p::repr_tag_t tag, pton_arena_value_t *arena_value) {
    value_.repr_tag_ = tag;
    payload()->as_arena_value_ = arena_value;
  }
};

// A variant that represents an array. An array can be either an actual array
// or null, to make conversion more convenient. If you want to be sure you're
// really dealing with an array do an if-check.
class array_t : public variant_t {
public:
  // Conversion to an array of some value. If the value is indeed an array the
  // result is a proper array, if it is something else the result is null.
  explicit array_t(variant_t variant);

  // Adds the given value at the end of this array if it is mutable. Returns
  // true if adding succeeded.
  bool add(variant_t value) { return array_add(value); }

  // Returns the length of this array.
  uint32_t length() const { return array_length(); }

  // Returns the index'th element, null if the index is greater than the array's
  // length.
  variant_t operator[](size_t index) const { return array_get(index); }
};

// A variant that represents a map. A map can be either an actual map or null,
// to make conversion more convenient. If you want to be sure you're really
// dealing with a map do an if-check.
class map_t : public variant_t {
public:
  // Conversion to a map of some value. If the value is indeed a map the
  // result is a proper map, if it is something else the result is null.
  explicit map_t(variant_t variant);

  // Adds a mapping from the given key to the given value if this map is
  // mutable. Returns true if setting succeeded.
  bool set(variant_t key, variant_t value) { return map_set(key, value); }

  // Returns the mapping for the given key.
  variant_t operator[](variant_t key) { return map_get(key); }

  // Returns the number of mappings in this map.
  uint32_t size() const { return map_size(); }

  // Returns an iterator for iterating this map. The first call to advance will
  // yield the first mapping, if there is one.
  map_iterator_t iter() const { return map_iter(); }
};

// A variant that represents a string. A string can be either an actual string
// or null, to make conversion more convenient. If you want to be sure you're
// really dealing with a string do an if-check.
class string_t : public variant_t {
public:
  explicit string_t(variant_t variant);

  // Returns the length of this string if it is a string, otherwise 0.
  size_t length() const { return string_length(); }

  // Returns the index'th character in this string if this is a string with
  // at least index characters, otherwise 0.
  char get(size_t index) const { return string_get(index); }

  // Sets the index'th character if this is a mutable string with at least
  // index characters. Returns true if setting succeeded.
  bool set(size_t index, char c) { return string_set(index, c); }
};

// A variant that represents a blob. A blob can be either an actual blob or
// null, to make conversion more convenient. If you want to be sure you're
// really dealing with a blob do an if-check.
class blob_t : public variant_t {
public:
  explicit blob_t(variant_t variant);

  // Returns the size of this blob if it is a blob, otherwise 0.
  size_t size() const { return blob_size(); }

  // Returns the index'th byte in this blob if this is a blob of size at least
  // index, otherwise 0.
  uint8_t get(size_t index) const { return blob_get(index); }

  // Sets the index'th byte if this is a mutable blob with size at least index.
  // Returns true if setting succeeded.
  bool set(size_t index, uint8_t b) { return blob_set(index, b); }
};

// A sink is like a pointer to a variant except that it also has access to an
// arena such that instead of creating a value in an arena and then storing it
// in the sink you would ask the sink to create the value itself.
class sink_t {
public:
  // Returns the value stored in this sink.
  variant_t operator*() const;

  // If this sink has not already been asigned, creates an array, stores it as
  // the value of this sink, and returns it.
  array_t as_array();

  // If this sink has not already been asigned, creates a map, stores it as
  // the value of this sink, and returns it.
  map_t as_map();

  // Sets the value of this sink, if it hasn't already been set. Otherwise this
  // is a no-op. Returns whether the value was set.
  bool set(variant_t value);

private:
  friend class ::pton_arena_t;
  explicit sink_t(pton_arena_sink_t *data);

  pton_arena_sink_t *data_;
};

// Utility for serializing variant values to plankton.
class BinaryWriter {
public:
  BinaryWriter();
  ~BinaryWriter();

  // Write the given value to this writer's internal buffer.
  void write(variant_t value);

  // Returns the start of the buffer.
  uint8_t *operator*() { return bytes_; }

  // Returns the size in bytes of the data written to this writer's buffer.
  size_t size() { return size_; }

private:
  friend class BinaryWriterImpl;
  uint8_t *bytes_;
  size_t size_;
};

// An object that holds the representation of a variant as a 7-bit ascii string.
class TextWriter {
public:
  TextWriter();
  ~TextWriter();

  // Write the given variant to this asciigram.
  void write(variant_t value);

  // After encoding, returns the string containing the encoded representation.
  const char *operator*() { return chars_; }

  // After encoding, returns the length of the string containing the encoded
  // representation.
  size_t length() { return length_; }

private:
  friend class TextWriterImpl;
  char *chars_;
  size_t length_;
};

// Utility for reading variant values from serialized data.
class BinaryReader {
public:
  // Creates a new reader that allocates values from the given arena.
  BinaryReader(pton_arena_t *arena);

  // Deserializes the given input and returns the result as a variant.
  variant_t parse(const void *data, size_t size);

private:
  friend class BinaryReaderImpl;
  pton_arena_t *arena_;
};

// Utility for converting a plankton variant to a 7-bit ascii string.
class TextReader {
public:
  // Creates a new parser which uses the given arena for allocation.
  TextReader(pton_arena_t *arena);

  // Parse the given input, returning the value. If any errors occur the
  // has_failed() and offender() methods can be used to identify what the
  // problem was.
  variant_t parse(const char *chars, size_t length);

  // Returns true iff the last parse failed.  If parse hasn't been called at all
  // returns false.
  bool has_failed() { return has_failed_; }

  // If has_failed() returns true this will return the offending character.
  char offender() { return offender_; }

private:
  friend class TextReaderImpl;
  pton_arena_t *arena_;
  bool has_failed_;
  char offender_;
};

} // namespace plankton

// An arena within which plankton values can be allocated. Once the values are
// no longer needed all can be disposed by disposing the arena.
class pton_arena_t {
public:
  // Creates a new empty arena.
  inline pton_arena_t();

  // Disposes all memory allocated within this arena.
  ~pton_arena_t();

  // Allocates a new array of values the given size within this arena. Public
  // for testing only. The values are not initialized.
  template <typename T>
  T *alloc_values(size_t elms);

  // Allocates a single value of the given type. The value is not initialized.
  template <typename T>
  T *alloc_value();

  // Creates and returns a new mutable array value.
  plankton::array_t new_array();

  // Creates and returns a new mutable array value.
  plankton::array_t new_array(size_t init_capacity);

  // Creates and returns a new map value.
  plankton::map_t new_map();

  // Creates and returns a new variant string. The string is fully owned by
  // the arena so the character array can be disposed after this call returns.
  // The length of the string is determined using strlen.
  plankton::string_t new_string(const char *str);

  // Creates and returns a new variant string. The string is fully owned by
  // the arena so the character array can be disposed after this call returns.
  plankton::string_t new_string(const char *str, size_t length);

  // Creates and returns a new mutable variant string of the given length,
  // initialized to all '\0's. Note that this doesn't mean that the string is
  // initially empty. Variant strings can handle null characters so what you
  // get is a 'length' long string where all the characters are null. The null
  // terminator is implicitly allocated in addition to the requested length, so
  // you only need to worry about the non-null characters.
  plankton::string_t new_string(size_t length);

  // Creates and returns a new variant blob. The contents it copied into this
  // arena so the data array can be disposed after this call returns.
  plankton::blob_t new_blob(const void *data, size_t size);

  // Creates and returns a new mutable variant blob of the given size,
  // initialized to all zeros.
  plankton::blob_t new_blob(size_t size);

  // Creates and returns a new sink value.
  plankton::sink_t new_sink();

private:
  // Allocates a raw block of memory.
  void *alloc_raw(size_t size);

  // Allocates the backing storage for a sink value.
  pton_arena_sink_t *alloc_sink();

  size_t capacity_;
  size_t used_;
  uint8_t **blocks_;
};

#endif // _PLANKTON_HH