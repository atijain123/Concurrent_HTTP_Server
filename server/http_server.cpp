#include <sys/time.h>
#include "server/http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "server/http_parser.h"
#include "utils/file_utils.h"
#include "utils/http_utils.h"

namespace concurrent_http {

namespace {

constexpr int kListenBacklog = 128;
constexpr int kSocketTimeoutSeconds = 5;

void CloseSocketQuietly(int fd) {
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
}

bool IsInternalMonitoringPath(const std::string& path) {
  return path == "/metrics" || path == "/metrics-view" || path == "/monitor" ||
         path == "/monitor/" || path.rfind("/monitor/", 0) == 0;
}

std::string CacheKeyForPath(const std::string& normalized_path) {
  if (normalized_path == "/") {
    return "/index.html";
  }
  if (!normalized_path.empty() && normalized_path.back() == '/') {
    return normalized_path + "index.html";
  }
  return normalized_path;
}

class ConnectionGuard {
 public:
  explicit ConnectionGuard(int fd) : fd_(fd) {}

  ~ConnectionGuard() { CloseSocketQuietly(fd_); }

 private:
  int fd_;
};

}  // namespace

HttpServer::HttpServer(ServerConfig config)
    : config_(std::move(config)),
      thread_pool_(config_.thread_count),
      logger_(config_.log_file, config_.mirror_logs_to_console),
      metrics_(config_.thread_count),
      cache_(config_.max_cache_bytes, config_.cache_alpha, config_.cache_beta),
      prefetcher_(PrefetchConfig{config_.document_root,
                                 config_.max_prefetch_resources,
                                 config_.max_prefetch_file_bytes,
                                 config_.prefetch_cache_threshold,
                                 128,
                                 config_.thread_count * 2},
                  cache_, logger_, metrics_),
      running_(false),
      listen_fd_(-1) {}

HttpServer::~HttpServer() { Stop(); }

void HttpServer::Start() {
  if (running_.load()) {
    return;
  }

  listen_fd_ = CreateListeningSocket();
  try {
    thread_pool_.Start();
    prefetcher_.Start();
    running_.store(true);
    accept_thread_ = std::thread(&HttpServer::AcceptLoop, this);
    UpdateApproximateMemoryMetric();
    logger_.LogInfo("Server listening on " + config_.host + ":" +
                    std::to_string(config_.port) + " with document root " +
                    config_.document_root);
  } catch (...) {
    CloseSocketQuietly(listen_fd_);
    listen_fd_ = -1;
    prefetcher_.Stop();
    thread_pool_.Stop();
    running_.store(false);
    throw;
  }
}

void HttpServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  CloseSocketQuietly(listen_fd_);
  listen_fd_ = -1;

  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  thread_pool_.Stop();
  prefetcher_.Stop();
  logger_.LogInfo("Server stopped");
}

bool HttpServer::IsRunning() const noexcept { return running_.load(); }

int HttpServer::CreateListeningSocket() {
  const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    throw std::runtime_error(std::string("socket failed: ") +
                             std::strerror(errno));
  }

  int reuse = 1;
  if (::setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    const std::string error = std::strerror(errno);
    CloseSocketQuietly(socket_fd);
    throw std::runtime_error("setsockopt(SO_REUSEADDR) failed: " + error);
  }

#ifdef SO_REUSEPORT
  ::setsockopt(socket_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(config_.port);

  if (config_.host.empty() || config_.host == "0.0.0.0" || config_.host == "*") {
    address.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
    CloseSocketQuietly(socket_fd);
    throw std::runtime_error("Invalid bind address: " + config_.host);
  }

  if (::bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
      0) {
    const std::string error = std::strerror(errno);
    CloseSocketQuietly(socket_fd);
    throw std::runtime_error("bind failed: " + error);
  }

  if (::listen(socket_fd, kListenBacklog) < 0) {
    const std::string error = std::strerror(errno);
    CloseSocketQuietly(socket_fd);
    throw std::runtime_error("listen failed: " + error);
  }

  return socket_fd;
}

