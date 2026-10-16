// Copyright 2026 Genio Middleware Authors.
//
// mw-viewer: GTK3 live view of the camera frames shared by the MediaManager
// with the NPU manager's detections drawn on top.
//
//   mw-viewer [--bus system|user] [--no-npu] [--fullscreen]
//   mw-viewer --snapshot FILE.png [--wait-frames N] [--timeout S]
//             [--expect-boxes]       (headless: no display needed)
//
// Keys: f = fullscreen, q / Esc = quit. The check boxes in the tool bar turn
// NPU models on and off while the NpuManager runs.

#include <cairo.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "mw/common/frame_types.h"
#include "mw/common/logging.h"
#include "mw/viewer/viewer_core.h"

namespace {

struct App {
  App()
      : window(nullptr),
        drawing(nullptr),
        status_label(nullptr),
        model_box(nullptr),
        redraw_pending(false),
        updating_checks(false),
        fullscreen(false) {}
  mw::ViewerCore* core = nullptr;
  GtkWidget* window;
  GtkWidget* drawing;
  GtkWidget* status_label;
  GtkWidget* model_box;
  std::atomic<bool> redraw_pending;
  bool updating_checks;
  bool fullscreen;
  std::vector<std::string> shown_models;
};

// Largest rectangle with the picture's aspect ratio that fits (aw x ah).
void FitRect(double aw, double ah, double iw, double ih, double* x, double* y,
             double* w, double* h) {
  const double scale = std::min(aw / iw, ah / ih);
  *w = iw * scale;
  *h = ih * scale;
  *x = (aw - *w) / 2;
  *y = (ah - *h) / 2;
}

// Draws the picture, then the detections, into (0, 0, aw, ah) of |cr|.
void DrawScene(cairo_t* cr, double aw, double ah, const mw::ViewerCore& core,
               const std::string& waiting_text) {
  cairo_set_source_rgb(cr, 0.07, 0.07, 0.08);
  cairo_paint(cr);
  const std::shared_ptr<const mw::BgraImage> image = core.latest_image();
  if (image == nullptr || image->width == 0 || image->height == 0) {
    cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 18.0);
    cairo_text_extents_t e;
    cairo_text_extents(cr, waiting_text.c_str(), &e);
    cairo_move_to(cr, (aw - e.x_advance) / 2, ah / 2);
    cairo_show_text(cr, waiting_text.c_str());
    return;
  }
  double x, y, w, h;
  FitRect(aw, ah, image->width, image->height, &x, &y, &w, &h);
  cairo_surface_t* surface = cairo_image_surface_create_for_data(
      const_cast<uint8_t*>(image->pixels.data()), CAIRO_FORMAT_RGB24,
      static_cast<int>(image->width), static_cast<int>(image->height),
      static_cast<int>(image->stride()));
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, w / image->width, h / image->height);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_restore(cr);
  cairo_surface_destroy(surface);
  mw::DrawOverlay(cr, x, y, w, h, core.overlay());
}

std::string WaitingText(const mw::ViewerCore::Stats& stats) {
  return stats.video_connected ? "Waiting for the first frame..."
                               : "Waiting for the MediaManager...";
}

gboolean OnDraw(GtkWidget* widget, cairo_t* cr, gpointer data) {
  App* app = static_cast<App*>(data);
  const mw::ViewerCore::Stats stats = app->core->stats();
  DrawScene(cr, gtk_widget_get_allocated_width(widget),
            gtk_widget_get_allocated_height(widget), *app->core,
            WaitingText(stats));
  return FALSE;
}

gboolean OnFrameIdle(gpointer data) {
  App* app = static_cast<App*>(data);
  app->redraw_pending = false;
  gtk_widget_queue_draw(app->drawing);
  return G_SOURCE_REMOVE;
}

void OnModelToggled(GtkToggleButton* button, gpointer data) {
  App* app = static_cast<App*>(data);
  if (app->updating_checks) {
    return;
  }
  const char* name =
      static_cast<const char*>(g_object_get_data(G_OBJECT(button), "model"));
  if (name != nullptr) {
    app->core->SetModelEnabled(name, gtk_toggle_button_get_active(button));
  }
}

