/**
 * @file main.cpp
 * @brief 02_rest_api — full RESTful CRUD example
 *
 *   GET    /health         — inline execution
 *   GET    /users          — list all users
 *   GET    /users/:id      — get one user
 *   POST   /users          — create a user
 *   PUT    /users/:id      — full replace
 *   PATCH  /users/:id      — partial update
 *   DELETE /users/:id      — delete a user
 */

#include "App.h"
#include "RouteBase.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <thread>

using namespace octane;

// ── in-memory store ───────────────────────────
struct User {
    std::string id;
    std::string name;
    std::string email;

    std::string toJson() const {
        return "{\"id\":\"" + id + "\","
               "\"name\":\"" + name + "\","
               "\"email\":\"" + email + "\"}";
    }
};

// ── simple JSON field extractor ───────────────
static std::string extractField(const std::string& body, const std::string& key) {
    auto pos = body.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = body.find(":", pos);
    pos = body.find("\"", pos) + 1;
    auto end = body.find("\"", pos);
    return body.substr(pos, end - pos);
}

static std::unordered_map<std::string, User> store = {
    { "1", { "1", "Alice", "alice@example.com" } },
    { "2", { "2", "Bob",   "bob@example.com"   } },
};
static std::mutex store_mutex;
static std::atomic<int> next_id{3};

// ── helpers ───────────────────────────────────
static std::string all_users_json() {
    std::string body = "[";
    for (auto& [id, user] : store)
        body += user.toJson() + ",";
    if (body.back() == ',') body.pop_back();
    body += "]";
    return body;
}

// ── GET /health (inline) ─────────────────────
// Small, non-blocking handlers should stay on the owning io_uring worker.
class Health : public routes::Get<Health> {
public:
    void handle(const HttpRequest&, HttpResponse& res) {
        res.status(200).json(R"({"status":"ok"})");
    }
};

// ── GET /users (shared blocking queue) ────────
// This demonstrates the queue shared by all routes selecting
// HandlerExecution::shared_blocking() on the same ring shard.
class GetUsers : public routes::Get<GetUsers> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        std::lock_guard lock(store_mutex);
        res.status(200).json(all_users_json());
    }
};

// ── GET /users/:id ────────────────────────────
class GetUser : public routes::Get<GetUser> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        std::string id(req.param("id"));

        std::lock_guard lock(store_mutex);
        auto it = store.find(id);
        if (it == store.end()) {
            res.status(404).json(R"({"error":"user not found"})");
            return;
        }
        res.status(200).json(it->second.toJson());
    }
};

// ── POST /users ───────────────────────────────
class CreateUser : public routes::Post<CreateUser> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        if (req.body.empty()) {
            res.status(400).json(R"({"error":"body required"})");
            return;
        }

        std::string id    = std::to_string(next_id++);
        std::string name  = extractField(req.body, "name");
        std::string email = extractField(req.body, "email");

        User user{ id, name, email };

        std::lock_guard lock(store_mutex);
        store[id] = user;
        res.status(201).json(user.toJson());
    }
};

// ── PUT /users/:id ────────────────────────────
class ReplaceUser : public routes::Put<ReplaceUser> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        std::string id(req.param("id"));

        if (req.body.empty()) {
            res.status(400).json(R"({"error":"body required"})");
            return;
        }

        std::lock_guard lock(store_mutex);
        auto it = store.find(id);
        if (it == store.end()) {
            res.status(404).json(R"({"error":"user not found"})");
            return;
        }

        it->second = {
            id,
            extractField(req.body, "name"),
            extractField(req.body, "email")
        };
        res.status(200).json(it->second.toJson());
    }
};

// ── PATCH /users/:id ──────────────────────────
class UpdateUser : public routes::Patch<UpdateUser> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        std::string id(req.param("id"));

        if (req.body.empty()) {
            res.status(400).json(R"({"error":"body required"})");
            return;
        }

        std::lock_guard lock(store_mutex);
        auto it = store.find(id);
        if (it == store.end()) {
            res.status(404).json(R"({"error":"user not found"})");
            return;
        }

        auto name  = extractField(req.body, "name");
        auto email = extractField(req.body, "email");
        if (!name.empty())  it->second.name  = name;
        if (!email.empty()) it->second.email = email;

        res.status(200).json(it->second.toJson());
    }
};

// ── DELETE /users/:id ─────────────────────────
class DeleteUser : public routes::Del<DeleteUser> {
public:
    void handle(const HttpRequest& req, HttpResponse& res) {
        std::string id(req.param("id"));

        std::lock_guard lock(store_mutex);
        auto it = store.find(id);
        if (it == store.end()) {
            res.status(404).json(R"({"error":"user not found"})");
            return;
        }

        store.erase(it);
        res.status(204).send("");
    }
};

int main() {
    App app;

    // Explicit Inline policy. Omitting the policy has the same behavior.
    app.get<Health>("/health", HandlerExecution::inline_execution());

    // Shared bounded blocking queue.
    app.get<GetUsers>("/users", HandlerExecution::shared_blocking());

    // Dedicated named queue for the simulated database workload.
    const auto database = HandlerExecution::named("database");
    app.get<GetUser>("/users/:id", database);
    app.post<CreateUser>("/users", database);
    app.put<ReplaceUser>("/users/:id", database);
    app.patch<UpdateUser>("/users/:id", database);
    app.del<DeleteUser>("/users/:id", database);

    transport::TcpServerOptions transport_options;
    transport_options.execution_queues.shared_threads_per_shard = 1;
    transport_options.execution_queues.shared_capacity = 128;
    transport_options.execution_queues.named.push_back({
        "database",
        1,   // Worker threads created per io_uring shard when first used.
        256  // Maximum waiting jobs per shard.
    });

    HttpLimits limits;
    // Keep the tutorial's ring and auxiliary queue thread count modest.
    const int ring_workers = static_cast<int>(
        std::min(4u, std::max(1u, std::thread::hardware_concurrency())));
    app.listen(8080, ring_workers, limits, transport_options);
}

/**
 *    # list all
 *    curl http://localhost:8080/users
 *
 *    # inline health check
 *    curl http://localhost:8080/health
 *
 *    # get one
 *    curl http://localhost:8080/users/1
 *
 *    # create
 *    curl -X POST http://localhost:8080/users \
 *         -H "Content-Type: application/json" \
 *         -d '{"name":"Cashin Martin","email":"cashinmartincj@outlook.com"}'
 *
 *    # full replace
 *    curl -X PUT http://localhost:8080/users/1 \
 *         -H "Content-Type: application/json" \
 *         -d '{"name":"Alice New","email":"new@example.com"}'
 *
 *    # partial update
 *    curl -X PATCH http://localhost:8080/users/1 \
 *         -H "Content-Type: application/json" \
 *         -d '{"name":"Alice Patched"}'
 *
 *    # delete
 *    curl -X DELETE http://localhost:8080/users/1
 *
 *    # verify deletion
 *    curl http://localhost:8080/users
 */
