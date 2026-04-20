#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#include "server/http_server.h"

namespace {

std::atomic<bool> g_shutdown_requested{false};

void HandleShutdownSignal(int /*signal*/) { g_shutdown_requested.store(true); }

std::size_t DefaultThreadCount() {
  const unsigned int detected = std::thread::hardware_concurrency();
  return detected == 0 ? 4 : static_cast<std::size_t>(detected);
}

std::uint16_t ParsePort(const char* value) {
  const unsigned long port = std::stoul(value);
  if (port == 0 || port > std::numeric_limits<std::uint16_t>::max()) {
    throw std::out_of_range("port must be between 1 and 65535");
  }
  return static_cast<std::uint16_t>(port);
}

std::size_t ParsePositiveSize(const char* value, const std::string& label) {
  const unsigned long long parsed = std::stoull(value);
  if (parsed == 0) {
    throw std::out_of_range(label + " must be greater than zero");
  }
  if (parsed > static_cast<unsigned long long>(
                   std::numeric_limits<std::size_t>::max())) {
    throw std::out_of_range(label + " is too large");
  }
  return static_cast<std::size_t>(parsed);
}

}  // namespace

int main(int argc, char* argv[]) {
  using concurrent_http::HttpServer;
  using concurrent_http::ServerConfig;

  try {
    ServerConfig config;
    config.thread_count = DefaultThreadCount();
    if (argc > 1) {
      config.port = ParsePort(argv[1]);
    }
    if (argc > 2) {
      config.document_root = argv[2];
    }
    if (argc > 3) {
      config.thread_count = ParsePositiveSize(argv[3], "thread_count");
    }
    if (argc > 4) {
      const std::size_t cache_size_mb =
          ParsePositiveSize(argv[4], "cache_size_mb");
      if (cache_size_mb >
          std::numeric_limits<std::size_t>::max() / (1024ULL * 1024ULL)) {
        throw std::out_of_range("cache_size_mb is too large");
      }
      config.max_cache_bytes = cache_size_mb * 1024ULL * 1024ULL;
    }

    std::signal(SIGINT, HandleShutdownSignal);
    std::signal(SIGTERM, HandleShutdownSignal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    HttpServer server(config);
    server.Start();

    std::cout << "Server running on http://localhost:" << config.port
              << "\n";
    std::cout << "Document root: " << config.document_root << "\n";
    std::cout << "Monitoring UI: http://localhost:" << config.port
              << "/monitor/\n";
    std::cout << "Metrics JSON:   http://localhost:" << config.port
              << "/metrics\n";
    std::cout << "Press Ctrl+C to stop.\n";

    while (!g_shutdown_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    server.Stop();
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Fatal error: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}


































































































































































































































































































































































































































































































































































































































































































