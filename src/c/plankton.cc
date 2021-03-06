//- Copyright 2014 the Neutrino authors (see AUTHORS).
//- Licensed under the Apache License, Version 2.0 (see LICENSE).

#include "c/stdc.h"
#include "c/stdnew.hh"
#include "io/iop.hh"
#include "marshal-inl.hh"
#include "plankton-binary.hh"
#include "plankton-inl.hh"
#include "socket.hh"
#include "utils/alloc.hh"

using namespace plankton;

// The current plankton version.
#define BINARY_VERSION 0xBE

// Expands to an initializer for a variant with the given tag and length fields
// in their headers. In particular, this initializes the version field
// appropriately.
#define VARIANT_INIT(tag, length) {{tag, BINARY_VERSION, length}, {0}}

typedef pton_variant_t::pton_variant_header_t header_t;

// Shared between all the arena types.
struct pton_arena_value_t {
public:
  pton_arena_value_t() : is_frozen_(false) { }

  // This virtual constructor is to make the compiler happy -- all these values
  // get arena deallocated so the constructor will never be called.
  virtual ~pton_arena_value_t() { }

  bool is_frozen() { return is_frozen_; }

  virtual void ensure_frozen() { is_frozen_ = true; }

protected:
  bool is_frozen_;
};

// An arena-allocated array.
struct pton_arena_array_t : public pton_arena_value_t {
public:
  pton_arena_array_t(Arena *origin, uint32_t init_capacity);

  bool add(Variant value);

  pton_sink_t *add_sink();

private:
  friend class plankton::Variant;
  friend class plankton::Arena;
  friend class ArraySink;
  static const uint32_t kDefaultInitCapacity = 8;
  Arena *origin_;
  uint32_t length_;
  uint32_t capacity_;
  Variant *elms_;
};

// An arena-allocated native object handle.
struct pton_arena_native_t : public pton_arena_value_t {
public:
  pton_arena_native_t(AbstractSeedType *type, void *object);

private:
  friend class plankton::Variant;
  friend class plankton::Arena;
  AbstractSeedType *type_;
  void *object_;
};

struct pton_arena_map_t : public pton_arena_value_t {
public:
  struct entry_t {
    Variant key;
    Variant value;
  };

  explicit pton_arena_map_t(Arena *origin);

  bool set(Variant key, Variant value);

  bool set(pton_sink_t **key_out, pton_sink_t **value_out);

  Variant get(Variant key, Variant defawlt = Variant()) const;

  bool has(Variant key) const;

  uint32_t size() const { return size_; }

  entry_t *elms() { return elms_; }

private:
  friend class ::Map_Iterator;
  friend class MapKeySink;
  friend class MapValueSink;

  Arena *origin_;
  uint32_t size_;
  uint32_t capacity_;
  entry_t *elms_;
};

struct pton_arena_seed_t : public pton_arena_value_t {
public:
  explicit pton_arena_seed_t(Arena *origin);

  virtual void ensure_frozen();

private:
  friend class plankton::Variant;
  Variant header_;
  Map fields_;
};

void pton_arena_seed_t::ensure_frozen() {
  fields_.ensure_frozen();
  pton_arena_value_t::ensure_frozen();
}

struct pton_arena_string_t : public pton_arena_value_t {
public:
  pton_arena_string_t(char *chars, uint32_t length, pton_charset_t encoding, bool is_frozen);

  uint32_t length() { return length_; }

  const char *chars() { return chars_; }

  pton_charset_t encoding() { return encoding_; }

private:
  char *chars_;
  uint32_t length_;
  pton_charset_t encoding_;
};

struct pton_arena_blob_t : public pton_arena_value_t {
public:
  pton_arena_blob_t(void *data, uint32_t size, bool is_frozen);

private:
  friend class plankton::Variant;
  void *data_;
  uint32_t size_;
};

struct pton_sink_t {
public:
  explicit pton_sink_t(Factory *origin);
  virtual ~pton_sink_t() { }

  // Sets this sink's value but only if the on_set callback returns true and
  // the value hasn't already been set.
  bool set(Variant value);

protected:
  // Sets whatever destination this sink is backed by.
  virtual bool set_destination(Variant value) = 0;

private:
  friend class plankton::Sink;
  bool is_empty_;
  Factory *origin_;
};

pton_arena_t *pton_new_arena() {
  return new Arena();
}

void pton_dispose_arena(pton_arena_t *arena) {
  delete arena;
}

ArenaData::~ArenaData() {
  // Invoke the scheduled cleanups.
  for (size_t i = 0; i < cleanups_.size(); i++) {
    tclib::callback_t<void(void)> &cleanup = cleanups_[i];
    cleanup();
  }
  // Release any adopted owners.
  for (size_t i = 0; i < adopted_.size(); i++)
    adopted_[i]->unmark_adopted();
  // Free memory.
  for (size_t i = 0; i < blocks_.size(); i++) {
    blob_t *blob = &blocks_[i];
    // For good measure, zap the memory before freeing it.
    blob_fill(*blob, 0xCD);
    allocator_default_free(*blob);
  }
}

void ArenaData::adopt_ownership(VariantOwner *owner) {
  owner->mark_adopted();
  adopted_.push_back(owner);
}

void ArenaData::register_cleanup(tclib::callback_t<void(void)> callback) {
  cleanups_.push_back(callback);
}

void ArenaData::mark_adopted() {
  ref();
}

void ArenaData::unmark_adopted() {
  deref();
}

VariantOwner *ArenaData::resolve_adopted() {
  return this;
}

void *ArenaData::alloc_raw(size_t bytes) {
  blob_t block = allocator_default_malloc(bytes);
  uint8_t *memory = static_cast<uint8_t*>(block.start);
  blocks_.push_back(block);
  return memory;
}

void Arena::adopt_ownership(VariantOwner *owner) {
  data()->adopt_ownership(owner->resolve_adopted());
}

void *Arena::alloc_raw(size_t bytes) {
  return data()->alloc_raw(bytes);
}

