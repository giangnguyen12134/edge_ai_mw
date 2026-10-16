// Copyright 2026 Genio Middleware Authors.

#include "mw/common/json.h"

#include <stdint.h>
#include <stdlib.h>

#include <fstream>
#include <sstream>

namespace mw {
namespace {

const int kMaxDepth = 64;

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text), pos_(0) {}

  Status Run(JsonValue* out) {
    SkipSpace();
    MW_RETURN_IF_ERROR(ParseValue(out, 0));
    SkipSpace();
    if (pos_ != text_.size()) {
      return Error("trailing characters");
    }
    return Status::Ok();
  }

 private:
  Status Error(const std::string& what) const {
    std::ostringstream os;
    os << "JSON parse error at offset " << pos_ << ": " << what;
    return InvalidArgumentError(os.str());
  }

  void SkipSpace() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Consume(const char* literal) {
    size_t i = 0;
    while (literal[i] != '\0') {
      if (pos_ + i >= text_.size() || text_[pos_ + i] != literal[i]) {
        return false;
      }
      ++i;
    }
    pos_ += i;
    return true;
  }

  Status ParseValue(JsonValue* out, int depth) {
    if (depth > kMaxDepth) {
      return Error("nesting too deep");
    }
    if (pos_ >= text_.size()) {
      return Error("unexpected end of input");
    }
    const char c = text_[pos_];
    if (c == '{') {
      return ParseObject(out, depth);
    }
    if (c == '[') {
      return ParseArray(out, depth);
    }
    if (c == '"') {
      std::string s;
      MW_RETURN_IF_ERROR(ParseString(&s));
      *out = JsonValue::MakeString(s);
      return Status::Ok();
    }
    if (Consume("true")) {
      *out = JsonValue::MakeBool(true);
      return Status::Ok();
    }
    if (Consume("false")) {
      *out = JsonValue::MakeBool(false);
      return Status::Ok();
    }
    if (Consume("null")) {
      *out = JsonValue();
      return Status::Ok();
    }
    return ParseNumber(out);
  }

  Status ParseNumber(JsonValue* out) {
    const char* begin = text_.c_str() + pos_;
    char* end = nullptr;
    const double value = strtod(begin, &end);
    if (end == begin) {
      return Error("invalid value");
    }
    pos_ += static_cast<size_t>(end - begin);
    *out = JsonValue::MakeNumber(value);
    return Status::Ok();
  }

  static void AppendUtf8(uint32_t cp, std::string* out) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseHex4(uint32_t* value) {
    if (pos_ + 4 > text_.size()) {
      return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    pos_ += 4;
    *value = v;
    return true;
  }

  Status ParseString(std::string* out) {
    ++pos_;  // Opening quote.
    out->clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return Status::Ok();
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return Error("control character in string");
      }
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        break;
      }
      const char esc = text_[pos_++];
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          out->push_back(esc);
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          uint32_t cp = 0;
          if (!ParseHex4(&cp)) {
            return Error("bad \\u escape");
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t low = 0;
            if (!Consume("\\u") || !ParseHex4(&low) || low < 0xDC00 ||
                low > 0xDFFF) {
              return Error("bad surrogate pair");
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          return Error("bad escape");
      }
    }
    return Error("unterminated string");
  }

  Status ParseArray(JsonValue* out, int depth) {
    ++pos_;  // '['
    *out = JsonValue::MakeArray();
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return Status::Ok();
    }
    while (true) {
      SkipSpace();
      JsonValue element;
      MW_RETURN_IF_ERROR(ParseValue(&element, depth + 1));
      out->Append(element);
      SkipSpace();
      if (pos_ >= text_.size()) {
        return Error("unterminated array");
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return Status::Ok();
      }
      return Error("expected ',' or ']'");
    }
  }

  Status ParseObject(JsonValue* out, int depth) {
    ++pos_;  // '{'
    *out = JsonValue::MakeObject();
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return Status::Ok();
    }
    while (true) {
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return Error("expected object key");
      }
      std::string key;
      MW_RETURN_IF_ERROR(ParseString(&key));
      SkipSpace();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        return Error("expected ':'");
      }
      ++pos_;
      SkipSpace();
      JsonValue value;
      MW_RETURN_IF_ERROR(ParseValue(&value, depth + 1));
      out->Set(key, value);
      SkipSpace();
      if (pos_ >= text_.size()) {
        return Error("unterminated object");
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return Status::Ok();
      }
      return Error("expected ',' or '}'");
    }
  }

  const std::string& text_;
  size_t pos_;
};

}  // namespace

JsonValue JsonValue::MakeBool(bool value) {
  JsonValue v;
  v.type_ = kBool;
  v.bool_ = value;
  return v;
}

JsonValue JsonValue::MakeNumber(double value) {
  JsonValue v;
  v.type_ = kNumber;
  v.number_ = value;
  return v;
}

JsonValue JsonValue::MakeString(const std::string& value) {
  JsonValue v;
  v.type_ = kString;
  v.string_ = value;
  return v;
}

JsonValue JsonValue::MakeArray() {
  JsonValue v;
  v.type_ = kArray;
  return v;
}

JsonValue JsonValue::MakeObject() {
  JsonValue v;
  v.type_ = kObject;
  return v;
}

void JsonValue::Set(const std::string& key, const JsonValue& value) {
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) {
      values_[i] = value;
      return;
    }
  }
  keys_.push_back(key);
  values_.push_back(value);
}

const JsonValue* JsonValue::Find(const std::string& key) const {
  if (type_ != kObject) {
    return nullptr;
  }
  for (size_t i = 0; i < keys_.size(); ++i) {
    if (keys_[i] == key) {
      return &values_[i];
    }
  }
  return nullptr;
}

std::string JsonValue::GetString(const std::string& key,
                                 const std::string& default_value) const {
  const JsonValue* v = Find(key);
  return (v != nullptr && v->is_string()) ? v->string_value() : default_value;
}

int JsonValue::GetInt(const std::string& key, int default_value) const {
  const JsonValue* v = Find(key);
  return (v != nullptr && v->is_number()) ? static_cast<int>(v->number_value())
                                          : default_value;
}

double JsonValue::GetDouble(const std::string& key,
                            double default_value) const {
  const JsonValue* v = Find(key);
  return (v != nullptr && v->is_number()) ? v->number_value() : default_value;
}

bool JsonValue::GetBool(const std::string& key, bool default_value) const {
  const JsonValue* v = Find(key);
  return (v != nullptr && v->is_bool()) ? v->bool_value() : default_value;
}

Status JsonValue::Parse(const std::string& text, JsonValue* out) {
  Parser parser(text);
  return parser.Run(out);
}

Status JsonValue::ParseFile(const std::string& path, JsonValue* out) {
  std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
  if (!file) {
    return NotFoundError("cannot open " + path);
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  Status status = Parse(contents.str(), out);
  if (!status.ok()) {
    return Status(status.code(), path + ": " + status.message());
  }
  return Status::Ok();
}

}  // namespace mw
