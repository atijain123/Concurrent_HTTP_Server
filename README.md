# Concurrent HTTP Server

A C++17 HTTP/1.1 static-file server with a fixed-size worker pool, workload-aware in-memory caching, predictive prefetching, request logging, runtime metrics, and a React monitoring dashboard.

The server is written for POSIX socket environments such as Linux, macOS, or WSL. The monitoring dashboard is a Vite React app that builds into `www/monitor/` and is served by the C++ server.

## Features

- Concurrent request handling through a fixed-size FIFO thread pool.
- HTTP/1.0 and HTTP/1.1 request parsing with header and body size limits.
- Static file serving from a configurable document root.
- Path normalization and root-bound path resolution to prevent directory traversal.
- `POST /echo` endpoint for simple request-body testing.
- Workload-aware in-memory cache with adaptive recency/frequency scoring.
- Predictive prefetching for HTML-linked local CSS, JS, and image resources.
- Structured request, info, and error logs in `logs/server.log`.
- JSON runtime metrics at `/metrics`.
- React monitoring dashboard at `/monitor/`.

## Project Layout

```text
.
|-- main.cpp
|-- Makefile
|-- cache/
|   |-- cache.h
|   `-- cache.cpp
|-- logger/
|   |-- logger.h
|   `-- logger.cpp
|-- metrics/
|   |-- metrics.h
|   `-- metrics.cpp
|-- monitor-ui/
|   |-- src/
|   |-- package.json
|   `-- vite.config.ts
|-- prefetch/
|   |-- prefetcher.h
|   `-- prefetcher.cpp
|-- server/
|   |-- http_parser.h
|   |-- http_parser.cpp
|   |-- http_server.h
|   |-- http_server.cpp
|   `-- http_types.h
|-- threadpool/
|   |-- thread_pool.h
|   `-- thread_pool.cpp
|-- utils/
|   |-- file_utils.h
|   |-- file_utils.cpp
|   |-- http_utils.h
|   `-- http_utils.cpp
`-- www/
    |-- index.html
    |-- web/
    `-- monitor/
```

## Requirements

### Server

- A C++17 compiler such as `g++`.
- POSIX socket headers and runtime support.
- `make` for the provided Makefile.

On Windows, build and run from WSL or another POSIX-compatible environment. Native MinGW builds will fail unless the networking layer is ported away from POSIX headers such as `arpa/inet.h` and `sys/socket.h`.

### Monitoring UI

- Node.js and npm.
- Dependencies installed in `monitor-ui/`.

## Build

Build the C++ server:

```bash
make
```

Equivalent compiler command:

```bash
g++ -I. -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread \
  main.cpp \
  server/http_server.cpp server/http_parser.cpp \
  threadpool/thread_pool.cpp cache/cache.cpp \
  prefetch/prefetcher.cpp logger/logger.cpp metrics/metrics.cpp \
  utils/file_utils.cpp utils/http_utils.cpp \
  -o concurrent_http_server
```

Build the monitoring dashboard after changing files under `monitor-ui/`:

```bash
cd monitor-ui
npm install
npm run build
```

The Vite build writes static assets directly to `www/monitor/`.

## Run

```bash
./concurrent_http_server
```

Optional arguments:

```text
./concurrent_http_server [port] [document_root] [thread_count] [cache_size_mb]
```

Examples:

```bash
./concurrent_http_server 8080 ./www 8 64
./concurrent_http_server 9090 ./www 4 32
```

When the server starts, it prints the document root, monitoring dashboard URL, and metrics URL.