void Arena::register_cleanup(tclib::callback_t<void(void)> callback) {
  data()->register_cleanup(callback);
}

void Arena::mark_adopted() {
  // ignore.
}

void Arena::unmark_adopted() {
  // ignore
}

VariantOwner *Arena::resolve_adopted() {
  return data();
}

ArenaData *Arena::data() {
  ArenaData *shared = refcount_shared();
  if (shared == NULL) {
    shared = new (tclib::kDefaultAlloc) ArenaData();
    shared->ref();
    tclib::refcount_reference_t<ArenaData>::set_refcount_shared(shared);
  }
  return shared;
}

Native Arena::new_raw_native(void *object, AbstractSeedType *type) {
  if (object == NULL) {
    return Variant::null();
  } else {
    pton_arena_native_t *data = alloc_value<pton_arena_native_t>();
    Variant result(header_t::PTON_REPR_ARNA_NATIVE,
        new (data) pton_arena_native_t(type, object));
    return result;
  }
}

Array Arena::new_array() {
  return new_array(pton_arena_array_t::kDefaultInitCapacity);
}

pton_variant_t pton_new_array(pton_arena_t *arena) {
  return Arena::from_c(arena)->new_array().to_c();
}

Variant::Variant(repr_tag_t tag, pton_arena_value_t *arena_value) {
  pton_variant_t value = VARIANT_INIT(tag, 0);
  value.payload_.as_arena_value_ = arena_value;
  value_ = value;
}

Array Arena::new_array(uint32_t init_capacity) {
  pton_arena_array_t *data = alloc_value<pton_arena_array_t>();
  Variant result(header_t::PTON_REPR_ARNA_ARRAY, new (data) pton_arena_array_t(this, init_capacity));
  return result;
}

pton_variant_t pton_new_array_with_capacity(pton_arena_t *arena, uint32_t init_capacity) {
  return Arena::from_c(arena)->new_array(init_capacity).to_c();
}

Map Arena::new_map() {
  pton_arena_map_t *data = alloc_value<pton_arena_map_t>();
  Variant result(header_t::PTON_REPR_ARNA_MAP, new (data) pton_arena_map_t(this));
  return Map(result);
}

pton_variant_t pton_new_map(pton_arena_t *arena) {
  return Arena::from_c(arena)->new_map().to_c();
}

pton_variant_t pton_new_seed(pton_arena_t *arena) {
  return Arena::from_c(arena)->new_seed().to_c();
}

Seed Arena::new_seed(AbstractSeedType *type) {
  pton_arena_seed_t *data = alloc_value<pton_arena_seed_t>();
  Variant result = Variant(header_t::PTON_REPR_ARNA_SEED, new (data) pton_arena_seed_t(this));
  if (type != NULL)
    result.seed_set_header(type->header());
  return result;
}

pton_variant_t pton_new_c_str(pton_arena_t *arena, const char *str) {
  return Arena::from_c(arena)->new_string(str).to_c();
}

String Arena::new_string(const char *str) {
  return new_string(str, static_cast<uint32_t>(strlen(str)));
}

String Arena::new_string(const char *str, uint32_t length) {
  return new_string(str, length, Variant::default_string_encoding());
}

String Arena::new_string(const void *str, uint32_t length,
    pton_charset_t encoding) {
  pton_arena_string_t *data = alloc_value<pton_arena_string_t>();
  char *own_str = alloc_values<char>(length + 1);
  memcpy(own_str, str, length);
  own_str[length] = '\0';
  Variant result(header_t::PTON_REPR_ARNA_STRING, new (data) pton_arena_string_t(
      own_str, length, encoding, true));
  return String(result);
}

pton_variant_t pton_new_string(pton_arena_t *arena, const char *str, uint32_t length) {
  return Arena::from_c(arena)->new_string(str, length).to_c();
}

String Arena::new_string(uint32_t length) {
  return new_string(length, Variant::default_string_encoding());
}

String Arena::new_string(uint32_t length, pton_charset_t encoding) {
  pton_arena_string_t *data = alloc_value<pton_arena_string_t>();
  char *own_str = alloc_values<char>(length + 1);
  memset(own_str, '\0', length + 1);
  Variant result(header_t::PTON_REPR_ARNA_STRING, new (data) pton_arena_string_t(
      own_str, length, encoding, false));
  return String(result);
}

pton_variant_t pton_new_mutable_string(pton_arena_t *arena, uint32_t length) {
  return Arena::from_c(arena)->new_string(length).to_c();
}

Blob Arena::new_blob(const void *start, uint32_t size) {
  pton_arena_blob_t *data = alloc_value<pton_arena_blob_t>();
  uint8_t *own_start = alloc_values<uint8_t>(size);
  memcpy(own_start, start, size);
  Variant result(header_t::PTON_REPR_ARNA_BLOB, new (data) pton_arena_blob_t(own_start, size, true));
  return Blob(result);
}

Blob Arena::new_blob(uint32_t size) {
  pton_arena_blob_t *data = alloc_value<pton_arena_blob_t>();
  uint8_t *bytes = alloc_values<uint8_t>(size);
  memset(bytes, 0, size);
  Variant result(header_t::PTON_REPR_ARNA_BLOB, new (data) pton_arena_blob_t(bytes, size, false));
  return Blob(result);
}

class VariantPtrSink : public pton_sink_t {
public:
  explicit VariantPtrSink(Factory *origin)
    : pton_sink_t(origin)
    , out_(NULL) { }
  void init(Variant *out);
  virtual bool set_destination(Variant value);
private:
  Variant *out_;
};

void VariantPtrSink::init(Variant *out) {
  out_ = out;
}

bool VariantPtrSink::set_destination(Variant value) {
  *out_ = value;
  return true;
}

Sink Arena::new_sink(Variant *out) {
  VariantPtrSink *sink = alloc_sink<VariantPtrSink>();
  sink->init(out);
  return Sink(sink);
}

