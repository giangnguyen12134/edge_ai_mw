// Copyright 2026 Genio Middleware Authors.
//
// Enrolled voice prints (speaker embeddings), optionally persisted as JSON.
// Thread safe. Voice prints are biometric data: keep the file readable only
// by the service user (the shipped systemd unit does).

#ifndef MW_NPU_SPEAKER_DB_H_
#define MW_NPU_SPEAKER_DB_H_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "mw/common/macros.h"
#include "mw/common/status.h"

namespace mw {

class SpeakerDb {
 public:
  // An empty |path| keeps the database in memory only.
  explicit SpeakerDb(const std::string& path);

  MW_DISALLOW_COPY_AND_ASSIGN(SpeakerDb);

  // Missing file is not an error (starts empty).
  Status Load();

  // Stores (or replaces) |name|; the embedding is L2-normalised and the file
  // rewritten atomically.
  Status Add(const std::string& name, const std::vector<float>& embedding);
  Status Remove(const std::string& name);
  std::vector<std::string> Names() const;

  // Best cosine match. Returns "unknown" with score 0 for an empty database.
  void Match(const std::vector<float>& embedding, std::string* name,
             float* score) const;

 private:
  Status SaveLocked() const;

  std::string path_;
  mutable std::mutex mutex_;
  std::map<std::string, std::vector<float>> speakers_;
};

}  // namespace mw

#endif  // MW_NPU_SPEAKER_DB_H_