## HTTP Endpoints

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/` | Serves `www/index.html`. |
| `GET` | `/web/` | Serves the hosted website content. |
| `GET` | `/monitor` | Redirects to `/monitor/`. |
| `GET` | `/monitor/` | Serves the React monitoring dashboard. |
| `GET` | `/metrics` | Returns live server metrics as JSON. |
| `GET` | `/metrics-view` | Serves a built-in HTML metrics page. |
| `POST` | `/echo` | Returns posted body content as JSON. |
| `GET` | any static path | Serves files below the configured document root. |

Unsupported methods return `405 Method Not Allowed`. Nonexistent files return `404 Not Found`. Idle or slow clients that do not send a complete request before the socket receive timeout get `408 Request Timeout`.

## Configuration

Default server configuration is defined in `server/http_server.h`:

| Field | Default | Purpose |
| --- | --- | --- |
| `host` | `0.0.0.0` | Bind address. |
| `port` | `8080` | TCP port. |
| `document_root` | `./www` | Static file root. |
| `log_file` | `./logs/server.log` | Log output path. |
| `thread_count` | `4` | Worker count before `main.cpp` replaces it with hardware concurrency. |
| `max_cache_bytes` | `32 MB` | Cache memory budget. |
| `cache_alpha` | `1.0` | Initial cache frequency weight. |
| `cache_beta` | `5.0` | Initial cache recency weight. |
| `prefetch_cache_threshold` | `0.80` | Stop prefetching once cache usage reaches this ratio. |
| `max_prefetch_resources` | `4` | Maximum resources prefetched per HTML document. |
| `max_prefetch_file_bytes` | `1 MB` | Maximum size for a prefetched file. |
| `mirror_logs_to_console` | `true` | Also write logs to stdout. |

Command-line arguments can override port, document root, thread count, and cache size.

## Request Handling Flow

1. `main.cpp` builds a `ServerConfig`, installs shutdown handlers, starts `HttpServer`, and waits for `SIGINT` or `SIGTERM`.
2. `HttpServer::Start()` creates the listening socket, starts the worker pool, starts the prefetcher, and launches an accept thread.
3. The accept loop accepts TCP clients, applies socket timeouts, records active connections, and enqueues each client into the thread pool.
4. A worker parses one request, routes it, sends one response, records client-facing metrics for non-internal routes, adjusts the cache strategy, and closes the connection.
5. Static `GET` requests first check the cache. Misses read from disk, populate the cache, and may trigger HTML resource prefetching.

Connections are closed after each response. Persistent keep-alive is not implemented.

## HTTP Parser

The parser:

- Reads headers until `\r\n\r\n`.
- Supports HTTP/1.0 and HTTP/1.1 request lines.
- Normalizes header names to lowercase.
- Supports `Content-Length` bodies up to 2 MB by default.
- Rejects chunked transfer encoding.
- URL-decodes and normalizes the request path.
- Splits query strings from paths.

Default parser limits:

| Limit | Value |
| --- | --- |
| Header bytes | `64 KB` |
| Body bytes | `2 MB` |
| Read buffer | `4096 bytes` |

## Static File Serving

Static paths are normalized through `utils::NormalizeUrlPath()` and resolved with `utils::ResolveRequestPath()`.

Resolution rules:

- `/` maps to `/index.html`.
- Paths ending in `/` map to `index.html` inside that directory.
- Backslashes are normalized to slashes.
- Query strings and fragments are stripped for filesystem resolution.
- The resolved path must stay inside the configured document root.

MIME types are selected from common file extensions in `utils/http_utils.cpp`.

The `www/web/` directory is the intended location for the hosted website. Monitoring files under `/monitor/` and metrics endpoints are internal server UI/API routes; they are served without being inserted into the workload-aware cache and without contributing to client-facing traffic counters or access logs.

## Workload-Aware Cache

The cache stores hosted website file content, content type, file size, access frequency, last access time, and eviction metadata. Internal monitoring and metrics routes are not stored in the cache.

Each entry keeps precomputed scores for the three supported cache profiles:

| Profile | Trigger | Alpha | Beta | Behavior |
| --- | --- | ---: | ---: | --- |
| Recency-heavy | cache hit rate `< 0.5` | `0.5` | `8.0` | Favors recently accessed entries. |
| Balanced | cache hit rate `0.5` through `0.8` | `1.0` | `5.0` | Balances frequency and recency. |
| Frequency-heavy | cache hit rate `> 0.8` | `2.0` | `3.0` | Favors repeatedly accessed entries. |

Score formula:

```text
score = alpha * access_frequency + beta * recency
recency = 1 / (age_seconds + 1)
```

The cache maintains one eviction index per profile. When the active profile changes, eviction can use the already stored index for that profile instead of rebuilding all scores. On entry insert or hit, the entry refreshes its score in all three profile indexes.

Complexity:

| Operation | Complexity |
| --- | --- |
| Cache lookup miss | `O(1)` average map lookup. |
| Cache hit | `O(P log N)` for `P = 3` score profiles. |
| Cache insert | `O(P log N)` plus any eviction work. |
| Profile switch | `O(1)`. |
| Evict one entry | `O(P log N)`. |
| Cache metadata space | `O(PN)` for three eviction indexes and score sets. |

This trades a small, fixed amount of extra memory for avoiding full eviction-index rebuilds when the adaptive alpha/beta mode changes.

## Predictive Prefetching

Prefetching runs on a dedicated background thread and is only scheduled for HTML responses.

The prefetcher extracts local resources from:

- `<link href="...">`
- `<script src="...">`
- `<img src="...">`

It skips:

- External URLs.
- Data URLs.
- Already cached resources.
- Resources outside the document root.
- Files larger than the configured prefetch size.
- Work when cache usage is above the threshold.
- Work when active connections exceed the heavy-load threshold.

Successful prefetches are inserted into the same workload-aware cache and logged.

## Metrics

`GET /metrics` returns JSON like:

```json
{
  "active_connections": 1,
  "total_requests": 42,
  "requests_per_second": 3,
  "cache_hit_rate": 0.75,
  "cache_hits": 30,
  "cache_misses": 10,
  "approximate_memory_usage_bytes": 8192,
  "cache_usage_bytes": 8192,
  "cache_usage_ratio": 0.000244,
  "thread_count": 8,
  "cache_alpha": 1,
  "cache_beta": 5
}
```

Metrics are updated after each handled non-internal request. Requests per second is calculated from client request completion timestamps in the last one-second window, so opening the monitoring dashboard does not inflate the displayed traffic. Active connections also represent client website requests, not monitoring or metrics polling.

## Monitoring Dashboard

The dashboard source lives in `monitor-ui/` and uses:

- React.
- TypeScript.
- Vite.
- Tailwind CSS.
- shadcn-style UI components.
- Recharts.

Open the dashboard at:

```text
http://127.0.0.1:8080/monitor/
```

The dashboard polls `/metrics` once per second while the document is visible. It shows:

- Active connections.
- Requests per second.
- Cache hit rate.
- Thread count.
- Cache memory usage.
- Cache hits vs misses pie chart.
- Recent request-rate line chart.
- Active cache strategy and alpha/beta values.

When there are no cache hits or misses yet, the pie chart area shows an explicit empty state instead of rendering a zero-value chart.

For local dashboard development:

```bash
cd monitor-ui
npm run dev
```

The Vite dev server proxies `/metrics` to `http://127.0.0.1:8080`, so start the C++ server first.