template <typename S>
S *Arena::alloc_sink() {
  S *result = alloc_value<S>();
  return new (result) S(this);
}

// Creates and returns a new sink value.
pton_sink_t *pton_new_sink(pton_arena_t *arena, pton_variant_t *out) {
  return Arena::from_c(arena)->new_sink(reinterpret_cast<Variant*>(out)).to_c();
}

static void pton_check_binary_version(pton_variant_t variant) {
  if (variant.header_.binary_version_ != BINARY_VERSION) {
    fprintf(stderr, "Plankton version mismatch: expected %i, found %i.\n",
        BINARY_VERSION, variant.header_.binary_version_);
    fflush(stderr);
    abort();
  }
}

pton_type_t pton_type(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return static_cast<pton_type_t>(variant.header_.repr_tag_ >> 4);
}

pton_type_t Variant::type() const {
  return pton_type(value_);
}

bool pton_variants_equal(pton_variant_t a, pton_variant_t b) {
  pton_check_binary_version(a);
  pton_check_binary_version(b);
  pton_type_t a_type = pton_type(a);
  pton_type_t b_type = pton_type(b);
  if (a_type != b_type)
    return false;
  switch (a_type) {
    case PTON_INTEGER:
      return pton_int64_value(a) == pton_int64_value(b);
    case PTON_STRING: {
      uint32_t length = pton_string_length(a);
      if (pton_string_length(b) != length)
        return false;
      return strncmp(pton_string_chars(a), pton_string_chars(b), length) == 0;
    }
    case PTON_BLOB: {
      uint32_t size = pton_blob_size(a);
      if (pton_blob_size(b) != size)
        return false;
      return strncmp(static_cast<const char*>(pton_blob_data(a)),
          static_cast<const char*>(pton_blob_data(b)), size) == 0;
    }
    case PTON_ARRAY:
      return a.payload_.as_arena_array_ == b.payload_.as_arena_array_;
    case PTON_MAP:
      return a.payload_.as_arena_map_ == b.payload_.as_arena_map_;
    case PTON_NULL:
      return true;
    case PTON_BOOL:
      return a.header_.repr_tag_ == b.header_.repr_tag_;
    case PTON_ID:
      return a.header_.length_ == b.header_.length_
          && a.payload_.as_inline_id_ == b.payload_.as_inline_id_;
    default:
      return false;
  }
}

bool Variant::operator==(const Variant &that) const {
  return pton_variants_equal(value_, that.value_);
}

bool pton_is_frozen(pton_variant_t variant) {
  pton_check_binary_version(variant);
  switch (variant.header_.repr_tag_) {
    case header_t::PTON_REPR_INT64:
    case header_t::PTON_REPR_NULL:
    case header_t::PTON_REPR_TRUE:
    case header_t::PTON_REPR_FALSE:
    case header_t::PTON_REPR_EXTN_STRING:
    case header_t::PTON_REPR_EXTN_BLOB:
    case header_t::PTON_REPR_INLN_ID:
      return true;
    case header_t::PTON_REPR_ARNA_ARRAY:
    case header_t::PTON_REPR_ARNA_MAP:
    case header_t::PTON_REPR_ARNA_STRING:
    case header_t::PTON_REPR_ARNA_BLOB:
    case header_t::PTON_REPR_ARNA_SEED:
      return variant.payload_.as_arena_value_->is_frozen();
    default:
      return false;
  }
}

bool Variant::is_frozen() const {
  return pton_is_frozen(value_);
}

void pton_ensure_frozen(pton_variant_t variant) {
  pton_check_binary_version(variant);
  switch (variant.header_.repr_tag_) {
    case header_t::PTON_REPR_ARNA_ARRAY:
    case header_t::PTON_REPR_ARNA_MAP:
    case header_t::PTON_REPR_ARNA_STRING:
    case header_t::PTON_REPR_ARNA_BLOB:
    case header_t::PTON_REPR_ARNA_SEED:
      variant.payload_.as_arena_value_->ensure_frozen();
      break;
    default:
      break;
  }
}

void Variant::ensure_frozen() {
  pton_ensure_frozen(value_);
}

Variant Variant::blob(const void *data, uint32_t size) {
  return Variant(pton_blob(data, size));
}

pton_charset_t Variant::default_string_encoding() {
  return PTON_CHARSET_UTF_8;
}

bool pton_array_add(pton_variant_t array, pton_variant_t value) {
  return Variant(array).array_add(Variant(value));
}

bool Variant::array_add(Variant value) {
  pton_check_binary_version(value_);
  pton_check_binary_version(value.value_);
  if (!is_array())
    return false;
  return value_.payload_.as_arena_array_->add(value);
}

pton_sink_t *pton_array_add_sink(pton_variant_t array) {
  pton_check_binary_version(array);
  if (!pton_is_array(array))
    return NULL;
  return array.payload_.as_arena_array_->add_sink();
}

Sink Variant::array_add_sink() {
  return Sink(pton_array_add_sink(value_));
}

Sink Array::add() {
  return array_add_sink();
}

uint32_t pton_array_length(pton_variant_t variant) {
  return Variant(variant).array_length();
}

uint32_t Variant::array_length() const {
  pton_check_binary_version(value_);
  return is_array() ? value_.payload_.as_arena_array_->length_ : 0;
}

pton_variant_t pton_array_get(pton_variant_t variant, uint32_t index) {
  return Variant(variant).array_get(index).to_c();
}

Variant Variant::array_get(uint32_t index) const {
  pton_check_binary_version(value_);
  if (!is_array())
    return null();
  pton_arena_array_t *data = value_.payload_.as_arena_array_;
  return (index < data->length_) ? data->elms_[index] : null();
}

pton_arena_array_t::pton_arena_array_t(Arena *origin, uint32_t init_capacity)
  : origin_(origin)
  , length_(0)
  , capacity_(0)
  , elms_(NULL) {
  if (init_capacity < kDefaultInitCapacity)
    init_capacity = kDefaultInitCapacity;
  capacity_ = init_capacity;
  elms_ = origin->alloc_values<Variant>(capacity_);
}

