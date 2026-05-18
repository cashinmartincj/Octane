/**
 * @file Trie.h
 * @brief Prefix trie for O(k) dynamic URL route matching
 *
 *   root
 *    ├── "users"          → handler ✓
 *    │      ├── "ids"     → handler ✓  (static beats wildcard)
 *    │      └── :id       → handler ✓  (captures value)
 *    │            └── "posts" → handler ✓
 *    └── "health"
 *           └── :ui       → handler ✓
 *
 * Built once at startup — thread safe at runtime.
 */

#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>
#include "HttpTypes.h"

namespace octane
{
    struct TrieNode {
        std::unordered_map<std::string, std::unique_ptr<TrieNode>> children;
        std::unique_ptr<TrieNode> wildcard;
        std::string               wildcard_name;
        std::optional<Handler>    handler;
    };

    class Trie {
        public:
            void insert(const std::string& path, Handler handler) {
                auto parts   = split(path, '/');
                TrieNode* node = &root_;

                for (auto& part : parts) {
                    if (part.empty()) continue;
                    if (part[0] == ':') {
                        if (!node->wildcard)
                            node->wildcard = std::make_unique<TrieNode>();
                        node->wildcard_name = part.substr(1);
                        node = node->wildcard.get();
                    } else {
                        if (!node->children.count(part))
                            node->children[part] = std::make_unique<TrieNode>();
                        node = node->children[part].get();
                    }
                }
                node->handler = std::move(handler);
            }

            // Returns false if no route matched (caller sends 404)
            bool search(const std::string& path,
                        Handler& out_handler,
                        std::unordered_map<std::string, std::string>& params) const {

                auto parts = split(path, '/');
                const TrieNode* node = &root_;

                for (auto& part : parts) {
                    if (part.empty()) continue;

                    auto it = node->children.find(part);
                    if (it != node->children.end()) {
                        node = it->second.get();
                    } else if (node->wildcard) {
                        params[node->wildcard_name] = part;
                        node = node->wildcard.get();
                    } else {
                        return false;
                    }
                }

                if (!node->handler) return false;
                out_handler = *node->handler;
                return true;
            }

        private:
            TrieNode root_;

            static std::vector<std::string> split(const std::string& s, char delim) {
                std::vector<std::string> parts;
                std::string current;
                for (char c : s) {
                    if (c == delim) { parts.push_back(current); current.clear(); }
                    else current += c;
                }
                if (!current.empty()) parts.push_back(current);
                return parts;
            }
        };
}