## Logging

Logs are written to `logs/server.log` and optionally mirrored to stdout.

Examples:

```text
[2026-04-20T18:00:00] [INFO] Server listening on 0.0.0.0:8080 with document root ./www
[2026-04-20T18:00:03] [REQUEST] 127.0.0.1 "/" 200
[2026-04-20T18:00:04] [INFO] Prefetched /styles.css
```

## Manual Testing

Basic static requests:

```bash
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/monitor/
curl -i http://127.0.0.1:8080/does-not-exist
```

Metrics:

```bash
curl -i http://127.0.0.1:8080/metrics
```

Echo endpoint:

```bash
curl -i -X POST http://127.0.0.1:8080/echo -d 'hello from client'
```

Cache behavior:

```bash
curl -s http://127.0.0.1:8080/ > /dev/null
curl -s http://127.0.0.1:8080/ > /dev/null
curl -s http://127.0.0.1:8080/metrics
```

Concurrency:

```bash
seq 1 100 | xargs -I{} -P 20 curl -s http://127.0.0.1:8080/ > /dev/null
ab -n 500 -c 50 http://127.0.0.1:8080/
```

## Cleanup

```bash
make clean
```

This removes object files and the `concurrent_http_server` binary.

## Notes and Limitations

- The server handles one request per TCP connection and then closes the connection.
- Chunked request bodies are rejected.
- TLS is not implemented.
- Range requests are not implemented.
- The cache is in-memory only and is rebuilt on process restart.
- The C++ server targets POSIX sockets; native Windows socket support would require a portability layer.