bool pton_arena_array_t::add(Variant value) {
  if (is_frozen())
    return false;
  if (length_ == capacity_) {
    capacity_ *= 2;
    Variant *new_elms = origin_->alloc_values<Variant>(capacity_);
    memcpy(new_elms, elms_, sizeof(Variant) * length_);
    elms_ = new_elms;
  }
  elms_[length_++] = value;
  return true;
}

class ArraySink : public pton_sink_t {
public:
  explicit ArraySink(Factory *origin)
    : pton_sink_t(origin)
    , array_(NULL)
    , index_(0) { }
  void init(pton_arena_array_t *data, uint32_t index);
  virtual bool set_destination(Variant value);
private:
  pton_arena_array_t *array_;
  uint32_t index_;
};

void ArraySink::init(pton_arena_array_t *array, uint32_t index) {
  array_ = array;
  index_ = index;
}

bool ArraySink::set_destination(Variant value) {
  if (array_->is_frozen())
    return false;
  array_->elms_[index_] = value;
  return true;
}

pton_sink_t *pton_arena_array_t::add_sink() {
  uint32_t index = length_;
  if (!add(Variant::null()))
    return NULL;
  ArraySink *result = origin_->alloc_sink<ArraySink>();
  result->init(this, index);
  return result;
}

pton_arena_native_t::pton_arena_native_t(AbstractSeedType *type, void *object)
  : type_(type)
  , object_(object) { }

uint32_t pton_map_size(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return pton_is_map(variant) ? variant.payload_.as_arena_map_->size() : 0;
}

AbstractSeedType *Variant::native_type() const {
  pton_check_binary_version(value_);
  switch (value_.header_.repr_tag_) {
    case header_t::PTON_REPR_EXTN_NATIVE:
      return value_.payload_.as_external_native_->type_;
    case header_t::PTON_REPR_ARNA_NATIVE:
      return value_.payload_.as_arena_native_->type_;
    default:
      return NULL;
  }
}

void *Variant::native_object() const {
  pton_check_binary_version(value_);
  switch (value_.header_.repr_tag_) {
    case header_t::PTON_REPR_EXTN_NATIVE:
      return value_.payload_.as_external_native_->object_;
    case header_t::PTON_REPR_ARNA_NATIVE:
      return value_.payload_.as_arena_native_->object_;
    default:
      return NULL;
  }
}

uint32_t Variant::map_size() const {
  return pton_map_size(value_);
}

bool pton_map_set(pton_variant_t map, pton_variant_t key, pton_variant_t value) {
  pton_check_binary_version(map);
  pton_check_binary_version(key);
  pton_check_binary_version(value);
  return pton_is_map(map) && map.payload_.as_arena_map_->set(key, value);
}

bool Variant::map_set(Variant key, Variant value) {
  return pton_map_set(value_, key.value_, value.value_);
}

pton_variant_t pton_map_get_with_default(pton_variant_t variant,
    pton_variant_t key, pton_variant_t defawlt) {
  pton_check_binary_version(variant);
  pton_check_binary_version(key);
  return pton_is_map(variant)
      ? variant.payload_.as_arena_map_->get(key, defawlt).to_c()
      : defawlt;
}

pton_variant_t pton_map_get(pton_variant_t variant, pton_variant_t key) {
  return pton_map_get_with_default(variant, key, pton_null());
}

bool pton_map_has(pton_variant_t variant, pton_variant_t key) {
  pton_check_binary_version(variant);
  pton_check_binary_version(key);
  return pton_is_map(variant) && variant.payload_.as_arena_map_->has(key);
}

bool pton_map_set_sinks(pton_variant_t map, pton_sink_t **key_out,
    pton_sink_t **value_out) {
  pton_check_binary_version(map);
  return pton_is_map(map) && map.payload_.as_arena_map_->set(key_out, value_out);
}

bool Variant::map_set(Sink *key_out, Sink *value_out) {
  pton_sink_t *key_ptr_out = NULL;
  pton_sink_t *value_ptr_out = NULL;
  if (pton_map_set_sinks(value_, &key_ptr_out, &value_ptr_out)) {
    *key_out = Sink(key_ptr_out);
    *value_out = Sink(value_ptr_out);
    return true;
  } else {
    return false;
  }
}

Variant Variant::map_get(Variant key, Variant defawlt) const {
  return pton_map_get_with_default(value_, key.value_, defawlt.value_);
}

Variant Variant::map_has(Variant key) const {
  return pton_map_has(value_, key.value_);
}

uint64_t pton_id64_value(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return pton_is_id(variant)
      ? variant.payload_.as_inline_id_
      : 0;
}

uint64_t Variant::id64_value() const {
  return pton_id64_value(value_);
}

uint32_t pton_id_size(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return pton_is_id(variant)
      ? variant.header_.length_
      : 0;
}

Variant Variant::seed_header() const {
  pton_check_binary_version(value_);
  return is_seed() ? value_.payload_.as_arena_seed_->header_ : null();
}

pton_variant_t pton_seed_get_header(pton_variant_t value) {
  return Variant(value).seed_header().to_c();
}

bool Variant::seed_set_header(Variant value) {
  pton_check_binary_version(value_);
  pton_check_binary_version(value.value_);
  if (is_seed() && !is_frozen()) {
    value_.payload_.as_arena_seed_->header_ = value;
    return true;
  } else {
    return false;
  }
}

void pton_seed_set_header(pton_variant_t value, pton_variant_t header) {
  Variant(value).seed_set_header(Variant(header));
}

bool Variant::seed_set_field(Variant key, Variant value) {
  pton_check_binary_version(value_);
  pton_check_binary_version(key.value_);
  pton_check_binary_version(value.value_);
  return is_seed()
      ? value_.payload_.as_arena_seed_->fields_.set(key, value)
      : false;
}

