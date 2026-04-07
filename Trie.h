#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>
#include "HttpTypes.h"

// Handler type — same as before
// ─────────────────────────────────────────────
//  TrieNode
//  Each node represents one URL segment
//  e.g. /users/:id/posts → root → users → :id → posts
// ─────────────────────────────────────────────
struct TrieNode {
    // Static children  e.g. "users", "posts", "products"
    std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;

    // Wildcard child   e.g. ":id", ":postId"
    // Only one wildcard allowed per level
    std::unique_ptr<TrieNode> wildcard;
    std::string               wildcard_name;   // "id", "postId" (stripped of ':')

    // If a route ends at this node, store its handler
    std::optional<Handler> handler;
};

// ─────────────────────────────────────────────
//  Trie
//  Built once at startup — read-only at runtime
//  Thread safe for concurrent reads (no locks needed)
// ─────────────────────────────────────────────
class Trie {
public:

    // ── Insert ──────────────────────────────────
    // Called at startup when routes are registered
    // path format: "GET:/users/:id/posts/:postId"
    void insert(const std::string& path, Handler handler) {
        auto parts   = split(path, '/');
        TrieNode* node = &root_;

        for (auto& part : parts) {
            if (part.empty()) continue;  // skip leading slash

            if (part[0] == ':') {
                // Wildcard segment
                if (!node->wildcard)
                    node->wildcard = std::make_unique<TrieNode>();
                node->wildcard_name = part.substr(1);  // strip ':'
                node = node->wildcard.get();
            } else {
                // Static segment
                if (!node->children.count(part))
                    node->children[part] = std::make_unique<TrieNode>();
                node = node->children[part].get();
            }
        }
        node->handler = std::move(handler);
    }

    // ── Search ──────────────────────────────────
    // Called on every request — must be fast
    // path format: "GET:/users/42/posts/7"
    // Fills params map with captured values
    // Returns true if a matching route was found
    bool search(const std::string& path,
                Handler& out_handler,
                std::unordered_map<std::string, std::string>& params) const {

        auto parts = split(path, '/');
        const TrieNode* node = &root_;

        for (auto& part : parts) {
            if (part.empty()) continue;

            // 1. Try exact static match first — O(1)
            auto it = node->children.find(part);
            if (it != node->children.end()) {
                node = it->second.get();
            }
            // 2. Fall back to wildcard if exists
            else if (node->wildcard) {
                params[node->wildcard_name] = part;   // capture value
                node = node->wildcard.get();
            }
            // 3. No match at this segment — dead end
            else {
                return false;
            }
        }

        if (!node->handler) return false;
        out_handler = *node->handler;
        return true;
    }

private:
    TrieNode root_;

    // Split string by delimiter into vector of parts
    static std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : s) {
            if (c == delim) {
                parts.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) parts.push_back(current);
        return parts;
    }
};