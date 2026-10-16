// Copyright 2026 Genio Middleware Authors.
//
// mw-example-client: reference application and debug tool built on the client
// SDK. It shows both ways an application consumes the middleware:
//
//   * NpuClient    inference results (small metadata, no pixels)
//   * MediaClient  raw camera frames as dma-buf leases (zero copy); a preview
//                  application would import frame.dma_buf_fd() into its
//                  renderer instead of reading frame.data().
//
//   mw-example-client [--bus system|user] [--seconds N] [--no-video]
//                     [--no-results] [--audio] [--save-frame FILE.pgm]
//                     [--list-models] [--model NAME=on|off] [--enroll NAME]
//                     [--remove-speaker NAME] [--list-speakers] [--stats]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "mw/common/logging.h"
#include "mw/common/termination_watcher.h"
#include "mw/common/time_util.h"
#include "mw/sdk/media_client.h"
#include "mw/sdk/npu_client.h"

namespace {

void PrintUsage(const char* program) {
  fprintf(
      stderr,
      "Usage: %s [options]\n"
      "  --bus system|user     bus to use (default system)\n"
      "  --seconds N           run N seconds then exit (default 5, 0 = "
      "until Ctrl-C)\n"
      "  --no-video            do not subscribe to camera frames\n"
      "  --no-results          do not print inference results\n"
      "  --audio               also attach the microphone ring and report "
      "its level\n"
      "  --save-frame FILE     write the first frame's luma as a PGM image\n"
      "  --list-models         list NPU models and exit\n"
      "  --model NAME=on|off   enable or disable a model\n"
      "  --enroll NAME         store the next utterance as NAME's voice "
      "print\n"
      "  --remove-speaker NAME forget a speaker\n"
      "  --list-speakers       list enrolled speakers and exit\n"
      "  --stats               print MediaManager and NpuManager counters\n",
      program);
}

// Saves the frame for inspection: a PGM of the luma plane for NV12 and YUYV,
// the untouched JPEG for MJPG.
bool SaveFrame(const mw::MediaClient::Frame& frame, const std::string& path) {
  const mw::FrameFormat& f = frame.format();
  const uint8_t* data = frame.data();
  if (data == nullptr) {
    return false;
  }
  FILE* out = fopen(path.c_str(), "wb");
  if (out == nullptr) {
    return false;
  }
  frame.BeginCpuRead();
  if (f.fourcc == mw::kFourccMjpeg) {
    fwrite(data, 1, frame.size(), out);
  } else {
    fprintf(out, "P5\n%u %u\n255\n", f.width, f.height);
    std::vector<uint8_t> row(f.width);
    for (uint32_t y = 0; y < f.height; ++y) {
      const uint8_t* src = data + static_cast<size_t>(y) * f.stride;
      if (f.fourcc == mw::kFourccYuyv) {
        for (uint32_t x = 0; x < f.width; ++x) {
          row[x] = src[2 * x];  // Y0 U Y1 V: luma is every other byte.
        }
        fwrite(row.data(), 1, f.width, out);
      } else {
        fwrite(src, 1, f.width, out);
      }
    }
  }
  frame.EndCpuRead();
  fclose(out);
  return true;
}

void PrintStats(const char* title,
                const std::vector<std::pair<std::string, uint64_t>>& stats) {
  printf("%s\n", title);
  for (size_t i = 0; i < stats.size(); ++i) {
    printf("  %-28s %llu\n", stats[i].first.c_str(),
           static_cast<unsigned long long>(stats[i].second));
  }
}

}  // namespace