void RebuildModelChecks(App* app,
                        const std::vector<mw::NpuClient::ModelInfo>& models) {
  std::vector<std::string> names;
  for (size_t i = 0; i < models.size(); ++i) {
    // Audio models have nothing to draw; the viewer toggles video models.
    if (models[i].kind != "speaker") {
      names.push_back(models[i].name);
    }
  }
  app->updating_checks = true;
  if (names != app->shown_models) {
    GList* children = gtk_container_get_children(GTK_CONTAINER(app->model_box));
    for (GList* c = children; c != nullptr; c = c->next) {
      gtk_widget_destroy(GTK_WIDGET(c->data));
    }
    g_list_free(children);
    for (size_t i = 0; i < names.size(); ++i) {
      GtkWidget* check = gtk_check_button_new_with_label(names[i].c_str());
      g_object_set_data_full(G_OBJECT(check), "model",
                             g_strdup(names[i].c_str()), g_free);
      g_signal_connect(check, "toggled", G_CALLBACK(OnModelToggled), app);
      gtk_box_pack_start(GTK_BOX(app->model_box), check, FALSE, FALSE, 4);
      gtk_widget_show(check);
    }
    app->shown_models = names;
  }
  GList* children = gtk_container_get_children(GTK_CONTAINER(app->model_box));
  for (GList* c = children; c != nullptr; c = c->next) {
    const char* name =
        static_cast<const char*>(g_object_get_data(G_OBJECT(c->data), "model"));
    for (size_t i = 0; name != nullptr && i < models.size(); ++i) {
      if (models[i].name == name) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->data),
                                     models[i].enabled);
      }
    }
  }
  g_list_free(children);
  app->updating_checks = false;
}

// Twice a second: status line, model check boxes, and a redraw so results
// expire on screen even when no new frame arrives.
gboolean OnTick(gpointer data) {
  App* app = static_cast<App*>(data);
  const mw::ViewerCore::Stats stats = app->core->stats();
  char text[256];
  if (stats.video_connected) {
    snprintf(text, sizeof(text), "%s  %ux%u %s  %.1f fps   NPU: %s",
             stats.stream_id.c_str(), stats.width, stats.height,
             mw::FourccToString(stats.fourcc).c_str(), stats.fps,
             stats.npu_connected ? "connected" : "not connected");
  } else {
    snprintf(text, sizeof(text), "camera: not connected   NPU: %s",
             stats.npu_connected ? "connected" : "not connected");
  }
  gtk_label_set_text(GTK_LABEL(app->status_label), text);
  RebuildModelChecks(app, app->core->models());
  gtk_widget_queue_draw(app->drawing);
  return G_SOURCE_CONTINUE;
}

gboolean OnKey(GtkWidget*, GdkEventKey* event, gpointer data) {
  App* app = static_cast<App*>(data);
  if (event->keyval == GDK_KEY_q || event->keyval == GDK_KEY_Q ||
      event->keyval == GDK_KEY_Escape) {
    gtk_main_quit();
    return TRUE;
  }
  if (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F) {
    app->fullscreen = !app->fullscreen;
    if (app->fullscreen) {
      gtk_window_fullscreen(GTK_WINDOW(app->window));
    } else {
      gtk_window_unfullscreen(GTK_WINDOW(app->window));
    }
    return TRUE;
  }
  return FALSE;
}

gboolean OnSignal(gpointer) {
  gtk_main_quit();
  return G_SOURCE_REMOVE;
}