Variant Variant::seed_get_field(Variant key) {
  pton_check_binary_version(value_);
  pton_check_binary_version(key.value_);
  return is_seed()
      ? value_.payload_.as_arena_seed_->fields_[key]
      : null();
}

uint32_t Variant::seed_field_count() {
  pton_check_binary_version(value_);
  return is_seed()
      ? value_.payload_.as_arena_seed_->fields_.size()
      : 0;
}

Map_Iterator Variant::seed_fields_begin() {
  pton_check_binary_version(value_);
  return is_seed()
      ? value_.payload_.as_arena_seed_->fields_.begin()
      : Map_Iterator();
}

Map_Iterator Variant::seed_fields_end() {
  pton_check_binary_version(value_);
  return is_seed()
      ? value_.payload_.as_arena_seed_->fields_.end()
      : Map_Iterator();
}

uint32_t Variant::id_size() const {
  return pton_id_size(value_);
}

Map_Iterator Variant::map_begin() const {
  return Map_Iterator(value_);
}

void pton_map_iter_init(pton_map_iter_t *iter, pton_variant_t variant) {
  pton_check_binary_version(variant);
  iter->cursor = 0;
  iter->data = pton_is_map(variant) ? variant.payload_.as_arena_map_ : NULL;
}

Map_Iterator Variant::map_end() const {
  return is_map()
      ? Map_Iterator(payload()->as_arena_map_, payload()->as_arena_map_->size())
      : Map_Iterator(NULL, 0);
}

Map_Iterator::Map_Iterator(pton_arena_map_t *data, uint32_t cursor)
  : entry_(data, cursor) { }

Map_Iterator::Map_Iterator(pton_variant_t variant) {
  pton_map_iter_init(&entry_, variant);
}

Variant Map_Iterator::Entry::key() const {
  return Variant(pton_map_iter_current_key(this));
}

pton_variant_t pton_map_iter_current_key(const pton_map_iter_t *iter) {
  return iter->data->elms()[iter->cursor].key.to_c();
}

Variant Map_Iterator::Entry::value() const {
  return Variant(pton_map_iter_current_value(this));
}

pton_variant_t pton_map_iter_current_value(const pton_map_iter_t *iter) {
  return iter->data->elms()[iter->cursor].value.to_c();
}

Map_Iterator &Map_Iterator::operator++() {
  pton_map_iter_advance(&entry_);
  return *this;
}

Map_Iterator &Map_Iterator::operator++(int) {
  pton_map_iter_advance(&entry_);
  return *this;
}

void pton_map_iter_advance(pton_map_iter_t *iter) {
  iter->cursor++;
}

bool Map_Iterator::has_next() {
  return pton_map_iter_has_next(&entry_);
}

bool pton_map_iter_has_next(pton_map_iter_t *iter) {
  return (iter->data != NULL) &&
      ((iter->cursor + 1) < iter->data->size());
}

pton_arena_map_t::pton_arena_map_t(Arena *origin)
  : origin_(origin)
  , size_(0)
  , capacity_(0)
  , elms_(NULL) { }

bool pton_arena_map_t::set(Variant key, Variant value) {
  if (is_frozen())
    return false;
  if (size_ == capacity_) {
    capacity_ = (capacity_ < 4 ? 4 : (2 * capacity_));
    entry_t *new_elms = origin_->alloc_values<entry_t>(capacity_);
    memcpy(new_elms, elms_, sizeof(entry_t) * size_);
    elms_ = new_elms;
  }
  entry_t *entry = &elms_[size_++];
  entry->key = key;
  entry->value = value;
  return true;
}

class MapSink : public pton_sink_t {
public:
  explicit MapSink(Factory *origin)
    : pton_sink_t(origin)
    , map_(NULL)
    , index_(0) { }
  void init(pton_arena_map_t *map, size_t index);
protected:
  pton_arena_map_t *map_;
  size_t index_;
};

void MapSink::init(pton_arena_map_t *map, size_t index) {
  map_ = map;
  index_ = index;
}

class MapKeySink : public MapSink {
public:
  explicit MapKeySink(Factory *origin) : MapSink(origin) { }
  virtual bool set_destination(Variant value);
};

bool MapKeySink::set_destination(Variant value) {
  if (map_->is_frozen())
    return false;
  map_->elms_[index_].key = value;
  return true;
}

class MapValueSink : public MapSink {
public:
  explicit MapValueSink(Factory *origin) : MapSink(origin) { }
  virtual bool set_destination(Variant value);
};

bool MapValueSink::set_destination(Variant value) {
  if (map_->is_frozen())
    return false;
  map_->elms_[index_].value = value;
  return true;
}

bool pton_arena_map_t::set(pton_sink_t **key_out, pton_sink_t **value_out) {
  size_t index = size_;
  if (!(set(Variant::null(), Variant::null())))
    return false;
  MapKeySink *key_sink = origin_->alloc_sink<MapKeySink>();
  key_sink->init(this, index);
  *key_out = key_sink;
  MapValueSink *value_sink = origin_->alloc_sink<MapValueSink>();
  value_sink->init(this, index);
  *value_out = value_sink;
  return true;
}

Variant pton_arena_map_t::get(Variant key, Variant defawlt) const {
  for (size_t i = 0; i < size_; i++) {
    entry_t *entry = &elms_[i];
    if (entry->key == key)
      return entry->value;
  }
  return defawlt;
}

bool pton_arena_map_t::has(Variant key) const {
  for (size_t i = 0; i < size_; i++) {
    entry_t *entry = &elms_[i];
    if (entry->key == key)
      return true;
  }
  return false;
}

pton_arena_seed_t::pton_arena_seed_t(Arena *origin) {
  fields_ = origin->new_map();
}

uint32_t pton_string_length(pton_variant_t variant) {
  pton_check_binary_version(variant);
  switch (variant.header_.repr_tag_) {
  case header_t::PTON_REPR_EXTN_STRING:
    return variant.header_.length_;
  case header_t::PTON_REPR_ARNA_STRING:
    return variant.payload_.as_arena_string_->length();
  default:
    return 0;
  }
}

