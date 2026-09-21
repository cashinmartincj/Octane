/**
 * @file Trie.h
 * @brief Prefix trie for zero-allocation dynamic URL route matching.
 */

#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>
#include <utility>
#include "HttpTypes.h"

namespace octane
{
    struct TrieNode {
        // Linear array for child branches (faster than hash map for typical route fan-out <= 8)
        std::vector<std::pair<std::string, std::unique_ptr<TrieNode>>> linear_children;
        
        // Hash map fallback for wide branching (> 8 static subroutes)
        std::unordered_map<std::string, std::unique_ptr<TrieNode>, StringViewHash, std::equal_to<>> map_children;

        std::unique_ptr<TrieNode> wildcard;
        std::string               wildcard_name;
        std::optional<HandlerRoute> handler;

        [[nodiscard]] const TrieNode* find_child(std::string_view part) const noexcept {
            if (!map_children.empty()) {
                auto it = map_children.find(part);
                return (it != map_children.end()) ? it->second.get() : nullptr;
            }
            for (const auto& [seg, child] : linear_children) {
                if (seg == part) return child.get();
            }
            return nullptr;
        }

        TrieNode* get_or_create_child(std::string_view part) {
            for (auto& [seg, child] : linear_children) {
                if (seg == part) return child.get();
            }
            if (map_children.empty() && linear_children.size() < 8) {
                linear_children.emplace_back(std::string(part), std::make_unique<TrieNode>());
                return linear_children.back().second.get();
            }
            // Migrate to map if fanout becomes large
            if (map_children.empty()) {
                for (auto& [seg, child] : linear_children) {
                    map_children.emplace(seg, std::move(child));
                }
                linear_children.clear();
            }
            auto& node = map_children[std::string(part)];
            if (!node) node = std::make_unique<TrieNode>();
            return node.get();
        }
    };

    class Trie {
    public:
        void insert(const std::string& path, HandlerRoute route) {
            TrieNode* node = &root_;
            std::size_t start = 0;
            const std::size_t len = path.size();

            while (start < len) {
                while (start < len && path[start] == '/') ++start;
                if (start >= len) break;

                std::size_t end = path.find('/', start);
                if (end == std::string::npos) end = len;

                std::string_view part = std::string_view(path).substr(start, end - start);
                start = end;

                if (part.empty()) continue;

                if (part[0] == ':') {
                    if (!node->wildcard) {
                        node->wildcard = std::make_unique<TrieNode>();
                    }
                    node->wildcard_name = std::string(part.substr(1));
                    node = node->wildcard.get();
                } else {
                    node = node->get_or_create_child(part);
                }
            }
            node->handler = std::move(route);
        }

        [[nodiscard]] bool search(std::string_view path,
                                  const HandlerRoute*& out_route,
                                  StringMap& params) const noexcept {
            const TrieNode* node = &root_;
            std::size_t start = 0;
            const std::size_t len = path.size();

            while (start < len) {
                while (start < len && path[start] == '/') ++start;
                if (start >= len) break;

                std::size_t end = path.find('/', start);
                if (end == std::string::npos) end = len;

                std::string_view part = path.substr(start, end - start);
                start = end;

                if (part.empty()) continue;

                const TrieNode* next = node->find_child(part);
                if (next) {
                    node = next;
                } else if (node->wildcard) {
                    params[node->wildcard_name] = part;
                    node = node->wildcard.get();
                } else {
                    return false;
                }
            }

            if (!node->handler) return false;
            out_route = &*node->handler;
            return true;
        }

    private:
        TrieNode root_;
    };
} // namespace octane
