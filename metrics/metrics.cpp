#include "metrics/metrics.h"

namespace concurrent_http {

MetricsCollector::MetricsCollector(std::size_t thread_count)
    : active_connections_(0),
      total_requests_(0),
      cache_hits_(0),
      cache_misses_(0),
      approximate_memory_usage_bytes_(0),
      thread_count_(thread_count) {}

void MetricsCollector::ConnectionOpened() {
  active_connections_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::ConnectionClosed() {
  std::size_t current = active_connections_.load(std::memory_order_relaxed);
  while (current > 0 &&
         !active_connections_.compare_exchange_weak(
             current, current - 1, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void MetricsCollector::RequestCompleted() {
  total_requests_.fetch_add(1, std::memory_order_relaxed);

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(rps_mutex_);
  recent_requests_.push_back(now);
  PruneOldSamplesLocked(now);
}

void MetricsCollector::RecordCacheHit() {
  cache_hits_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::RecordCacheMiss() {
  cache_misses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsCollector::SetApproximateMemoryUsage(std::size_t bytes) {
  approximate_memory_usage_bytes_.store(bytes, std::memory_order_relaxed);
}

MetricsSnapshot MetricsCollector::Snapshot() const {
  MetricsSnapshot snapshot;
  snapshot.active_connections =
      active_connections_.load(std::memory_order_relaxed);
  snapshot.total_requests = total_requests_.load(std::memory_order_relaxed);
  snapshot.cache_hits = cache_hits_.load(std::memory_order_relaxed);
  snapshot.cache_misses = cache_misses_.load(std::memory_order_relaxed);
  snapshot.approximate_memory_usage_bytes =
      approximate_memory_usage_bytes_.load(std::memory_order_relaxed);
  snapshot.thread_count = thread_count_;

  {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(rps_mutex_);
    PruneOldSamplesLocked(now);
    snapshot.requests_per_second =
        static_cast<double>(recent_requests_.size());
  }

  const std::size_t total_cache_events =
      snapshot.cache_hits + snapshot.cache_misses;
  snapshot.cache_hit_rate =
      total_cache_events == 0
          ? 0.0
          : static_cast<double>(snapshot.cache_hits) /
                static_cast<double>(total_cache_events);

  return snapshot;
}

void MetricsCollector::PruneOldSamplesLocked(
    const std::chrono::steady_clock::time_point& now) const {
  const auto cutoff = now - std::chrono::seconds(1);
  while (!recent_requests_.empty() && recent_requests_.front() < cutoff) {
    recent_requests_.pop_front();
  }
}

}  // namespace concurrent_http