void HttpServer::AcceptLoop() {
  while (running_.load()) {
    sockaddr_in client_address {};
    socklen_t client_length = sizeof(client_address);
    const int client_fd =
        ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_address),
                 &client_length);
    if (client_fd < 0) {
      if (!running_.load()) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      logger_.LogError(std::string("accept failed: ") + std::strerror(errno));
      continue;
    }

    timeval timeout {};
    timeout.tv_sec = kSocketTimeoutSeconds;
    timeout.tv_usec = 0;
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    char ip_buffer[INET_ADDRSTRLEN] = {0};
    const char* ip =
        ::inet_ntop(AF_INET, &client_address.sin_addr, ip_buffer, sizeof(ip_buffer));
    const std::string client_ip = ip != nullptr ? ip : "unknown";

    try {
      thread_pool_.Enqueue(
          [this, client_fd, client_ip] { HandleClient(client_fd, client_ip); });
    } catch (const std::exception& ex) {
      CloseSocketQuietly(client_fd);
      logger_.LogError(std::string("Failed to enqueue client task: ") + ex.what());
    }
  }
}

void HttpServer::HandleClient(int client_fd, const std::string& client_ip) {
  ConnectionGuard connection_guard(client_fd);

  HttpResponse response;
  std::optional<bool> cache_lookup;
  std::string resource = "<parse-error>";
  bool is_internal_monitoring_request = false;
  bool should_record_client_request = false;
  bool active_client_connection_recorded = false;

  try {
    const auto parse_result = HttpParser::ReadFromSocket(client_fd);
    if (!parse_result.success) {
      response.status_code = parse_result.status_code;
      response.content_type = "text/plain; charset=utf-8";
      response.body = parse_result.error_message + "\n";
    } else {
      resource = parse_result.request.path;
      is_internal_monitoring_request = IsInternalMonitoringPath(resource);
      should_record_client_request = !is_internal_monitoring_request;
      if (should_record_client_request) {
        metrics_.ConnectionOpened();
        active_client_connection_recorded = true;
      }
      response = HandleRequest(parse_result.request, &cache_lookup);
    }
  } catch (const std::exception& ex) {
    logger_.LogError(std::string("Request handling exception: ") + ex.what());
    response.status_code = 500;
    response.content_type = "text/plain; charset=utf-8";
    response.body = "500 Internal Server Error\n";
  }

  if (!SendResponse(client_fd, response) && should_record_client_request) {
    logger_.LogError("Failed to send response to " + client_ip);
  }

  if (should_record_client_request) {
    metrics_.RequestCompleted();
    if (cache_lookup.has_value()) {
      if (*cache_lookup) {
        metrics_.RecordCacheHit();
      } else {
        metrics_.RecordCacheMiss();
      }
    }
    const auto snapshot = metrics_.Snapshot();
    cache_.AdjustWeights(snapshot.cache_hit_rate);
    logger_.LogRequest(client_ip, resource, response.status_code);
  }
  UpdateApproximateMemoryMetric();
  if (active_client_connection_recorded) {
    metrics_.ConnectionClosed();
  }
}

HttpResponse HttpServer::HandleRequest(const HttpRequest& request,
                                       std::optional<bool>* cache_lookup) {
  if (request.method == "GET" && request.path == "/metrics") {
    return BuildMetricsResponse();
  }
  if (request.method == "GET" && request.path == "/metrics-view") {
    return BuildMetricsPage();
  }
  if (request.method == "GET" && request.path == "/monitor") {
    HttpResponse response;
    response.status_code = 302;
    response.content_type = "text/html; charset=utf-8";
    response.body = "";
    response.headers.emplace_back("Location", "/monitor/");
    response.headers.emplace_back("Cache-Control", "no-store");
    return response;
  }

  if (request.method == "POST" && request.path == "/echo") {
    return HandleEchoPost(request);
  }

  if (request.method == "GET") {
    return ServeStaticFile(request, cache_lookup);
  }

  if (request.method == "POST") {
    HttpResponse response;
    response.status_code = 404;
    response.content_type = "text/plain; charset=utf-8";
    response.body = "404 Not Found\nPOST is available at /echo.\n";
    return response;
  }

  HttpResponse response;
  response.status_code = 405;
  response.content_type = "text/plain; charset=utf-8";
  response.body = "405 Method Not Allowed\n";
  response.headers.emplace_back("Allow", "GET, POST");
  return response;
}