uint32_t Variant::string_length() const {
  return pton_string_length(value_);
}

const char *pton_string_chars(pton_variant_t variant) {
  pton_check_binary_version(variant);
  switch (variant.header_.repr_tag_) {
    case header_t::PTON_REPR_EXTN_STRING:
      return variant.payload_.as_external_string_chars_;
    case header_t::PTON_REPR_ARNA_STRING:
      return variant.payload_.as_arena_string_->chars();
    default:
      return NULL;
  }
}

char *pton_string_mutable_chars(pton_variant_t variant) {
  return pton_is_frozen(variant)
      ? NULL
      : const_cast<char*>(pton_string_chars(variant));
}

pton_charset_t Variant::string_encoding() const {
  return pton_string_encoding(value_);
}

pton_charset_t pton_string_encoding(pton_variant_t variant) {
  pton_check_binary_version(variant);
  switch (variant.header_.repr_tag_) {
    case header_t::PTON_REPR_EXTN_STRING:
      return Variant::default_string_encoding();
    case header_t::PTON_REPR_ARNA_STRING:
      return variant.payload_.as_arena_string_->encoding();
    default:
      return PTON_CHARSET_NONE;
  }
}

const char *Variant::string_chars() const {
  return pton_string_chars(value_);
}

char *Variant::string_mutable_chars() const {
  return pton_string_mutable_chars(value_);
}

pton_arena_string_t::pton_arena_string_t(char *chars, uint32_t length,
    pton_charset_t encoding, bool is_frozen)
  : chars_(chars)
  , length_(length)
  , encoding_(encoding) {
  is_frozen_ = is_frozen;
}

pton_arena_blob_t::pton_arena_blob_t(void *data, uint32_t size, bool is_frozen)
  : data_(data)
  , size_(size) {
  is_frozen_ = is_frozen;
}

uint32_t pton_blob_size(pton_variant_t variant) {
  return Variant(variant).blob_size();
}

uint32_t Variant::blob_size() const {
  pton_check_binary_version(value_);
  switch (value_.header_.repr_tag_) {
    case header_t::PTON_REPR_EXTN_BLOB:
      return value_.header_.length_;
    case header_t::PTON_REPR_ARNA_BLOB:
      return value_.payload_.as_arena_blob_->size_;
    default:
      return 0;
  }
}

const void *pton_blob_data(pton_variant_t variant) {
  return Variant(variant).blob_data();
}

const void *Variant::blob_data() const {
  pton_check_binary_version(value_);
  switch (value_.header_.repr_tag_) {
    case header_t::PTON_REPR_EXTN_BLOB:
      return value_.payload_.as_external_blob_data_;
    case header_t::PTON_REPR_ARNA_BLOB:
      return value_.payload_.as_arena_blob_->data_;
    default:
      return NULL;
  }
}

void *Variant::blob_mutable_data() {
  return is_frozen() ? NULL : const_cast<void*>(blob_data());
}

Sink::Sink(pton_sink_t *data)
  : data_(data) { }

bool pton_sink_set(pton_sink_t *sink, pton_variant_t value) {
  pton_check_binary_version(value);
  return sink->set(value);
}

bool Sink::set(Variant value) {
  return pton_sink_set(data_, value.value_);
}

pton_variant_t pton_sink_as_array(pton_sink_t *sink) {
  return Sink(sink).as_array().to_c();
}

Array Sink::as_array() {
  if (!can_be_set())
    return Variant::null();
  Variant value = data_->origin_->new_array();
  return set(value) ? value : Variant::null();
}

pton_variant_t pton_sink_as_map(pton_sink_t *sink) {
  return Sink(sink).as_map().to_c();
}

bool Sink::can_be_set() {
  return (data_ != NULL) && data_->is_empty_;
}

Map Sink::as_map() {
  if (!can_be_set())
    return Variant::null();
  Variant value = factory()->new_map();
  return set(value) ? value : Variant::null();
}

Seed Sink::as_seed() {
  if (!can_be_set())
    return Variant::null();
  Variant value = factory()->new_seed();
  return set(value) ? value : Variant::null();
}

Blob Sink::as_blob(uint32_t size) {
  if (!can_be_set())
    return Variant::null();
  Variant value = factory()->new_blob(size);
  return set(value) ? value : Variant::null();
}

pton_sink_t *pton_sink_new_sink(pton_sink_t *sink, pton_variant_t *out) {
  return Sink(sink).factory()->new_sink(reinterpret_cast<Variant*>(out)).to_c();
}

Factory *Sink::factory() {
  return data_->origin_;
}

bool pton_sink_set_string(pton_sink_t *sink, const char *chars, uint32_t length) {
  return Sink(sink).set_string(chars, length);
}

bool Sink::set_string(const char *chars, uint32_t length) {
  if (!can_be_set())
    return false;
  Variant value = factory()->new_string(chars, length);
  return data_->set(value);
}

pton_sink_t::pton_sink_t(Factory *origin)
  : is_empty_(true)
  , origin_(origin) { }

bool pton_sink_t::set(Variant value) {
  if (is_empty_) {
    if (!set_destination(value))
      return false;
    is_empty_ = false;
    return true;
  } else {
    return false;
  }
}

bool pton_is_integer(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return variant.header_.repr_tag_ == header_t::PTON_REPR_INT64;
}

bool pton_is_null(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return variant.header_.repr_tag_ == header_t::PTON_REPR_NULL;
}

bool pton_is_array(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return variant.header_.repr_tag_ == header_t::PTON_REPR_ARNA_ARRAY;
}

bool pton_is_map(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return variant.header_.repr_tag_ == header_t::PTON_REPR_ARNA_MAP;
}

bool pton_is_id(pton_variant_t variant) {
  pton_check_binary_version(variant);
  return variant.header_.repr_tag_ == header_t::PTON_REPR_INLN_ID;
}

bool pton_bool_value(pton_variant_t variant) {
  return pton_bool_value_with_default(variant, false);
}

