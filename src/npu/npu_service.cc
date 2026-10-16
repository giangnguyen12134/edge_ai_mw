// Copyright 2026 Genio Middleware Authors.

#include "mw/npu/npu_service.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <chrono>

#include "mw/common/logging.h"
#include "mw/common/time_util.h"
#include "mw/ipc/dbus_names.h"
#include "mw/ipc/introspection.h"
#include "mw/ipc/result_codec.h"

namespace mw {
namespace {

const uint32_t kVadBlockMs = 20;      // Analysis block of the energy VAD.
const uint32_t kVadHangoverMs = 300;  // Silence that ends an utterance.
const uint64_t kEnrollTimeoutNs = 30ull * 1000000000ull;
const uint64_t kHealthyWithinNs = 5ull * 1000000000ull;
const size_t kMaxSpeakerNameLength = 64;
const size_t kAudioReadFrames = 960;  // 60 ms at 16 kHz per read.

MediaClient::Options MakeMediaOptions(const NpuConfig& config) {
  MediaClient::Options options;
  options.bus = config.bus;
  options.media_bus_name = config.media_bus_name;
  options.client_name = "npu-manager";
  options.queue_depth = config.media_queue_depth;
  options.map_frames = true;  // The CPU reference scaler reads the frame.
  for (size_t i = 0; i < config.models.size(); ++i) {
    if (config.models[i].kind == "speaker") {
      options.want_audio = true;
    }
  }
  return options;
}

bool ValidSpeakerName(const std::string& name) {
  if (name.empty() || name.size() > kMaxSpeakerNameLength) {
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    if (static_cast<unsigned char>(name[i]) < 0x20) {
      return false;
    }
  }
  return true;
}

}  // namespace

NpuService::ModelState::ModelState(const ModelConfig& model_config)
    : config(model_config),
      loaded(false),
      enabled(model_config.enabled),
      have_run(false),
      last_run_ns(0),
      have_sequence(false),
      last_sequence(0),
      active(false),
      last_hit_ns(0),
      runs(0),
      errors(0),
      infer_us_total(0),
      latency_us_total(0) {}

NpuService::NpuService(const NpuConfig& config)
    : config_(config),
      media_(MakeMediaOptions(config)),
      speakers_(config.speaker_db_path),
      thermal_(config.thermal_path, config.throttle_temp_c,
               config.throttle_rate_scale),
      speaker_model_(nullptr),
      stop_(false),
      started_(false),
      heartbeat_ns_(0),
      frame_dirty_(false),
      warned_unsupported_format_(false),
      audio_unusable_(false),
      audio_cursor_(0),
      trailing_silence_ms_(0),
      in_speech_(false),
      enroll_deadline_ns_(0),
      frames_received_(0),
      frames_replaced_(0),
      utterances_(0),
      audio_lost_frames_(0),
      results_published_(0),
      throttled_loops_(0),
      temp_milli_c_(-1),
      thermal_throttled_(false),
      start_ns_(0) {}

NpuService::~NpuService() { Stop(); }

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

Status NpuService::LoadModels() {
  MW_RETURN_IF_ERROR(CreateBufferAllocator(config_.allocator, &allocator_));
  MW_RETURN_IF_ERROR(CreateNpuRuntime(config_, &runtime_));
  MW_RETURN_IF_ERROR(CreateImageScaler("cpu", &scaler_));
  MW_RETURN_IF_ERROR(speakers_.Load());
  MW_LOG(Info) << "NPU runtime: " << runtime_->Describe()
               << ", scaler: " << scaler_->Describe()
               << ", buffers: " << allocator_->Describe();

  std::vector<ModelConfig> sorted = config_.models;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const ModelConfig& a, const ModelConfig& b) {
                     return a.priority > b.priority;
                   });
  size_t loaded = 0;
  for (size_t i = 0; i < sorted.size(); ++i) {
    std::unique_ptr<ModelState> m(new ModelState(sorted[i]));
    m->spec = InputSpecFromConfig(m->config);
    Status status = runtime_->Load(m->config, &m->model);
    if (status.ok()) {
      status = m->input.Allocate(allocator_.get(), m->spec.bytes());
    }
    if (status.ok() && m->config.is_video()) {
      status = CreatePostprocessor(m->config, &m->post);
    }
    if (status.ok()) {
      m->loaded = true;
      ++loaded;
      if (m->config.kind == "speaker") {
        speaker_model_ = m.get();
      }
      MW_LOG(Info) << "model '" << m->config.name << "' (" << m->config.kind
                   << ") loaded, input " << m->spec.bytes() << " bytes"
                   << (m->enabled ? "" : ", disabled");
    } else {
      // One broken model must not take the others down with it.
      m->enabled = false;
      MW_LOG(Error) << "model '" << m->config.name
                    << "' failed to load: " << status.ToString();
    }
    models_.push_back(std::move(m));
  }
  if (loaded == 0) {
    return FailedPreconditionError("no model could be loaded");
  }
  return Status::Ok();
}

