// Copyright 2026 Genio Middleware Authors.
//
// Client SDK for the NpuManager: inference results and model control.
//
//   mw::NpuClient client(mw::NpuClient::Options());
//   client.Start([](const mw::InferenceResult& r) { ... });
//
// The result callback runs on the SDK's bus thread; return quickly.

#ifndef MW_SDK_NPU_CLIENT_H_
#define MW_SDK_NPU_CLIENT_H_

#include <stdint.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "mw/common/inference_result.h"
#include "mw/common/macros.h"
#include "mw/common/status.h"
#include "mw/ipc/bus.h"

namespace mw {

class NpuClient {
 public:
  struct Options {
    Options();
    std::string bus;           // "system" or "user".
    std::string npu_bus_name;  // NpuManager well-known name.
  };

  struct ModelInfo {
    ModelInfo() : enabled(false), target_fps(0) {}
    std::string name;
    std::string kind;
    bool enabled;
    double target_fps;
  };

  typedef std::function<void(const InferenceResult& result)> ResultCallback;

  explicit NpuClient(const Options& options);
  ~NpuClient();

  MW_DISALLOW_COPY_AND_ASSIGN(NpuClient);

  // Connects and starts listening. The NpuManager need not be running yet.
  Status Start(const ResultCallback& on_result);
  void Stop();

  Status ListModels(std::vector<ModelInfo>* models);
  Status SetModelEnabled(const std::string& name, bool enabled);

  // The next utterance heard by VoiceSprint is stored as |name|'s voice print.
  Status EnrollSpeaker(const std::string& name);
  Status RemoveSpeaker(const std::string& name);
  Status ListSpeakers(std::vector<std::string>* names);
  Status GetStats(std::vector<std::pair<std::string, uint64_t>>* stats);

 private:
  Status Call(const std::string& member, const Bus::FillFunction& fill,
              const Bus::ReplyReader& read);

  Options options_;
  Bus bus_;
  ResultCallback on_result_;
  bool started_;
};

}  // namespace mw

#endif  // MW_SDK_NPU_CLIENT_H_