bool pton_bool_value_with_default(pton_variant_t variant, bool if_not_bool) {
  pton_check_binary_version(variant);
  if (variant.header_.repr_tag_ == header_t::PTON_REPR_TRUE) {
    return true;
  } else if (variant.header_.repr_tag_ == header_t::PTON_REPR_FALSE) {
    return false;
  } else {
    return if_not_bool;
  }
}

int64_t pton_int64_value(pton_variant_t variant) {
  return pton_int64_value_with_default(variant, 0);
}

int64_t pton_int64_value_with_default(pton_variant_t variant, int64_t if_not_int) {
  pton_check_binary_version(variant);
  return pton_is_integer(variant) ? variant.payload_.as_int64_ : if_not_int;
}


pton_variant_t pton_null() {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_NULL, 0);
  return result;
}

pton_variant_t pton_true() {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_TRUE, 0);
  return result;
}

pton_variant_t pton_false() {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_FALSE, 0);
  return result;
}

pton_variant_t pton_bool(bool value) {
  pton_variant_t result = VARIANT_INIT(
      value ? header_t::PTON_REPR_TRUE : header_t::PTON_REPR_FALSE,
      0);
  return result;
}

pton_variant_t pton_integer(int64_t value) {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_INT64, 0);
  result.payload_.as_int64_ = value;
  return result;
}

pton_variant_t pton_string(const char *chars, uint32_t length) {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_EXTN_STRING,
      length);
  result.payload_.as_external_string_chars_ = chars;
  return result;
}

pton_variant_t pton_c_str(const char *chars) {
  return pton_string(chars, static_cast<uint32_t>(strlen(chars)));
}

pton_variant_t pton_blob(const void *data, uint32_t size) {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_EXTN_BLOB,
      size);
  result.payload_.as_external_blob_data_ = data;
  return result;
}

pton_variant_t pton_id64(uint64_t value) {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_INLN_ID, 64);
  result.payload_.as_inline_id_ = value;
  return result;
}

pton_variant_t pton_id32(uint32_t value) {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_INLN_ID, 32);
  result.payload_.as_inline_id_ = value;
  return result;
}

pton_variant_t pton_id(uint32_t size, uint64_t value) {
  pton_variant_t result = VARIANT_INIT(header_t::PTON_REPR_INLN_ID, size);
  result.payload_.as_inline_id_ = value;
  return result;
}

OutputSocket::OutputSocket(tclib::OutStream *dest)
  : dest_(dest)
  , cursor_(0)
  , default_encoding_(PTON_CHARSET_UTF_8)
  , has_been_inited_(false) { }

static const byte_t kHeader[8] = {'p', 't', 0xF6, 'n', 0, 0, 0, 0};

fat_bool_t OutputSocket::init() {
  write_blob(const_cast<byte_t*>(kHeader), 8);
  write_byte(kSetDefaultStringEncoding);
  write_uint64(default_encoding_);
  write_padding();
  dest_->flush();
  has_been_inited_ = true;
  return F_TRUE;
}

bool OutputSocket::set_default_string_encoding(pton_charset_t value) {
  if (has_been_inited_)
    return false;
  default_encoding_ = value;
  return true;
}

void OutputSocket::send_value(Variant value, Variant stream_id) {
  write_byte(kSendValue);
  write_value(stream_id);
  write_value(value);
  write_padding();
  flush();
}

void OutputSocket::write_blob(byte_t *data, size_t size) {
  cursor_ += size;
  tclib::WriteIop iop(dest_, data, size);
  iop.execute();
}

void OutputSocket::write_value(Variant value) {
  BinaryWriter writer;
  writer.write(value);
  size_t size = writer.size();
  write_uint64(size);
  write_blob(*writer, size);
}

void OutputSocket::write_byte(byte_t value) {
  write_blob(&value, 1);
}

void OutputSocket::write_uint64(uint64_t value) {
  // This is redundant with pton_assembler_t::write_uint64 but trying to factor
  // it out and share the code would probably be more trouble than it's worth.
  uint64_t current = value;
  while (current >= 0x80) {
    write_byte(static_cast<byte_t>((current & 0x7F) | 0x80));
    current = (current >> 7) - 1;
  }
  write_byte(static_cast<byte_t>(current));
}

void OutputSocket::write_padding() {
  while ((cursor_ % 8) != 0)
    write_byte(0);
}

void OutputSocket::flush() {
  dest_->flush();
}

StreamId::StreamId(byte_t *raw_key, size_t key_size, bool owns_key)
  : raw_key_(raw_key)
  , key_size_(key_size)
  , hash_code_(0)
  , owns_key_(owns_key) {
  for (size_t i = 0; i < key_size_; i++)
    hash_code_ = (hash_code_ << 3) ^ raw_key_[i] ^ (hash_code_ >> (WORD_SIZE - 3));
}

bool StreamId::operator==(const StreamId &that) const {
  if (key_size_ != that.key_size_)
    return false;
  for (size_t i = 0; i < key_size_; i++) {
    if (raw_key_[i] != that.raw_key_[i])
      return false;
  }
  return true;
}

bool StreamId::operator<(const StreamId &that) const {
  if (key_size_ < that.key_size_)
    return true;
  for (size_t i = 0; i < key_size_; i++) {
    if (raw_key_[i] < that.raw_key_[i])
      return true;
  }
  return false;
}

void StreamId::dispose() {
  if (owns_key_)
    delete[] raw_key_;
  raw_key_ = NULL;
}

InputSocket::InputSocket(tclib::InStream *src)
  : src_(src)
  , has_been_inited_(false)
  , cursor_(0)
  , default_type_registry_(NULL) {
  CHECK_FALSE("NULL socket source", src == NULL);
  stream_factory_ = tclib::new_callback(new_default_stream);
}

InputSocket::~InputSocket() {
  // Dispose all the data associated with the streams map.
  for (StreamMap::iterator i = streams_.begin(); i != streams_.end(); ++i) {
    StreamId id = i->first;
    InputStream *stream = i->second;
    id.dispose();
    delete stream;
  }
  streams_.clear();
}