Status NpuService::Start() {
  if (started_) {
    return FailedPreconditionError("NpuService already started");
  }
  MW_RETURN_IF_ERROR(ValidateNpuConfig(config_));
  start_ns_ = MonotonicNs();
  MW_RETURN_IF_ERROR(LoadModels());

  BusType type;
  MW_RETURN_IF_ERROR(ParseBusType(config_.bus, &type));
  MW_RETURN_IF_ERROR(bus_.Connect(type, dbus::kNpuBusName));
  MW_RETURN_IF_ERROR(bus_.AddObject(
      dbus::kNpuObjectPath, dbus::kNpuInterface, dbus::kNpuIntrospectionXml,
      [this](const char* member, sd_bus_message* call) {
        OnMethodCall(member, call);
      }));
  MW_RETURN_IF_ERROR(bus_.Start());

  stop_ = false;
  started_ = true;
  heartbeat_ns_ = MonotonicNs();
  worker_ = std::thread(&NpuService::InferenceLoop, this);

  const Status media_status = media_.Start(
      [this](MediaClient::Frame frame) { OnFrame(std::move(frame)); }, nullptr);
  if (!media_status.ok()) {
    Stop();
    return media_status;
  }
  return Status::Ok();
}

void NpuService::Stop() {
  if (!started_) {
    return;
  }
  stop_ = true;
  frame_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  media_.Stop();
  {
    std::shared_ptr<MediaClient::Frame> dropped;
    std::lock_guard<std::mutex> lock(frame_mutex_);
    dropped.swap(latest_frame_);
  }
  bus_.Stop();
  started_ = false;
}

bool NpuService::Healthy() const {
  if (!started_) {
    return false;
  }
  const uint64_t now = MonotonicNs();
  const uint64_t beat = heartbeat_ns_.load();
  return now < beat || now - beat < kHealthyWithinNs;
}