HttpResponse HttpServer::ServeStaticFile(const HttpRequest& request,
                                         std::optional<bool>* cache_lookup) {
  namespace fs = std::filesystem;

  const std::string normalized_path =
      request.path.empty() ? "/" : utils::NormalizeUrlPath(request.path);
  const std::string cache_key = CacheKeyForPath(normalized_path);
  const bool is_internal_monitoring_path =
      IsInternalMonitoringPath(normalized_path);
  const bool is_monitor_spa =
      normalized_path == "/monitor/" ||
      normalized_path == "/monitor/index.html";
  std::string file_path;
  if (!utils::ResolveRequestPath(config_.document_root, normalized_path,
                                 &file_path)) {
    return HttpResponse{404, "text/plain; charset=utf-8", "404 Not Found\n", {}};
  }

  std::error_code ec;
  if (!fs::exists(file_path, ec) || !fs::is_regular_file(file_path, ec)) {
    return HttpResponse{404, "text/plain; charset=utf-8", "404 Not Found\n", {}};
  }

  if (!is_internal_monitoring_path) {
    CacheResult cached;
    if (cache_.Get(cache_key, &cached)) {
      if (cache_lookup != nullptr) {
        *cache_lookup = true;
      }

      HttpResponse response;
      response.status_code = 200;
      response.content_type = cached.content_type;
      response.body = *cached.content;

      if (request.method == "GET" &&
          response.content_type.rfind("text/html", 0) == 0) {
        prefetcher_.ScheduleHtmlPrefetch(cache_key, response.body);
      }
      return response;
    }
  }

  if (cache_lookup != nullptr && !is_internal_monitoring_path) {
    *cache_lookup = false;
  }

  std::string content;
  if (!utils::ReadFileToString(file_path, &content)) {
    return HttpResponse{500, "text/plain; charset=utf-8",
                        "500 Internal Server Error\n", {}};
  }

  const std::string content_type = utils::GetMimeType(file_path);
  if (!is_internal_monitoring_path) {
    cache_.Put(cache_key, content, content_type, 1);
    UpdateApproximateMemoryMetric();

    if (request.method == "GET" && content_type.rfind("text/html", 0) == 0) {
      prefetcher_.ScheduleHtmlPrefetch(cache_key, content);
    }
  }

  HttpResponse response;
  response.status_code = 200;
  response.content_type = content_type;
  response.body = std::move(content);
  if (is_internal_monitoring_path || is_monitor_spa) {
    response.headers.emplace_back("Cache-Control", "no-store");
  }
  return response;
}

HttpResponse HttpServer::BuildMetricsResponse() const {
  HttpResponse response;
  response.status_code = 200;
  response.content_type = "application/json; charset=utf-8";
  response.body = BuildMetricsJson();
  response.headers.emplace_back("Cache-Control", "no-store");
  return response;
}

