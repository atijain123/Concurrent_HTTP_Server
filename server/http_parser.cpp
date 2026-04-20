#include "server/http_parser.h"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>

#include "utils/file_utils.h"
#include "utils/http_utils.h"

namespace concurrent_http {

namespace {

constexpr std::size_t kReadBufferSize = 4096;

bool IsReceiveTimeout(int error_code) {
  return error_code == EAGAIN
#if EWOULDBLOCK != EAGAIN
      || error_code == EWOULDBLOCK
#endif
#ifdef ETIMEDOUT
      || error_code == ETIMEDOUT
#endif
      ;
}

void StripCarriageReturn(std::string* line) {
  if (line != nullptr && !line->empty() && line->back() == '\r') {
    line->pop_back();
  }
}

HttpParser::ParseResult ParseHeaderBlock(const std::string& header_block,
                                         const std::string& body,
                                         std::size_t max_body_bytes) {
  HttpParser::ParseResult result;
  std::istringstream stream(header_block);
  std::string request_line;
  if (!std::getline(stream, request_line)) {
    result.status_code = 400;
    result.error_message = "Malformed request line";
    return result;
  }

  StripCarriageReturn(&request_line);
  std::istringstream request_stream(request_line);
  HttpRequest request;
  if (!(request_stream >> request.method >> request.target >> request.version)) {
    result.status_code = 400;
    result.error_message = "Invalid request line";
    return result;
  }

  if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
    result.status_code = 400;
    result.error_message = "Unsupported HTTP version";
    return result;
  }

  std::string line;
  while (std::getline(stream, line)) {
    StripCarriageReturn(&line);
    if (line.empty()) {
      continue;
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
      result.status_code = 400;
      result.error_message = "Malformed header";
      return result;
    }

    const std::string key = utils::ToLower(utils::Trim(line.substr(0, colon)));
    const std::string value = utils::Trim(line.substr(colon + 1));
    request.headers[key] = value;
  }

  if (request.headers.find("transfer-encoding") != request.headers.end()) {
    result.status_code = 400;
    result.error_message = "Chunked transfer encoding is not supported";
    return result;
  }

  std::size_t content_length = 0;
  const auto content_length_it = request.headers.find("content-length");
  if (content_length_it != request.headers.end()) {
    try {
      content_length =
          static_cast<std::size_t>(std::stoull(content_length_it->second));
    } catch (const std::exception&) {
      result.status_code = 400;
      result.error_message = "Invalid Content-Length";
      return result;
    }
  }

  if (content_length > max_body_bytes) {
    result.status_code = 413;
    result.error_message = "Request body exceeds configured limit";
    return result;
  }

  if (body.size() < content_length) {
    result.status_code = 400;
    result.error_message = "Incomplete request body";
    return result;
  }

  request.body = body.substr(0, content_length);

  const std::size_t query_separator = request.target.find('?');
  const std::string raw_path =
      query_separator == std::string::npos
          ? request.target
          : request.target.substr(0, query_separator);
  request.query = query_separator == std::string::npos
                      ? ""
                      : request.target.substr(query_separator + 1);
  request.path =
      utils::NormalizeUrlPath(utils::UrlDecode(raw_path.empty() ? "/" : raw_path));

  result.success = true;
  result.status_code = 200;
  result.request = std::move(request);
  return result;
}

}  // namespace

HttpParser::ParseResult HttpParser::ReadFromSocket(int client_socket,
                                                   std::size_t max_header_bytes,
                                                   std::size_t max_body_bytes) {
  ParseResult result;
  std::string buffer;
  buffer.reserve(kReadBufferSize);

  std::size_t header_end = std::string::npos;
  while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
    if (buffer.size() >= max_header_bytes) {
      result.status_code = 400;
      result.error_message = "Request headers too large";
      return result;
    }

    char read_buffer[kReadBufferSize];
    const ssize_t received =
        ::recv(client_socket, read_buffer, sizeof(read_buffer), 0);
    if (received == 0) {
      result.status_code = 400;
      result.error_message = "Client closed connection early";
      return result;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (IsReceiveTimeout(errno)) {
        result.status_code = 408;
        result.error_message = "Request timed out while reading headers";
      } else {
        result.status_code = 400;
        result.error_message =
            std::string("Unable to read request: ") + std::strerror(errno);
      }
      return result;
    }

    buffer.append(read_buffer, static_cast<std::size_t>(received));
    if (buffer.size() > max_header_bytes) {
      result.status_code = 400;
      result.error_message = "Request headers too large";
      return result;
    }
  }

  const std::string header_block = buffer.substr(0, header_end);
  const std::size_t body_offset = header_end + 4;

  std::size_t content_length = 0;
  {
    const std::string lower_headers = utils::ToLower(header_block);
    const auto content_length_pos = lower_headers.find("content-length:");
    if (content_length_pos != std::string::npos) {
      const auto line_end = lower_headers.find("\r\n", content_length_pos);
      const std::string line =
          header_block.substr(content_length_pos,
                              line_end == std::string::npos
                                  ? std::string::npos
                                  : line_end - content_length_pos);
      const auto colon = line.find(':');
      if (colon != std::string::npos) {
        try {
          content_length = static_cast<std::size_t>(
              std::stoull(utils::Trim(line.substr(colon + 1))));
        } catch (const std::exception&) {
          result.status_code = 400;
          result.error_message = "Invalid Content-Length";
          return result;
        }
      }
    }
  }

  if (content_length > max_body_bytes) {
    result.status_code = 413;
    result.error_message = "Request body exceeds configured limit";
    return result;
  }

  while (buffer.size() - body_offset < content_length) {
    char read_buffer[kReadBufferSize];
    const ssize_t received =
        ::recv(client_socket, read_buffer, sizeof(read_buffer), 0);
    if (received == 0) {
      result.status_code = 400;
      result.error_message = "Client closed connection before sending body";
      return result;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (IsReceiveTimeout(errno)) {
        result.status_code = 408;
        result.error_message = "Request timed out while reading body";
      } else {
        result.status_code = 400;
        result.error_message =
            std::string("Unable to read request body: ") + std::strerror(errno);
      }
      return result;
    }
    buffer.append(read_buffer, static_cast<std::size_t>(received));
  }

  const std::string body = buffer.substr(body_offset, content_length);
  return ParseHeaderBlock(header_block, body, max_body_bytes);
}

}  // namespace concurrent_http
