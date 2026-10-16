// Copyright 2026 Genio Middleware Authors.
//
// Tracks video subscribers and the pool slots each one currently holds
// ("leases"). A slow subscriber that reaches its queue depth simply misses
// new frames, so it can never stall the camera. When a subscriber disappears
// (unsubscribe or bus peer exit) all of its leases are reclaimed.

#ifndef MW_MEDIA_VIDEO_HUB_H_
#define MW_MEDIA_VIDEO_HUB_H_

#include <stdint.h>

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "mw/common/frame_types.h"
#include "mw/common/macros.h"
#include "mw/common/status.h"
#include "mw/media/frame_pool.h"

namespace mw {

struct VideoSubscriber {
  VideoSubscriber() : id(0), queue_depth(0) {}
  uint32_t id;
  std::string peer;         // Unique bus name of the subscriber.
  std::string client_name;  // Free-form name for diagnostics.
  uint32_t queue_depth;     // Max frames held at once.
};

class VideoHub {
 public:
  // Called (outside the hub lock) for each subscriber a frame was delivered
  // to. Must be quick and non-blocking.
  typedef std::function<void(const VideoSubscriber& subscriber,
                             const FrameInfo& frame)>
      NotifyFunction;

  VideoHub(FramePool* pool, const NotifyFunction& notify);

  MW_DISALLOW_COPY_AND_ASSIGN(VideoHub);

  // Registers a subscriber and returns its id (never 0).
  uint32_t Add(const std::string& peer, const std::string& client_name,
               uint32_t queue_depth);

  // Removes a subscription owned by |peer| and returns its leases.
  Status Remove(uint32_t id, const std::string& peer);

  // Removes every subscription of |peer|. Returns how many were removed.
  size_t RemovePeer(const std::string& peer);

  // Returns the lease on |slot|.
  Status Release(uint32_t id, const std::string& peer, uint32_t slot);

  // Offers |frame| (whose slot the caller holds a reference on) to every
  // subscriber below its queue depth. Returns the number of deliveries.
  size_t Publish(const FrameInfo& frame);

  size_t subscriber_count() const;
  // Frames not delivered because a subscriber was at its queue depth.
  uint64_t skipped_deliveries() const;
  std::vector<VideoSubscriber> Snapshot() const;

 private:
  struct Entry {
    VideoSubscriber subscriber;
    std::set<uint32_t> held;
  };

  void DropEntryLocked(Entry* entry);

  FramePool* pool_;
  NotifyFunction notify_;
  mutable std::mutex mutex_;
  std::map<uint32_t, Entry> entries_;
  uint32_t next_id_;
  uint64_t skipped_;
};

}  // namespace mw

#endif  // MW_MEDIA_VIDEO_HUB_H_