NpuService::ModelState* NpuService::FindModel(const std::string& name) const {
  for (size_t i = 0; i < models_.size(); ++i) {
    if (models_[i]->config.name == name) {
      return models_[i].get();
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Frames and the inference loop
// ---------------------------------------------------------------------------

void NpuService::OnFrame(MediaClient::Frame frame) {
  std::shared_ptr<MediaClient::Frame> fresh(
      new MediaClient::Frame(std::move(frame)));
  std::shared_ptr<MediaClient::Frame> old;
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    old.swap(latest_frame_);
    latest_frame_ = fresh;
    frame_dirty_ = true;
  }
  ++frames_received_;
  if (old != nullptr) {
    ++frames_replaced_;  // Inference was not ready: older frame discarded.
  }
  old.reset();  // Return the slot outside the lock.
  frame_cv_.notify_one();
}

void NpuService::InferenceLoop() {
  SetCurrentThreadName("mw-npu");
  // With audio the loop must wake often enough to keep VAD latency low.
  const std::chrono::milliseconds idle_wait(speaker_model_ != nullptr ? 5 : 50);
  while (!stop_) {
    heartbeat_ns_ = MonotonicNs();
    std::shared_ptr<MediaClient::Frame> frame;
    {
      std::unique_lock<std::mutex> lock(frame_mutex_);
      frame_cv_.wait_for(lock, idle_wait,
                         [this] { return stop_ || frame_dirty_; });
      if (stop_) {
        break;
      }
      if (frame_dirty_) {
        frame = latest_frame_;
        frame_dirty_ = false;
      }
    }
    if (frame != nullptr && frame->valid()) {
      RunVideoModels(*frame);
    }
    frame.reset();
    RunSpeaker();
  }
}

bool NpuService::GateOpen(const ModelState& m, uint64_t now_ns) const {
  if (m.config.gated_by.empty()) {
    return true;
  }
  const ModelState* gate = FindModel(m.config.gated_by);
  if (gate == nullptr || !gate->loaded || !gate->enabled) {
    return false;
  }
  const uint64_t hit = gate->last_hit_ns.load();
  if (hit == 0) {
    return false;
  }
  const uint64_t hold_ns =
      static_cast<uint64_t>(m.config.gate_hold_ms) * 1000000ull;
  return hit > now_ns || now_ns - hit <= hold_ns;
}

void NpuService::RunVideoModels(const MediaClient::Frame& frame) {
  const double scale = thermal_.RateScale();
  temp_milli_c_ = static_cast<int64_t>(thermal_.last_temp_c() * 1000.0);
  thermal_throttled_ = thermal_.throttled();
  if (scale < 1.0) {
    ++throttled_loops_;
  }

  // The CPU scaler reads NV12 and YUYV. MJPG frames need a decoder first
  // (a preprocessing stage that does not exist yet).
  const uint32_t fourcc = frame.format().fourcc;
  if (fourcc != kFourccNv12 && fourcc != kFourccYuyv) {
    if (!warned_unsupported_format_) {
      warned_unsupported_format_ = true;
      MW_LOG(Warning)
          << "camera delivers " << FourccToString(frame.format().fourcc)
          << "; video models need NV12 or YUYV and are idle until a "
             "preprocessing stage exists (workaround: set "
             "video.pool_format=nv12 in MediaManager)";
    }
    return;
  }
  SourceImage source;
  source.data = frame.data();
  source.dma_buf_fd = frame.dma_buf_fd();
  source.format = frame.format();

  bool cpu_read_open = false;
  for (size_t i = 0; i < models_.size(); ++i) {
    ModelState* m = models_[i].get();
    if (!m->loaded || !m->config.is_video()) {
      continue;
    }
    const uint64_t now = MonotonicNs();
    const bool runnable = m->enabled && GateOpen(*m, now);
    if (!runnable) {
      if (m->active) {
        // Tell listeners the previous detections are no longer valid.
        m->active = false;
        InferenceResult cleared;
        cleared.model = m->config.name;
        cleared.kind = m->config.kind;
        cleared.sequence = frame.info().sequence;
        cleared.timestamp_ns = frame.info().timestamp_ns;
        Publish(cleared);
      }
      continue;
    }
    const double period_s = 1.0 / (m->config.target_fps * scale);
    const uint64_t period_ns = static_cast<uint64_t>(period_s * 1e9);
    if (m->have_run && now - m->last_run_ns < period_ns) {
      continue;
    }
    if (m->have_sequence && m->last_sequence == frame.info().sequence) {
      continue;
    }
    if (!cpu_read_open) {
      frame.BeginCpuRead();
      cpu_read_open = true;
    }
    m->have_run = true;
    m->last_run_ns = now;
    m->have_sequence = true;
    m->last_sequence = frame.info().sequence;
    RunImageModel(m, frame, source);
  }
  if (cpu_read_open) {
    frame.EndCpuRead();
  }
}

void NpuService::RunImageModel(ModelState* m, const MediaClient::Frame& frame,
                               const SourceImage& source) {
  const uint64_t t0 = MonotonicNs();
  LetterboxInfo geometry;
  std::vector<OutputTensor> outputs;
  std::vector<ResultItem> items;
  Status status =
      scaler_->Convert(source, m->config, m->spec, &m->input, &geometry);
  if (status.ok()) {
    status = m->model->Run(m->input, &outputs);
  }
  if (status.ok()) {
    status = m->post->Process(outputs, geometry, &items);
  }
  const uint64_t t1 = MonotonicNs();
  if (!status.ok()) {
    LogModelError(m, status);
    return;
  }
  ++m->runs;
  m->infer_us_total += (t1 - t0) / 1000;
  if (t1 > frame.info().timestamp_ns) {
    m->latency_us_total += (t1 - frame.info().timestamp_ns) / 1000;
  }
  if (!items.empty()) {
    m->last_hit_ns = t1;
  }
  m->active = true;

  InferenceResult result;
  result.model = m->config.name;
  result.kind = m->config.kind;
  result.sequence = frame.info().sequence;
  result.timestamp_ns = frame.info().timestamp_ns;
  result.items.swap(items);
  Publish(result);
}

void NpuService::LogModelError(ModelState* m, const Status& status) {
  const uint64_t count = ++m->errors;
  if (count == 1 || count % 100 == 0) {
    MW_LOG(Error) << "model '" << m->config.name << "' failed (" << count
                  << " so far): " << status.ToString();
  }
}

void NpuService::Publish(const InferenceResult& result) {
  ++results_published_;
  bus_.EmitSignal("", dbus::kNpuObjectPath, dbus::kNpuInterface,
                  dbus::kNpuResult, [result](sd_bus_message* message) {
                    return AppendInferenceResult(message, result);
                  });
}

// ---------------------------------------------------------------------------
// VoiceSprint: energy VAD -> utterance -> speaker embedding -> match/enroll
// ---------------------------------------------------------------------------

void NpuService::RunSpeaker() {
  ModelState* m = speaker_model_;
  if (m == nullptr || audio_unusable_) {
    return;
  }
  // Expire a stale enrollment request even when no audio arrives.
  {
    std::lock_guard<std::mutex> lock(enroll_mutex_);
    if (!enroll_name_.empty() && MonotonicNs() > enroll_deadline_ns_) {
      MW_LOG(Warning) << "enrollment of '" << enroll_name_
                      << "' timed out without an utterance";
      enroll_name_.clear();
    }
  }
  std::shared_ptr<AudioRingReader> ring = media_.audio_ring();
  if (ring == nullptr) {
    audio_ring_.reset();
    return;
  }
  if (ring != audio_ring_) {
    if (ring->sample_rate() != m->config.sample_rate) {
      MW_LOG(Error) << "microphone runs at " << ring->sample_rate()
                    << " Hz but model '" << m->config.name << "' needs "
                    << m->config.sample_rate
                    << " Hz; VoiceSprint disabled (set audio.sample_rate or "
                       "the model sample_rate to match)";
      audio_unusable_ = true;
      return;
    }
    audio_ring_ = ring;
    audio_cursor_ = ring->WritePosition();  // Ignore audio from before now.
    block_.clear();
    segment_.clear();
    in_speech_ = false;
    trailing_silence_ms_ = 0;
  }
  if (!m->enabled) {
    audio_cursor_ = ring->WritePosition();
    block_.clear();
    segment_.clear();
    in_speech_ = false;
    return;
  }
  const uint32_t channels = ring->channels();
  std::vector<int16_t> buffer(kAudioReadFrames * channels);
  uint64_t lost = 0;
  size_t frames = 0;
  while ((frames = ring->Read(&audio_cursor_, buffer.data(), kAudioReadFrames,
                              &lost)) > 0) {
    FeedAudio(buffer.data(), frames, channels);
  }
  if (lost > 0) {
    audio_lost_frames_ += lost;
    // The stream has a hole: do not stitch two pieces into one utterance.
    block_.clear();
    segment_.clear();
    in_speech_ = false;
    trailing_silence_ms_ = 0;
  }
}

void NpuService::FeedAudio(const int16_t* samples, size_t frames,
                           uint32_t channels) {
  const ModelConfig& cfg = speaker_model_->config;
  const size_t block_samples =
      static_cast<size_t>(cfg.sample_rate) * kVadBlockMs / 1000;
  const size_t window_samples = speaker_model_->spec.width;
  for (size_t i = 0; i < frames; ++i) {
    // Channel 0 is used; the model is mono.
    block_.push_back(static_cast<float>(samples[i * channels]) / 32768.0f);
    if (block_.size() < block_samples) {
      continue;
    }
    double energy = 0;
    for (size_t k = 0; k < block_.size(); ++k) {
      energy += static_cast<double>(block_[k]) * block_[k];
    }
    energy /= static_cast<double>(block_.size());
    const double level_db = 10.0 * log10(energy + 1e-12);
    const bool speech = level_db > cfg.vad_threshold_db;
    if (speech) {
      in_speech_ = true;
      trailing_silence_ms_ = 0;
      segment_.insert(segment_.end(), block_.begin(), block_.end());
    } else if (in_speech_) {
      trailing_silence_ms_ += kVadBlockMs;
      segment_.insert(segment_.end(), block_.begin(), block_.end());
    }
    block_.clear();
    if (in_speech_ && (trailing_silence_ms_ >= kVadHangoverMs ||
                       segment_.size() >= window_samples)) {
      FinishUtterance();
    }
  }
}

void NpuService::FinishUtterance() {
  const ModelConfig& cfg = speaker_model_->config;
  const size_t trailing_samples =
      static_cast<size_t>(trailing_silence_ms_) * cfg.sample_rate / 1000;
  const size_t speech_samples = segment_.size() > trailing_samples
                                    ? segment_.size() - trailing_samples
                                    : 0;
  const uint64_t speech_ms =
      static_cast<uint64_t>(speech_samples) * 1000 / cfg.sample_rate;
  std::vector<float> pcm;
  pcm.swap(segment_);
  in_speech_ = false;
  trailing_silence_ms_ = 0;
  if (speech_ms >= cfg.min_speech_ms) {
    ProcessUtterance(pcm);
  }
}

void NpuService::ProcessUtterance(const std::vector<float>& pcm) {
  ModelState* m = speaker_model_;
  ++utterances_;
  const uint64_t t0 = MonotonicNs();

  const size_t window = m->spec.width;
  const size_t copy = std::min(pcm.size(), window);
  m->input.BeginCpuWrite();
  float* dst = reinterpret_cast<float*>(m->input.data());
  memcpy(dst, pcm.data(), copy * sizeof(float));
  if (copy < window) {
    memset(dst + copy, 0, (window - copy) * sizeof(float));
  }
  m->input.EndCpuWrite();

  std::vector<OutputTensor> outputs;
  Status status = m->model->Run(m->input, &outputs);
  if (status.ok() && (outputs.empty() || outputs[0].data.empty())) {
    status = InternalError("speaker model produced no embedding");
  }
  std::vector<float> embedding;
  if (status.ok()) {
    embedding = outputs[0].data;
    if (!L2Normalize(&embedding)) {
      status = InternalError("speaker embedding is all zeros");
    }
  }
  if (!status.ok()) {
    LogModelError(m, status);
    return;
  }
  const uint64_t t1 = MonotonicNs();
  ++m->runs;
  m->infer_us_total += (t1 - t0) / 1000;

  std::string label;
  float score = 1.0f;
  std::string enroll_name;
  {
    std::lock_guard<std::mutex> lock(enroll_mutex_);
    if (!enroll_name_.empty()) {
      enroll_name.swap(enroll_name_);
    }
  }
  if (!enroll_name.empty()) {
    const Status stored = speakers_.Add(enroll_name, embedding);
    if (!stored.ok()) {
      MW_LOG(Error) << "cannot store voice print: " << stored.ToString();
      return;
    }
    MW_LOG(Info) << "enrolled speaker '" << enroll_name << "'";
    label = "enrolled:" + enroll_name;
  } else {
    std::string name;
    speakers_.Match(embedding, &name, &score);
    label = score >= m->config.match_threshold ? name : "unknown";
    if (label != "unknown") {
      m->last_hit_ns = t1;
    }
  }
  InferenceResult result;
  result.model = m->config.name;
  result.kind = "speaker";
  result.items.push_back(ResultItem(label, score, 0, 0, 0, 0));
  Publish(result);
}

// ---------------------------------------------------------------------------
// D-Bus interface
// ---------------------------------------------------------------------------

void NpuService::OnMethodCall(const char* member, sd_bus_message* call) {
  if (strcmp(member, dbus::kNpuListModels) == 0) {
    HandleListModels(call);
  } else if (strcmp(member, dbus::kNpuSetModelEnabled) == 0) {
    HandleSetModelEnabled(call);
  } else if (strcmp(member, dbus::kNpuEnrollSpeaker) == 0) {
    HandleEnrollSpeaker(call);
  } else if (strcmp(member, dbus::kNpuRemoveSpeaker) == 0) {
    HandleRemoveSpeaker(call);
  } else if (strcmp(member, dbus::kNpuListSpeakers) == 0) {
    HandleListSpeakers(call);
  } else if (strcmp(member, dbus::kNpuGetStats) == 0) {
    HandleGetStats(call);
  } else {
    sd_bus_reply_method_errorf(call, SD_BUS_ERROR_UNKNOWN_METHOD,
                               "Unknown method '%s'", member);
  }
}

void NpuService::HandleListModels(sd_bus_message* call) {
  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(call, &reply);
  if (r >= 0) {
    r = sd_bus_message_open_container(reply, 'a', "(ssbd)");
  }
  for (size_t i = 0; r >= 0 && i < models_.size(); ++i) {
    const ModelState& m = *models_[i];
    r = sd_bus_message_append(reply, "(ssbd)", m.config.name.c_str(),
                              m.config.kind.c_str(), m.enabled ? 1 : 0,
                              m.config.is_video() ? m.config.target_fps : 0.0);
  }
  if (r >= 0) {
    r = sd_bus_message_close_container(reply);
  }
  if (r >= 0) {
    r = sd_bus_send(nullptr, reply, nullptr);
  }
  sd_bus_message_unref(reply);
  if (r < 0) {
    Bus::ReplyStatus(call, ErrnoStatus("build ListModels reply", -r));
  }
}

void NpuService::HandleSetModelEnabled(sd_bus_message* call) {
  const char* name = nullptr;
  int enabled = 0;
  if (sd_bus_message_read(call, "sb", &name, &enabled) < 0) {
    Bus::ReplyStatus(call, InvalidArgumentError("expected (string, bool)"));
    return;
  }
  ModelState* m = FindModel(name);
  if (m == nullptr) {
    Bus::ReplyStatus(call,
                     NotFoundError(std::string("no model '") + name + "'"));
    return;
  }
  if (!m->loaded) {
    Bus::ReplyStatus(call, FailedPreconditionError(
                               std::string("model '") + name +
                               "' failed to load and cannot be enabled"));
    return;
  }
  m->enabled = enabled != 0;
  MW_LOG(Info) << "model '" << name << "' "
               << (enabled ? "enabled" : "disabled");
  Bus::ReplyOk(call);
}

void NpuService::HandleEnrollSpeaker(sd_bus_message* call) {
  const char* name = nullptr;
  if (sd_bus_message_read(call, "s", &name) < 0) {
    Bus::ReplyStatus(call, InvalidArgumentError("expected (string)"));
    return;
  }
  if (!ValidSpeakerName(name)) {
    Bus::ReplyStatus(call, InvalidArgumentError(
                               "speaker name must be 1..64 printable chars"));
    return;
  }
  if (speaker_model_ == nullptr || !speaker_model_->enabled ||
      audio_unusable_) {
    Bus::ReplyStatus(
        call, FailedPreconditionError("no usable speaker model is running"));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(enroll_mutex_);
    enroll_name_ = name;
    enroll_deadline_ns_ = MonotonicNs() + kEnrollTimeoutNs;
  }
  MW_LOG(Info) << "enrollment armed for '" << name << "'";
  Bus::ReplyOk(call);
}

void NpuService::HandleRemoveSpeaker(sd_bus_message* call) {
  const char* name = nullptr;
  if (sd_bus_message_read(call, "s", &name) < 0) {
    Bus::ReplyStatus(call, InvalidArgumentError("expected (string)"));
    return;
  }
  Bus::ReplyStatus(call, speakers_.Remove(name));
}

void NpuService::HandleListSpeakers(sd_bus_message* call) {
  const std::vector<std::string> names = speakers_.Names();
  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(call, &reply);
  if (r >= 0) {
    r = sd_bus_message_open_container(reply, 'a', "s");
  }
  for (size_t i = 0; r >= 0 && i < names.size(); ++i) {
    r = sd_bus_message_append(reply, "s", names[i].c_str());
  }
  if (r >= 0) {
    r = sd_bus_message_close_container(reply);
  }
  if (r >= 0) {
    r = sd_bus_send(nullptr, reply, nullptr);
  }
  sd_bus_message_unref(reply);
  if (r < 0) {
    Bus::ReplyStatus(call, ErrnoStatus("build ListSpeakers reply", -r));
  }
}

void NpuService::HandleGetStats(sd_bus_message* call) {
  std::vector<std::pair<std::string, uint64_t>> stats;
  stats.push_back(std::make_pair(std::string("uptime_s"),
                                 (MonotonicNs() - start_ns_) / 1000000000ull));
  stats.push_back(
      std::make_pair(std::string("frames_received"), frames_received_.load()));
  stats.push_back(
      std::make_pair(std::string("frames_replaced"), frames_replaced_.load()));
  stats.push_back(std::make_pair(std::string("results_published"),
                                 results_published_.load()));
  stats.push_back(
      std::make_pair(std::string("utterances"), utterances_.load()));
  stats.push_back(std::make_pair(std::string("audio_lost_frames"),
                                 audio_lost_frames_.load()));
  stats.push_back(
      std::make_pair(std::string("throttled_loops"), throttled_loops_.load()));
  stats.push_back(
      std::make_pair(std::string("thermal_throttled"),
                     static_cast<uint64_t>(thermal_throttled_ ? 1 : 0)));
  const int64_t temp = temp_milli_c_.load();
  if (temp >= 0) {
    stats.push_back(std::make_pair(std::string("soc_temp_millicelsius"),
                                   static_cast<uint64_t>(temp)));
  }
  for (size_t i = 0; i < models_.size(); ++i) {
    const ModelState& m = *models_[i];
    const std::string prefix = m.config.name + ".";
    const uint64_t runs = m.runs.load();
    stats.push_back(std::make_pair(prefix + "runs", runs));
    stats.push_back(std::make_pair(prefix + "errors", m.errors.load()));
    stats.push_back(
        std::make_pair(prefix + "avg_infer_us",
                       runs > 0 ? m.infer_us_total.load() / runs : 0));
    stats.push_back(
        std::make_pair(prefix + "avg_latency_us",
                       runs > 0 ? m.latency_us_total.load() / runs : 0));
  }

  sd_bus_message* reply = nullptr;
  int r = sd_bus_message_new_method_return(call, &reply);
  if (r >= 0) {
    r = sd_bus_message_open_container(reply, 'a', "(st)");
  }
  for (size_t i = 0; r >= 0 && i < stats.size(); ++i) {
    r = sd_bus_message_append(reply, "(st)", stats[i].first.c_str(),
                              stats[i].second);
  }
  if (r >= 0) {
    r = sd_bus_message_close_container(reply);
  }
  if (r >= 0) {
    r = sd_bus_send(nullptr, reply, nullptr);
  }
  sd_bus_message_unref(reply);
  if (r < 0) {
    Bus::ReplyStatus(call, ErrnoStatus("build GetStats reply", -r));
  }
}

}  // namespace mw
