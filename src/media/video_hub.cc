// Copyright 2026 VMX Middleware Team.

#include "mw/media/video_hub.h"

#include "mw/common/logging.h"

namespace mw {

VideoHub::VideoHub(FramePool* pool, const NotifyFunction& notify)
    : pool_(pool), notify_(notify), next_id_(1), skipped_(0) {}

uint32_t VideoHub::Add(const std::string& peer, const std::string& client_name,
                       uint32_t queue_depth) {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t id = next_id_++;
  Entry& entry = entries_[id];
  entry.subscriber.id = id;
  entry.subscriber.peer = peer;
  entry.subscriber.client_name = client_name;
  entry.subscriber.queue_depth = queue_depth;
  return id;
}

void VideoHub::DropEntryLocked(Entry* entry) {
  for (std::set<uint32_t>::const_iterator it = entry->held.begin();
       it != entry->held.end(); ++it) {
    pool_->Unref(*it);
  }
  entry->held.clear();
}

Status VideoHub::Remove(uint32_t id, const std::string& peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<uint32_t, Entry>::iterator it = entries_.find(id);
  if (it == entries_.end() || it->second.subscriber.peer != peer) {
    return NotFoundError("no such video subscription");
  }
  DropEntryLocked(&it->second);
  entries_.erase(it);
  return Status::Ok();
}

size_t VideoHub::RemovePeer(const std::string& peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t removed = 0;
  for (std::map<uint32_t, Entry>::iterator it = entries_.begin();
       it != entries_.end();) {
    if (it->second.subscriber.peer == peer) {
      DropEntryLocked(&it->second);
      entries_.erase(it++);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

Status VideoHub::Release(uint32_t id, const std::string& peer, uint32_t slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::map<uint32_t, Entry>::iterator it = entries_.find(id);
  if (it == entries_.end() || it->second.subscriber.peer != peer) {
    return NotFoundError("no such video subscription");
  }
  if (it->second.held.erase(slot) == 0) {
    return InvalidArgumentError("slot is not leased by this subscription");
  }
  pool_->Unref(slot);
  return Status::Ok();
}

size_t VideoHub::Publish(const FrameInfo& frame) {
  std::vector<VideoSubscriber> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::map<uint32_t, Entry>::iterator it = entries_.begin();
         it != entries_.end(); ++it) {
      Entry& entry = it->second;
      if (entry.held.size() >= entry.subscriber.queue_depth) {
        ++skipped_;
        continue;
      }
      entry.held.insert(frame.slot);
      pool_->AddRef(frame.slot);
      targets.push_back(entry.subscriber);
    }
  }
  for (size_t i = 0; i < targets.size(); ++i) {
    notify_(targets[i], frame);
  }
  return targets.size();
}

size_t VideoHub::subscriber_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

uint64_t VideoHub::skipped_deliveries() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skipped_;
}

std::vector<VideoSubscriber> VideoHub::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<VideoSubscriber> out;
  for (std::map<uint32_t, Entry>::const_iterator it = entries_.begin();
       it != entries_.end(); ++it) {
    out.push_back(it->second.subscriber);
  }
  return out;
}

}  // namespace mw
