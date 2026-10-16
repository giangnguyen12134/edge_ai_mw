// Copyright 2026 Genio Middleware Authors.
//
// Small strict JSON (RFC 8259) parser used for configuration files. It is not
// intended for large documents or hot paths.

#ifndef MW_COMMON_JSON_H_
#define MW_COMMON_JSON_H_

#include <stddef.h>

#include <string>
#include <vector>

#include "mw/common/status.h"

namespace mw {

class JsonValue {
 public:
  enum Type { kNull, kBool, kNumber, kString, kArray, kObject };

  JsonValue() : type_(kNull), bool_(false), number_(0) {}

  static JsonValue MakeBool(bool value);
  static JsonValue MakeNumber(double value);
  static JsonValue MakeString(const std::string& value);
  static JsonValue MakeArray();
  static JsonValue MakeObject();

  Type type() const { return type_; }
  bool is_object() const { return type_ == kObject; }
  bool is_array() const { return type_ == kArray; }
  bool is_string() const { return type_ == kString; }
  bool is_number() const { return type_ == kNumber; }
  bool is_bool() const { return type_ == kBool; }

  bool bool_value() const { return bool_; }
  double number_value() const { return number_; }
  const std::string& string_value() const { return string_; }
  const std::vector<JsonValue>& array() const { return array_; }

  // Object access. Find() returns nullptr when this is not an object or the
  // key is missing.
  const JsonValue* Find(const std::string& key) const;
  size_t member_count() const { return keys_.size(); }
  const std::string& member_key(size_t index) const { return keys_[index]; }
  const JsonValue& member_value(size_t index) const { return values_[index]; }

  // Typed getters returning |default_value| when the key is missing or has a
  // different type.
  std::string GetString(const std::string& key,
                        const std::string& default_value) const;
  int GetInt(const std::string& key, int default_value) const;
  double GetDouble(const std::string& key, double default_value) const;
  bool GetBool(const std::string& key, bool default_value) const;

  // Builders (used by the parser and tests).
  void Append(const JsonValue& value) { array_.push_back(value); }
  void Set(const std::string& key, const JsonValue& value);

  // Parses |text|. On failure |out| is unspecified and the message contains
  // the byte offset of the error.
  static Status Parse(const std::string& text, JsonValue* out);
  static Status ParseFile(const std::string& path, JsonValue* out);

 private:
  Type type_;
  bool bool_;
  double number_;
  std::string string_;
  std::vector<JsonValue> array_;
  std::vector<std::string> keys_;
  std::vector<JsonValue> values_;
};

}  // namespace mw

#endif  // MW_COMMON_JSON_H_
