#include "utils/http_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace concurrent_http {
namespace utils {

namespace {

std::tm SafeGmTime(std::time_t value) {
  std::tm output{};
#if defined(_WIN32)
  gmtime_s(&output, &value);
#else
  gmtime_r(&value, &output);
#endif
  return output;
}

}  // namespace

std::string Trim(const std::string& value) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };

  std::size_t begin = 0;
  while (begin < value.size() &&
         is_space(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin &&
         is_space(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }

  return value.substr(begin, end - begin);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string UrlDecode(const std::string& value) {
  std::ostringstream output;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() &&
        std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
      const std::string hex = value.substr(i + 1, 2);
      output << static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
      i += 2;
    } else {
      output << value[i];
    }
  }
  return output.str();
}

std::string GetMimeType(const std::string& path) {
  const std::string extension =
      ToLower(std::filesystem::path(path).extension().string());

  if (extension == ".html" || extension == ".htm") {
    return "text/html; charset=utf-8";
  }
  if (extension == ".css") {
    return "text/css; charset=utf-8";
  }
  if (extension == ".js") {
    return "application/javascript; charset=utf-8";
  }
  if (extension == ".json") {
    return "application/json; charset=utf-8";
  }
  if (extension == ".txt") {
    return "text/plain; charset=utf-8";
  }
  if (extension == ".svg") {
    return "image/svg+xml";
  }
  if (extension == ".png") {
    return "image/png";
  }
  if (extension == ".jpg" || extension == ".jpeg") {
    return "image/jpeg";
  }
  if (extension == ".gif") {
    return "image/gif";
  }
  if (extension == ".ico") {
    return "image/x-icon";
  }
  return "application/octet-stream";
}

std::string StatusText(int status_code) {
  switch (status_code) {
    case 200:
      return "OK";
    case 302:
      return "Found";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 413:
      return "Payload Too Large";
    case 500:
      return "Internal Server Error";
    default:
      return "Unknown";
  }
}

std::string HttpDateNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time_value = std::chrono::system_clock::to_time_t(now);
  const std::tm gm_time = SafeGmTime(time_value);

  std::ostringstream stream;
  stream << std::put_time(&gm_time, "%a, %d %b %Y %H:%M:%S GMT");
  return stream.str();
}

std::string EscapeJson(const std::string& value) {
  std::ostringstream output;
  for (const char ch : value) {
    switch (ch) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec;
        } else {
          output << ch;
        }
        break;
    }
  }
  return output.str();
}

std::string BuildHttpResponse(
    int status_code, const std::string& content_type, const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& extra_headers) {
  std::ostringstream stream;
  stream << "HTTP/1.1 " << status_code << ' ' << StatusText(status_code)
         << "\r\n";
  stream << "Date: " << HttpDateNow() << "\r\n";
  stream << "Server: ConcurrentPosixHttpServer/1.0\r\n";
  stream << "Content-Type: " << content_type << "\r\n";
  stream << "Content-Length: " << body.size() << "\r\n";
  stream << "Connection: close\r\n";
  for (const auto& [name, value] : extra_headers) {
    stream << name << ": " << value << "\r\n";
  }
  stream << "\r\n";
  stream << body;
  return stream.str();
}

}  // namespace utils
}  // namespace concurrent_http