bool InputSocket::set_stream_factory(InputStreamFactory factory) {
  if (has_been_inited_)
    return false;
  stream_factory_ = factory;
  return true;
}

InputStream *InputSocket::new_default_stream(InputStreamConfig *config) {
  return new BufferInputStream(config);
}

BufferInputStream::BufferInputStream(InputStreamConfig *config)
  : InputStream(config)
  , type_registry_(config->default_type_registry()) { }

void BufferInputStream::receive_block(MessageData *message) {
  pending_messages_.push_back(message);
}

Variant BufferInputStream::pull_message(Factory *factory) {
  if (pending_messages_.empty())
    return Variant::null();
  MessageData *message = pending_messages_.front();
  pending_messages_.erase(pending_messages_.begin());
  BinaryReader reader(factory);
  reader.set_type_registry(type_registry_);
  Variant result = reader.parse(message->data(), message->size());
  delete message;
  return result;
}

PushInputStream::PushInputStream(InputStreamConfig *config, MessageAction action)
  : InputStream(config)
  , type_registry_(config->default_type_registry()) {
  if (!action.is_empty())
    actions_.push_back(action);
}

InputStream *PushInputStream::new_instance(InputStreamConfig *config) {
  return new PushInputStream(config);
}

void PushInputStream::receive_block(MessageData *message) {
  Arena arena;
  BinaryReader reader(&arena);
  reader.set_type_registry(type_registry_);
  Variant value = reader.parse(message->data(), message->size());
  delete message;
  ParsedMessage parsed(&arena, value);
  for (std::vector<MessageAction>::iterator i = actions_.begin();
       i != actions_.end();
       i++) {
    MessageAction &action = *i;
    action(&parsed);
  }
}

void PushInputStream::add_action(MessageAction action) {
  actions_.push_back(action);
}

// The raw underlying data of the root id.
static const byte_t kRawRootId[1] = {BinaryImplUtils::boNull};

fat_bool_t InputSocket::init() {
  byte_t header[8];
  bool at_eof = false;
  read_blob(header, 8, &at_eof);
  for (size_t i = 0; i < 8; i++) {
    if (header[i] != kHeader[i])
      return F_FALSE;
  }
  StreamId id = root_id();
  InputStreamConfig config(id, default_type_registry_);
  InputStream *root_stream = stream_factory_(&config);
  streams_[id] = root_stream;
  has_been_inited_ = true;
  return F_TRUE;
}

fat_bool_t InputSocket::process_next_instruction(ProcessInstrStatus *status_out) {
  bool at_eof = false;
  byte_t opcode = read_byte(&at_eof);
  switch (opcode) {
    case kSetDefaultStringEncoding: {
      read_uint64(&at_eof);
      read_padding(&at_eof);
      return F_BOOL(!at_eof);
    }
    case kSendValue: {
      size_t stream_id_size = 0;
      byte_t *stream_id_data = read_value(&stream_id_size, &at_eof);
      StreamId id(stream_id_data, stream_id_size, true);
      size_t value_size = 0;
      byte_t *value_data = read_value(&value_size, &at_eof);
      read_padding(&at_eof);
      InputStream *dest = get_stream(id);
      if (dest == NULL) {
        delete value_data;
      } else {
        dest->receive_block(new MessageData(value_data, value_size));
      }
      id.dispose();
      return F_BOOL(!at_eof);
    }
    default: {
      if (!(opcode == 0 && at_eof) && (status_out != NULL))
        // When we reach the end a 0 is returned so we allow that case without
        // reporting an error, otherwise we report if asked to.
        *status_out = ProcessInstrStatus(true);
      return F_FALSE;
    }
  }
}

fat_bool_t InputSocket::process_all_instructions() {
  CHECK_TRUE("input socket not inited", has_been_inited_);
  fat_bool_t last_result = F_TRUE;
  while (last_result) {
    ProcessInstrStatus status;
    last_result = process_next_instruction(&status);
    if (status.is_error())
      return last_result;
  }
  return F_TRUE;
}

byte_t *InputSocket::read_value(size_t *size_out, bool *at_eof_out) {
  uint32_t size = read_uint32(at_eof_out);
  byte_t *data = new byte_t[size];
  read_blob(data, size, at_eof_out);
  *size_out = size;
  return data;
}

StreamId InputSocket::root_id() {
  return StreamId(const_cast<byte_t*>(kRawRootId), 1, false);
}

InputStream *InputSocket::root_stream() {
  return get_stream(root_id());
}

InputStream *InputSocket::get_stream(StreamId id) {
  StreamMap::iterator i = streams_.find(id);
  return (i == streams_.end()) ? NULL : i->second;
}

void InputSocket::read_blob(byte_t *dest, size_t size, bool *at_eof_out) {
  cursor_ += size;
  tclib::ReadIop iop(src_, dest, size);
  iop.execute();
  if (iop.at_eof())
    *at_eof_out = true;
}

byte_t InputSocket::read_byte(bool *at_eof_out) {
  byte_t value = 0;
  read_blob(&value, 1, at_eof_out);
  return value;
}

uint64_t InputSocket::read_uint64(bool *at_eof_out) {
  uint8_t next = read_byte(at_eof_out);
  uint64_t result = (next & 0x7F);
  uint64_t offset = 7;
  while (next >= 0x80) {
    next = read_byte(at_eof_out);
    uint64_t payload = ((next & 0x7F) + 1);
    result = result + (payload << offset);
    offset += 7;
  }
  return result;
}

uint32_t InputSocket::read_uint32(bool *at_eof_out) {
  uint64_t full = read_uint64(at_eof_out);
  CHECK_TRUE("uint32 too wide", full <= 0xFFFFFFFF);
  return static_cast<uint32_t>(full);
}

void InputSocket::read_padding(bool *at_eof_out) {
  while ((cursor_ % 8) != 0)
    read_byte(at_eof_out);
}
