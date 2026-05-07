# Octane

A high-performance async HTTP/1.1 web framework written in C++20.

Built on Boost.Asio with a thread pool scaled to hardware concurrency, a hybrid router (O(1) hash map for static routes, trie for dynamic), zero-copy file serving via `mmap`, and CRTP-based route handlers with no virtual dispatch overhead.

**~300k req/s** on a standard development machine.

---

## Features

- Async TCP server — Boost.Asio thread pool, one thread per core
- Persistent keep-alive connections
- Hybrid router — O(1) static route lookup, trie for dynamic routes with path params
- CRTP route handlers — no vtable, no virtual dispatch
- HTTP/1.1 parser — request line, headers, query string, body (Content-Length)
- Zero-copy static file serving via `mmap`
- POST/PUT/PATCH body parsing via two-phase Content-Length reads
- Single include, two-line route registration

---

## Requirements

- C++20
- CMake 3.20+
- Boost (Asio)
- pthreads (Linux/macOS)

---

## Platform Support

| Platform | Status |
|---|---|
| Linux   | ✅ Full support |
| macOS   | ✅ Full support |
| Windows | ✅ Full support |

---

## Build

```bash
git clone https://github.com/yourusername/octane
cd octane
mkdir build && cd build
cmake ..
make
```

To skip examples:

```bash
cmake .. -DOCTANE_BUILD_EXAMPLES=OFF
```

To build a single example:

```bash
cmake --build . --target hello_world
```

---

## Quick Start

```cpp
#include "App.h"

// Define a route using CRTP
class HelloWorld : public routes::Get<HelloWorld> {
public:
    void handle(HttpRequest& req, HttpResponse& res) {
        res.status(200).body("Hello, World!");
    }
};

int main() {
    App app;
    app.get<HelloWorld>("/");
    app.listen(8080);
}
```

---

## Route Parameters

```cpp
class GetUser : public routes::Get<GetUser> {
public:
    void handle(HttpRequest& req, HttpResponse& res) {
        auto id = req.param("id");
        res.status(200).json("{\"id\":\"" + id + "\"}");
    }
};

app.get<GetUser>("/users/:id");
```

---

## POST Body

```cpp
class CreateUser : public routes::Post<CreateUser> {
public:
    void handle(HttpRequest& req, HttpResponse& res) {
        auto body = req.body; // raw body string
        res.status(201).json(body);
    }
};

app.post<CreateUser>("/users");
```

---

## Static File Serving

```cpp
class ServeFile : public routes::Get<ServeFile> {
public:
    void handle(HttpRequest& req, HttpResponse& res) {
        res.file("index.html"); // zero-copy via mmap
    }
};
```

Files are resolved relative to the executable directory.

---

## Supported HTTP Methods

| Method | CRTP Base |
|--------|-----------|
| GET | `routes::Get<T>` |
| POST | `routes::Post<T>` |
| PUT | `routes::Put<T>` |
| PATCH | `routes::Patch<T>` |
| DELETE | `routes::Del<T>` |
| OPTIONS | `routes::Options<T>` |

---

## Project Structure

```
octane/
├── include/
│   ├── App.h           # Thread pool, accept loop, entry point
│   ├── Router.h        # Hybrid static/dynamic router
│   ├── Trie.h          # Trie for dynamic route matching
│   ├── HttpParser.h    # Raw bytes → HttpRequest
│   ├── HttpRequest.h   # Request struct + helpers
│   ├── HttpResponse.h  # Response struct + serialization
│   ├── HttpTypes.h     # ContentType, HttpMethod, Handler
│   ├── RouteBase.h     # CRTP base classes
│   └── FileHandle.h    # mmap file serving
├── src/
│   └── FileHandle.cpp
├── examples/
│   ├── 01_hello_world/
│   └── 02_rest_api/
├── CMakeLists.txt
├── LICENSE
└── README.md
```

---

## Using as a Library (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(
    octane
    GIT_REPOSITORY https://github.com/cashinmartincj/octane
    GIT_TAG        v0.1.0
)
FetchContent_MakeAvailable(octane)

target_link_libraries(your_app PRIVATE octane_lib)
```

---

## Auth, Middleware & CORS

Octane is an infrastructure layer — routing, parsing, and I/O. Auth, CORS, and logging are application concerns handled inside your route handlers:

```cpp
class ProtectedRoute : public routes::Get<ProtectedRoute> {
public:
    void handle(HttpRequest& req, HttpResponse& res) {
        auto token = req.header("Authorization");
        if (token.empty()) {
            res.status(401).json("{\"error\":\"unauthorized\"}");
            return;
        }
        // proceed
    }
};
```

---

## License

MIT — see [LICENSE](LICENSE)