HttpResponse HttpServer::BuildMetricsPage() const {
  const MetricsSnapshot m = metrics_.Snapshot();
  const CacheStats c = cache_.GetStats();
  double alpha = cache_.GetAlpha();
  double beta = cache_.GetBeta();
  std::ostringstream html;

  html << R"HTML(
<!DOCTYPE html>
<html>
<head>
<title>Metrics View</title>
<style>
body {
  background: linear-gradient(135deg, #0f172a, #020617);
  color: #e2e8f0;
  font-family: 'Segoe UI', sans-serif;
  padding: 20px;
}

h1 {
  margin-bottom: 20px;
}

.grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 20px;
}

.card {
  background: rgba(30, 41, 59, 0.6);
  backdrop-filter: blur(10px);
  padding: 20px;
  border-radius: 15px;
  box-shadow: 0 8px 20px rgba(0,0,0,0.3);
  transition: all 0.25s ease;
}

.card:hover {
  transform: translateY(-5px);
}

.label {
  color: #94a3b8;
  font-size: 12px;
}

.value {
  font-size: 22px;
  margin-top: 5px;
  font-weight: bold;
}

/* STATUS COLORS */
.good { color: #22c55e; }
.warn { color: #facc15; }
.bad  { color: #ef4444; }

/* STATUS DOT */
.dot {
  display: inline-block;
  width: 8px;
  height: 8px;
  border-radius: 50%;
  margin-right: 6px;
}
.good-dot { background: #22c55e; }
.warn-dot { background: #facc15; }
.bad-dot  { background: #ef4444; }
</style>
</head>
<body>

<h1>Server Metrics</h1>

<div class="grid">
)HTML";

  auto add = [&](const std::string& label,
                 const std::string& val,
                 const std::string& cls) {
    html << "<div class='card'><div class='label'>"
     << "<span class='dot " << cls << "-dot'></span>"
     << label
         << "</div><div class='value " << cls << "'>" << val
         << "</div></div>";
  };

  // 🔥 formatted values
  std::ostringstream rps, hit_rate, usage;
  std::ostringstream alpha_s, beta_s;

  alpha_s << std::fixed << std::setprecision(2) << alpha;
  beta_s << std::fixed << std::setprecision(2) << beta;
  rps << std::fixed << std::setprecision(2) << m.requests_per_second;
  hit_rate << std::fixed << std::setprecision(2) << (m.cache_hit_rate * 100);
  usage << std::fixed << std::setprecision(3) << (c.usage_ratio * 100);

  double hit = m.cache_hit_rate * 100;
  double use = c.usage_ratio * 100;

  // 🔥 classification logic
  std::string hit_cls = (hit > 80) ? "good" : (hit > 50) ? "warn" : "bad";
  std::string use_cls = (use < 50) ? "good" : (use < 80) ? "warn" : "bad";
  std::string rps_cls = (m.requests_per_second > 5) ? "good" : "warn";

  // 🔥 metrics
  add("Active Connections", std::to_string(m.active_connections), "good");
  add("Total Requests", std::to_string(m.total_requests), "good");
  add("Requests/sec", rps.str(), rps_cls);

  add("Cache Hit Rate", hit_rate.str() + "%", hit_cls);
  add("Cache Hits", std::to_string(m.cache_hits), "good");
  add("Cache Misses", std::to_string(m.cache_misses), "bad");

  add("Cache Memory",
      std::to_string(c.current_memory_usage_bytes / 1024) + " KB", "warn");

  add("Cache Usage", usage.str() + "%", use_cls);

  add("Threads", std::to_string(m.thread_count), "good");
  add("Cache Alpha", alpha_s.str(), "good");
  add("Cache Beta", beta_s.str(), "good");
  html << R"HTML(
</div>
</body>
</html>
)HTML";

  HttpResponse res;
  res.status_code = 200;
  res.content_type = "text/html; charset=utf-8";
  res.body = html.str();
  res.headers.emplace_back("Cache-Control", "no-store");
  return res;
}
HttpResponse HttpServer::HandleEchoPost(const HttpRequest& request) const {
  std::ostringstream body;
  body << "{"
       << "\"received_bytes\":" << request.body.size() << ","
       << "\"body\":\"" << utils::EscapeJson(request.body) << "\""
       << "}";

  HttpResponse response;
  response.status_code = 200;
  response.content_type = "application/json; charset=utf-8";
  response.body = body.str();
  return response;
}

bool HttpServer::SendResponse(int client_fd, const HttpResponse& response) const {
  const std::string payload = utils::BuildHttpResponse(
      response.status_code, response.content_type, response.body, response.headers);

  std::size_t total_sent = 0;
  while (total_sent < payload.size()) {
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    const ssize_t sent =
        ::send(client_fd, payload.data() + total_sent,
               payload.size() - total_sent, flags);
    if (sent < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (sent == 0) {
      return false;
    }
    total_sent += static_cast<std::size_t>(sent);
  }
  return true;
}

void HttpServer::UpdateApproximateMemoryMetric() {
  metrics_.SetApproximateMemoryUsage(cache_.GetStats().current_memory_usage_bytes);
}

std::string HttpServer::BuildMetricsJson() const {
  const MetricsSnapshot metrics = metrics_.Snapshot();
  const CacheStats cache_stats = cache_.GetStats();

  std::ostringstream body;
  body << "{\n"
     << "  \"active_connections\": " << metrics.active_connections << ",\n"
     << "  \"total_requests\": " << metrics.total_requests << ",\n"
     << "  \"requests_per_second\": " << metrics.requests_per_second << ",\n"
     << "  \"cache_hit_rate\": " << metrics.cache_hit_rate << ",\n"
     << "  \"cache_hits\": " << metrics.cache_hits << ",\n"
     << "  \"cache_misses\": " << metrics.cache_misses << ",\n"
     << "  \"approximate_memory_usage_bytes\": " << metrics.approximate_memory_usage_bytes << ",\n"
     << "  \"cache_usage_bytes\": " << cache_stats.current_memory_usage_bytes << ",\n"
     << "  \"cache_usage_ratio\": " << cache_stats.usage_ratio << ",\n"
     << "  \"thread_count\": " << metrics.thread_count << ",\n"
     << "  \"cache_alpha\":" << cache_.GetAlpha() << ",\n"
     << "  \"cache_beta\":" << cache_.GetBeta() << "\n"
     << "}";
  return body.str();
}

}  // namespace concurrent_http