int RunWindow(mw::ViewerCore* core, bool use_npu, bool fullscreen) {
  App app;
  app.core = core;
  app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(app.window), "Genio camera viewer");
  gtk_window_set_default_size(GTK_WINDOW(app.window), 960, 620);
  g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  g_signal_connect(app.window, "key-press-event", G_CALLBACK(OnKey), &app);

  GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(app.window), vbox);

  GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(bar), 6);
  app.status_label = gtk_label_new("starting...");
  gtk_label_set_xalign(GTK_LABEL(app.status_label), 0.0f);
  gtk_box_pack_start(GTK_BOX(bar), app.status_label, TRUE, TRUE, 0);
  app.model_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  if (use_npu) {
    gtk_box_pack_end(GTK_BOX(bar), app.model_box, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

  app.drawing = gtk_drawing_area_new();
  gtk_widget_set_hexpand(app.drawing, TRUE);
  gtk_widget_set_vexpand(app.drawing, TRUE);
  g_signal_connect(app.drawing, "draw", G_CALLBACK(OnDraw), &app);
  gtk_box_pack_start(GTK_BOX(vbox), app.drawing, TRUE, TRUE, 0);

  g_unix_signal_add(SIGINT, OnSignal, nullptr);
  g_unix_signal_add(SIGTERM, OnSignal, nullptr);
  g_timeout_add(500, OnTick, &app);

  // Frames arrive on an SDK thread; wake the UI thread, at most once per
  // pending redraw so a fast camera cannot flood the main loop.
  core->SetFrameNotify([&app]() {
    if (!app.redraw_pending.exchange(true)) {
      g_idle_add(OnFrameIdle, &app);
    }
  });
  const mw::Status started = core->Start();
  if (!started.ok()) {
    fprintf(stderr, "mw-viewer: %s\n", started.ToString().c_str());
    gtk_widget_destroy(app.window);
    return 1;
  }
  if (fullscreen) {
    app.fullscreen = true;
    gtk_window_fullscreen(GTK_WINDOW(app.window));
  }
  gtk_widget_show_all(app.window);
  gtk_main();
  core->Stop();  // No more idle callbacks into |app| after this.
  return 0;
}

// Headless: wait for frames (and detections), render one picture, exit.
int RunSnapshot(mw::ViewerCore* core, const std::string& path,
                uint32_t wait_frames, double timeout_s, bool expect_boxes,
                bool use_npu) {
  const mw::Status started = core->Start();
  if (!started.ok()) {
    fprintf(stderr, "mw-viewer: %s\n", started.ToString().c_str());
    return 1;
  }
  const bool need_boxes = expect_boxes && use_npu;
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(static_cast<int>(timeout_s * 1000));
  std::shared_ptr<const mw::BgraImage> image;
  mw::OverlaySnapshot overlay;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (core->stats().frames < wait_frames) {
      continue;
    }
    image = core->latest_image();
    overlay = core->overlay();
    if (image != nullptr && (!need_boxes || !overlay.boxes.empty())) {
      break;
    }
  }
  const mw::ViewerCore::Stats stats = core->stats();
  core->Stop();
  if (image == nullptr) {
    fprintf(stderr, "mw-viewer: no frame received within %.0f s\n", timeout_s);
    return 2;
  }
  cairo_surface_t* surface = cairo_image_surface_create(
      CAIRO_FORMAT_RGB24, static_cast<int>(image->width),
      static_cast<int>(image->height));
  cairo_t* cr = cairo_create(surface);
  cairo_surface_t* picture = cairo_image_surface_create_for_data(
      const_cast<uint8_t*>(image->pixels.data()), CAIRO_FORMAT_RGB24,
      static_cast<int>(image->width), static_cast<int>(image->height),
      static_cast<int>(image->stride()));
  cairo_set_source_surface(cr, picture, 0, 0);
  cairo_paint(cr);
  mw::DrawOverlay(cr, 0, 0, image->width, image->height, overlay);
  cairo_destroy(cr);
  cairo_surface_destroy(picture);
  const cairo_status_t written =
      cairo_surface_write_to_png(surface, path.c_str());
  cairo_surface_destroy(surface);
  if (written != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "mw-viewer: cannot write %s: %s\n", path.c_str(),
            cairo_status_to_string(written));
    return 1;
  }
  printf("snapshot: %s %ux%u camera=%s frames=%llu boxes=%zu captions=%zu\n",
         path.c_str(), image->width, image->height,
         mw::FourccToString(stats.fourcc).c_str(),
         static_cast<unsigned long long>(stats.frames), overlay.boxes.size(),
         overlay.captions.size());
  if (need_boxes && overlay.boxes.empty()) {
    fprintf(stderr, "mw-viewer: no detections arrived in time\n");
    return 3;
  }
  return 0;
}

void PrintUsage(const char* program) {
  fprintf(stderr,
          "usage: %s [--bus system|user] [--no-npu] [--fullscreen]\n"
          "       %s --snapshot FILE.png [--wait-frames N] [--timeout S]\n"
          "          [--expect-boxes]\n",
          program, program);
}

}  // namespace

int main(int argc, char** argv) {
  std::string bus = "system";
  std::string snapshot;
  bool use_npu = true;
  bool fullscreen = false;
  bool expect_boxes = false;
  int wait_frames = 5;
  double timeout_s = 10.0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--bus" && has_value) {
      bus = argv[++i];
    } else if (arg == "--no-npu") {
      use_npu = false;
    } else if (arg == "--fullscreen") {
      fullscreen = true;
    } else if (arg == "--snapshot" && has_value) {
      snapshot = argv[++i];
    } else if (arg == "--wait-frames" && has_value) {
      wait_frames = atoi(argv[++i]);
    } else if (arg == "--timeout" && has_value) {
      timeout_s = atof(argv[++i]);
    } else if (arg == "--expect-boxes") {
      expect_boxes = true;
    } else {
      PrintUsage(argv[0]);
      return 2;
    }
  }
  if (bus != "system" && bus != "user") {
    PrintUsage(argv[0]);
    return 2;
  }
  if (wait_frames < 1 || timeout_s <= 0) {
    PrintUsage(argv[0]);
    return 2;
  }

  mw::ViewerCore::Options options;
  options.bus = bus;
  options.use_npu = use_npu;
  mw::ViewerCore core(options);

  if (!snapshot.empty()) {
    return RunSnapshot(&core, snapshot, static_cast<uint32_t>(wait_frames),
                       timeout_s, expect_boxes, use_npu);
  }
  if (!gtk_init_check(&argc, &argv)) {
    fprintf(stderr,
            "mw-viewer: cannot open a display (is DISPLAY or "
            "WAYLAND_DISPLAY set?). Use --snapshot for headless runs.\n");
    return 1;
  }
  return RunWindow(&core, use_npu, fullscreen);
}
