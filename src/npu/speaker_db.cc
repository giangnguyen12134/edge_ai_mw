// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/speaker_db.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sstream>

#include "mw/common/json.h"
#include "mw/npu/postprocess.h"

namespace mw {
namespace {

const size_t kMaxNameLength = 64;

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

}  // namespace

SpeakerDb::SpeakerDb(const std::string& path) : path_(path) {}

Status SpeakerDb::Load() {
  if (path_.empty()) {
    return Status::Ok();
  }
  if (access(path_.c_str(), F_OK) != 0 && errno == ENOENT) {
    return Status::Ok();
  }
  JsonValue root;
  MW_RETURN_IF_ERROR(JsonValue::ParseFile(path_, &root));
  const JsonValue* list = root.Find("speakers");
  if (list == nullptr || !list->is_array()) {
    return InvalidArgumentError(path_ + ": missing 'speakers' array");
  }
  std::map<std::string, std::vector<float>> loaded;
  for (size_t i = 0; i < list->array().size(); ++i) {
    const JsonValue& entry = list->array()[i];
    const std::string name = entry.GetString("name", "");
    const JsonValue* emb = entry.Find("embedding");
    if (name.empty() || emb == nullptr || !emb->is_array() ||
        emb->array().empty()) {
      return InvalidArgumentError(path_ + ": malformed speaker entry");
    }
    std::vector<float> v;
    for (size_t k = 0; k < emb->array().size(); ++k) {
      if (!emb->array()[k].is_number()) {
        return InvalidArgumentError(path_ + ": non-numeric embedding value");
      }
      v.push_back(static_cast<float>(emb->array()[k].number_value()));
    }
    loaded[name] = v;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  speakers_.swap(loaded);
  return Status::Ok();
}

Status SpeakerDb::SaveLocked() const {
  if (path_.empty()) {
    return Status::Ok();
  }
  std::ostringstream os;
  os << "{\"speakers\":[";
  bool first = true;
  for (std::map<std::string, std::vector<float>>::const_iterator it =
           speakers_.begin();
       it != speakers_.end(); ++it) {
    os << (first ? "" : ",") << "{\"name\":\"" << JsonEscape(it->first)
       << "\",\"embedding\":[";
    for (size_t i = 0; i < it->second.size(); ++i) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.8g", static_cast<double>(it->second[i]));
      os << (i == 0 ? "" : ",") << buf;
    }
    os << "]}";
    first = false;
  }
  os << "]}\n";
  const std::string text = os.str();
  const std::string tmp = path_ + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    return ErrnoStatus("open " + tmp, errno);
  }
  chmod(tmp.c_str(), 0600);
  const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
  const bool flushed = fflush(f) == 0 && fsync(fileno(f)) == 0;
  fclose(f);
  if (!wrote || !flushed) {
    unlink(tmp.c_str());
    return InternalError("failed to write " + tmp);
  }
  if (rename(tmp.c_str(), path_.c_str()) != 0) {
    return ErrnoStatus("rename " + tmp, errno);
  }
  return Status::Ok();
}

Status SpeakerDb::Add(const std::string& name,
                      const std::vector<float>& embedding) {
  if (name.empty() || name.size() > kMaxNameLength) {
    return InvalidArgumentError("speaker name must be 1..64 characters");
  }
  std::vector<float> v = embedding;
  if (!L2Normalize(&v)) {
    return InvalidArgumentError("empty or all-zero embedding");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!speakers_.empty() && speakers_.begin()->second.size() != v.size()) {
    return InvalidArgumentError("embedding size differs from enrolled ones");
  }
  speakers_[name] = v;
  return SaveLocked();
}

Status SpeakerDb::Remove(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (speakers_.erase(name) == 0) {
    return NotFoundError("unknown speaker '" + name + "'");
  }
  return SaveLocked();
}

std::vector<std::string> SpeakerDb::Names() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  for (std::map<std::string, std::vector<float>>::const_iterator it =
           speakers_.begin();
       it != speakers_.end(); ++it) {
    names.push_back(it->first);
  }
  return names;
}

void SpeakerDb::Match(const std::vector<float>& embedding, std::string* name,
                      float* score) const {
  *name = "unknown";
  *score = 0.0f;
  std::lock_guard<std::mutex> lock(mutex_);
  float best = -2.0f;
  std::string best_name;
  for (std::map<std::string, std::vector<float>>::const_iterator it =
           speakers_.begin();
       it != speakers_.end(); ++it) {
    const float s = CosineSimilarity(embedding, it->second);
    if (s > best) {
      best = s;
      best_name = it->first;
    }
  }
  if (!best_name.empty()) {
    *name = best_name;
    *score = best;
  }
}

}  // namespace mw
