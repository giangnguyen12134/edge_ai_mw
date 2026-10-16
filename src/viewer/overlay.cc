// Copyright 2026 Genio Middleware Authors.

#include "mw/viewer/overlay.h"

#include <stdio.h>

#include <algorithm>

namespace mw {
namespace {

struct Rgb {
  double r, g, b;
};

// Distinct on dark and bright pictures alike.
const Rgb kPalette[] = {
    {0.10, 0.85, 0.35},  // green
    {1.00, 0.60, 0.00},  // orange
    {0.20, 0.65, 1.00},  // blue
    {1.00, 0.25, 0.55},  // pink
    {0.95, 0.90, 0.15},  // yellow
    {0.65, 0.45, 1.00},  // violet
};
const int kPaletteSize =
    static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));

std::string Describe(const ResultItem& item) {
  char score[16];
  snprintf(score, sizeof(score), "%.2f", item.score);
  return item.label + " " + score;
}

bool HasBox(const ResultItem& item) { return item.w > 0 && item.h > 0; }

}  // namespace

void OverlayColor(int index, double* r, double* g, double* b) {
  const Rgb& c =
      kPalette[((index % kPaletteSize) + kPaletteSize) % kPaletteSize];
  *r = c.r;
  *g = c.g;
  *b = c.b;
}

ResultStore::ResultStore(uint64_t video_max_age_ns, uint64_t audio_max_age_ns)
    : video_max_age_ns_(video_max_age_ns),
      audio_max_age_ns_(audio_max_age_ns),
      next_color_(0),
      order_set_(false),
      updates_(0) {}

ResultStore::Entry& ResultStore::EntryForLocked(const std::string& model) {
  std::map<std::string, Entry>::iterator it = entries_.find(model);
  if (it == entries_.end()) {
    Entry entry;
    entry.color_index = next_color_++;
    it = entries_.insert(std::make_pair(model, entry)).first;
  }
  return it->second;
}

void ResultStore::Reserve(const std::string& model) {
  std::lock_guard<std::mutex> lock(mutex_);
  EntryForLocked(model);
}

void ResultStore::SetOrder(const std::vector<std::string>& models) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < models.size(); ++i) {
    EntryForLocked(models[i]);
  }
  if (order_set_) {
    return;
  }
  order_set_ = true;
  int next = 0;
  for (size_t i = 0; i < models.size(); ++i) {
    entries_[models[i]].color_index = next++;
  }
  for (std::map<std::string, Entry>::iterator it = entries_.begin();
       it != entries_.end(); ++it) {
    if (std::find(models.begin(), models.end(), it->first) == models.end()) {
      it->second.color_index = next++;
    }
  }
  next_color_ = next;
}

void ResultStore::Update(const InferenceResult& result, uint64_t now_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++updates_;
  Entry& entry = EntryForLocked(result.model);
  entry.result = result;
  entry.received_ns = now_ns;
}

uint64_t ResultStore::updates() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return updates_;
}

OverlaySnapshot ResultStore::Snapshot(uint64_t now_ns) const {
  OverlaySnapshot snapshot;
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::map<std::string, Entry>::const_iterator it = entries_.begin();
       it != entries_.end(); ++it) {
    const Entry& entry = it->second;
    const InferenceResult& r = entry.result;
    const uint64_t max_age =
        r.sequence == 0 ? audio_max_age_ns_ : video_max_age_ns_;
    if (now_ns > entry.received_ns && now_ns - entry.received_ns > max_age) {
      continue;
    }
    for (size_t i = 0; i < r.items.size(); ++i) {
      const ResultItem& item = r.items[i];
      if (HasBox(item)) {
        OverlayBox box;
        box.model = r.model;
        box.label = item.label;
        box.score = item.score;
        box.x = item.x;
        box.y = item.y;
        box.w = item.w;
        box.h = item.h;
        box.color_index = entry.color_index;
        snapshot.boxes.push_back(box);
      } else {
        snapshot.captions.push_back(r.model + ": " + Describe(item));
      }
    }
  }
  return snapshot;
}

void DrawOverlay(cairo_t* cr, double x, double y, double width, double height,
                 const OverlaySnapshot& overlay) {
  cairo_save(cr);
  cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 14.0);
  cairo_font_extents_t font;
  cairo_font_extents(cr, &font);
  const double line = 2.5;

  // Outlines first, so no label chip is ever covered by another box's line.
  for (size_t i = 0; i < overlay.boxes.size(); ++i) {
    const OverlayBox& box = overlay.boxes[i];
    double r, g, b;
    OverlayColor(box.color_index, &r, &g, &b);
    cairo_set_line_width(cr, line);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_rectangle(cr, x + box.x * width, y + box.y * height, box.w * width,
                    box.h * height);
    cairo_stroke(cr);
  }

  // Label chips sit above their box (inside it when there is no room) and
  // are moved down when they would cover an earlier chip.
  struct Chip {
    double x, y, w, h;
  };
  std::vector<Chip> placed;
  for (size_t i = 0; i < overlay.boxes.size(); ++i) {
    const OverlayBox& box = overlay.boxes[i];
    double r, g, b;
    OverlayColor(box.color_index, &r, &g, &b);
    const std::string described =
        Describe(ResultItem(box.label, box.score, 0, 0, 0, 0));
    const std::string text =
        box.label == box.model ? described : box.model + ": " + described;
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text.c_str(), &extents);
    Chip chip;
    chip.w = extents.x_advance + 8.0;
    chip.h = font.height + 4.0;
    chip.x = x + box.x * width - line / 2;
    const double top = y + box.y * height;
    chip.y = (top - chip.h >= y) ? top - chip.h : top;
    for (int attempt = 0; attempt < 16; ++attempt) {
      bool overlaps = false;
      for (size_t j = 0; j < placed.size(); ++j) {
        const Chip& o = placed[j];
        if (chip.x < o.x + o.w && o.x < chip.x + chip.w && chip.y < o.y + o.h &&
            o.y < chip.y + chip.h) {
          overlaps = true;
          chip.y = o.y + o.h + 1.0;
          break;
        }
      }
      if (!overlaps) {
        break;
      }
    }
    placed.push_back(chip);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_rectangle(cr, chip.x, chip.y, chip.w, chip.h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_move_to(cr, chip.x + 4.0, chip.y + 2.0 + font.ascent);
    cairo_show_text(cr, text.c_str());
  }

  double cy = y + 8.0;
  for (size_t i = 0; i < overlay.captions.size(); ++i) {
    const std::string& text = overlay.captions[i];
    cairo_text_extents_t extents;
    cairo_text_extents(cr, text.c_str(), &extents);
    const double chip_h = font.height + 4.0;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
    cairo_rectangle(cr, x + 8.0, cy, extents.x_advance + 10.0, chip_h);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, x + 13.0, cy + 2.0 + font.ascent);
    cairo_show_text(cr, text.c_str());
    cy += chip_h + 3.0;
  }
  cairo_restore(cr);
}

}  // namespace mw
