// Copyright 2026 Genio Middleware Authors.
//
// What to draw on top of the video: bounding boxes from detection models and
// text captions for results without a box (gesture class, speaker name).
// ResultStore keeps the newest result per model; DrawOverlay paints a snapshot
// with cairo. Neither needs a display.

#ifndef MW_VIEWER_OVERLAY_H_
#define MW_VIEWER_OVERLAY_H_

#include <cairo.h>
#include <stdint.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "mw/common/inference_result.h"
#include "mw/common/macros.h"

namespace mw {

struct OverlayBox {
  OverlayBox() : score(0), x(0), y(0), w(0), h(0), color_index(0) {}
  std::string model;
  std::string label;
  double score;
  double x, y, w, h;  // Normalised to the frame, (x, y) = top-left corner.
  int color_index;    // Stable per model; see OverlayColor().
};

struct OverlaySnapshot {
  std::vector<OverlayBox> boxes;
  std::vector<std::string> captions;  // "model: label 0.93" lines.
};

// Newest result per model. A result with no items (the NPU manager sends one
// when a model stops, for example because its gate closed) removes that
// model's drawing at once; anything else disappears when it is older than the
// maximum age. Thread safe: written by the SDK's bus thread, read by the UI.
class ResultStore {
 public:
  // |video_max_age_ns| applies to results tied to a video frame (boxes,
  // gestures); |audio_max_age_ns| to results with sequence 0 (speaker).
  ResultStore(uint64_t video_max_age_ns, uint64_t audio_max_age_ns);

  MW_DISALLOW_COPY_AND_ASSIGN(ResultStore);

  // Gives |model| the next free colour if it has none yet. Calling this with
  // the model list in its configured order keeps colours the same from run
  // to run instead of depending on which result arrives first.
  void Reserve(const std::string& model);

  // Gives the models the colours 0, 1, 2... in |models| order, even when some
  // results arrived (and took a colour) before the list was known. The first
  // call fixes the order; later calls only add models not seen before.
  void SetOrder(const std::vector<std::string>& models);

  void Update(const InferenceResult& result, uint64_t now_ns);
  OverlaySnapshot Snapshot(uint64_t now_ns) const;
  uint64_t updates() const;

 private:
  struct Entry {
    Entry() : received_ns(0), color_index(0) {}
    InferenceResult result;
    uint64_t received_ns;
    int color_index;
  };

  Entry& EntryForLocked(const std::string& model);

  uint64_t video_max_age_ns_;
  uint64_t audio_max_age_ns_;
  mutable std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  int next_color_;
  bool order_set_;
  uint64_t updates_;
};

// Opaque RGB (0..1) of palette entry |index| (wraps around).
void OverlayColor(int index, double* r, double* g, double* b);

// Paints |overlay| into the rectangle (x, y, width, height) of |cr|, which is
// where the video picture was drawn (so normalised boxes map onto it).
void DrawOverlay(cairo_t* cr, double x, double y, double width, double height,
                 const OverlaySnapshot& overlay);

}  // namespace mw

#endif  // MW_VIEWER_OVERLAY_H_