int main(int argc, char** argv) {
  mw::TerminationWatcher termination;

  std::string bus = "system";
  int seconds = 5;
  bool video = true;
  bool results = true;
  bool audio = false;
  bool list_models = false;
  bool list_speakers = false;
  bool stats = false;
  std::string save_frame;
  std::string model_toggle;
  std::string enroll;
  bool enroll_requested = false;
  std::string remove_speaker;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--bus" && has_value) {
      bus = argv[++i];
    } else if (arg == "--seconds" && has_value) {
      seconds = atoi(argv[++i]);
    } else if (arg == "--no-video") {
      video = false;
    } else if (arg == "--no-results") {
      results = false;
    } else if (arg == "--audio") {
      audio = true;
    } else if (arg == "--save-frame" && has_value) {
      save_frame = argv[++i];
    } else if (arg == "--list-models") {
      list_models = true;
    } else if (arg == "--model" && has_value) {
      model_toggle = argv[++i];
    } else if (arg == "--enroll" && has_value) {
      enroll = argv[++i];
      enroll_requested = true;
    } else if (arg == "--remove-speaker" && has_value) {
      remove_speaker = argv[++i];
    } else if (arg == "--list-speakers") {
      list_speakers = true;
    } else if (arg == "--stats") {
      stats = true;
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }
  const bool control_only = list_models || list_speakers || stats ||
                            !model_toggle.empty() || !remove_speaker.empty();

  // ---- NpuClient: control commands and results ----------------------------
  std::atomic<uint64_t> result_count(0);
  mw::NpuClient::Options npu_options;
  npu_options.bus = bus;
  mw::NpuClient npu(npu_options);
  mw::Status status = npu.Start([&](const mw::InferenceResult& r) {
    ++result_count;
    if (!results) {
      return;
    }
    printf("result %-10s %-14s seq=%llu items=%zu", r.model.c_str(),
           r.kind.c_str(), static_cast<unsigned long long>(r.sequence),
           r.items.size());
    for (size_t i = 0; i < r.items.size() && i < 3; ++i) {
      const mw::ResultItem& item = r.items[i];
      printf("  [%s %.2f", item.label.c_str(), item.score);
      if (item.w > 0) {
        printf(" @%.2f,%.2f %.2fx%.2f", item.x, item.y, item.w, item.h);
      }
      printf("]");
    }
    printf("\n");
    fflush(stdout);
  });
  if (!status.ok()) {
    fprintf(stderr, "cannot connect to the bus: %s\n",
            status.ToString().c_str());
    return 1;
  }

  int exit_code = 0;
  if (list_models) {
    std::vector<mw::NpuClient::ModelInfo> models;
    status = npu.ListModels(&models);
    if (!status.ok()) {
      fprintf(stderr, "ListModels: %s\n", status.ToString().c_str());
      return 1;
    }
    for (size_t i = 0; i < models.size(); ++i) {
      printf("%-12s %-15s %-8s %.1f fps\n", models[i].name.c_str(),
             models[i].kind.c_str(), models[i].enabled ? "enabled" : "disabled",
             models[i].target_fps);
    }
  }
  if (!model_toggle.empty()) {
    const size_t eq = model_toggle.find('=');
    if (eq == std::string::npos || (model_toggle.substr(eq + 1) != "on" &&
                                    model_toggle.substr(eq + 1) != "off")) {
      fprintf(stderr, "--model expects NAME=on or NAME=off\n");
      return 2;
    }
    status = npu.SetModelEnabled(model_toggle.substr(0, eq),
                                 model_toggle.substr(eq + 1) == "on");
    if (!status.ok()) {
      fprintf(stderr, "SetModelEnabled: %s\n", status.ToString().c_str());
      exit_code = 1;
    }
  }
  if (!remove_speaker.empty()) {
    status = npu.RemoveSpeaker(remove_speaker);
    if (!status.ok()) {
      fprintf(stderr, "RemoveSpeaker: %s\n", status.ToString().c_str());
      exit_code = 1;
    }
  }
  if (list_speakers) {
    std::vector<std::string> names;
    status = npu.ListSpeakers(&names);
    if (!status.ok()) {
      fprintf(stderr, "ListSpeakers: %s\n", status.ToString().c_str());
      return 1;
    }
    for (size_t i = 0; i < names.size(); ++i) {
      printf("%s\n", names[i].c_str());
    }
  }
  if (enroll_requested) {
    status = npu.EnrollSpeaker(enroll);
    if (!status.ok()) {
      fprintf(stderr, "EnrollSpeaker: %s\n", status.ToString().c_str());
      return 1;
    }
    printf("speak now: the next utterance becomes the voice print of '%s'\n",
           enroll.c_str());
  }
  if (control_only && !enroll_requested) {
    if (stats) {
      std::vector<std::pair<std::string, uint64_t>> npu_stats;
      if (npu.GetStats(&npu_stats).ok()) {
        PrintStats("NpuManager", npu_stats);
      }
    }
    if (!stats || !video) {
      return exit_code;
    }
  }

  // ---- MediaClient: raw frames and audio ----------------------------------
  std::mutex mutex;
  uint64_t frames = 0;
  uint64_t latency_us_total = 0;
  uint64_t first_sequence = 0;
  uint64_t last_sequence = 0;
  bool saved = save_frame.empty();
  uint64_t t_start = mw::MonotonicNs();

  mw::MediaClient::Options media_options;
  media_options.bus = bus;
  media_options.client_name = "example-client";
  media_options.queue_depth = 2;
  media_options.map_frames = !save_frame.empty();
  media_options.want_audio = audio;
  mw::MediaClient media(media_options);
  if (video) {
    status = media.Start(
        [&](mw::MediaClient::Frame frame) {
          const uint64_t now = mw::MonotonicNs();
          std::lock_guard<std::mutex> lock(mutex);
          if (frames == 0) {
            first_sequence = frame.info().sequence;
            t_start = now;
          }
          ++frames;
          last_sequence = frame.info().sequence;
          if (now > frame.info().timestamp_ns) {
            latency_us_total += (now - frame.info().timestamp_ns) / 1000;
          }
          if (!saved) {
            saved = true;
            if (SaveFrame(frame, save_frame)) {
              printf("saved frame %llu (%ux%u) to %s\n",
                     static_cast<unsigned long long>(frame.info().sequence),
                     frame.format().width, frame.format().height,
                     save_frame.c_str());
            } else {
              fprintf(stderr, "cannot save frame to %s\n", save_frame.c_str());
            }
          }
          // |frame| goes out of scope here and the slot returns to the pool.
        },
        [](bool connected, const mw::MediaClient::VideoInfo& info) {
          if (connected) {
            printf("camera connected: %ux%u, %u buffers\n", info.format.width,
                   info.format.height, info.slot_count);
          } else {
            printf("camera service went away; waiting for it to return\n");
          }
          fflush(stdout);
        });
    if (!status.ok()) {
      fprintf(stderr, "MediaClient: %s\n", status.ToString().c_str());
      return 1;
    }
  }

  // ---- Run -----------------------------------------------------------------
  const uint64_t run_start = mw::MonotonicNs();
  uint64_t audio_cursor = 0;
  bool audio_attached = false;
  while (!termination.Wait(200)) {
    if (seconds > 0 && mw::MonotonicNs() - run_start >
                           static_cast<uint64_t>(seconds) * 1000000000ull) {
      break;
    }
    if (audio && video) {
      std::shared_ptr<mw::AudioRingReader> ring = media.audio_ring();
      if (ring != nullptr) {
        if (!audio_attached) {
          audio_cursor = ring->WritePosition();
          audio_attached = true;
          printf("microphone: %u Hz, %u ch\n", ring->sample_rate(),
                 ring->channels());
        }
        std::vector<int16_t> buffer(4800 * ring->channels());
        uint64_t lost = 0;
        size_t n = ring->Read(&audio_cursor, buffer.data(), 4800, &lost);
        if (n > 0) {
          int peak = 0;
          for (size_t i = 0; i < n * ring->channels(); ++i) {
            peak = std::max(peak, abs(static_cast<int>(buffer[i])));
          }
          printf("audio %zu frames, peak %d\n", n, peak);
        }
      }
    }
  }

  // ---- Summary
  // ---------------------------------------------------------------
  if (video) {
    std::vector<std::pair<std::string, uint64_t>> media_stats;
    if (stats) {
      media.GetStats(&media_stats);
    }
    media.Stop();
    std::lock_guard<std::mutex> lock(mutex);
    const double secs = static_cast<double>(mw::MonotonicNs() - t_start) / 1e9;
    printf(
        "frames: %llu in %.1f s = %.1f fps, avg latency %llu us, "
        "sequence %llu..%llu\n",
        static_cast<unsigned long long>(frames), secs,
        secs > 0 ? static_cast<double>(frames) / secs : 0.0,
        static_cast<unsigned long long>(frames ? latency_us_total / frames : 0),
        static_cast<unsigned long long>(first_sequence),
        static_cast<unsigned long long>(last_sequence));
    if (stats && !media_stats.empty()) {
      PrintStats("MediaManager", media_stats);
    }
  }
  printf("results received: %llu\n",
         static_cast<unsigned long long>(result_count.load()));
  npu.Stop();
  return exit_code;
}
