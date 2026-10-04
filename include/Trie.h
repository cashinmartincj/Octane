/**
 * @file Trie.h
 * @brief Prefix Radix Trie for dynamic parameterized URL routing (`/users/:id`).
 *
 * @details
 * `Trie` implements an adaptive Radix prefix tree optimized for URL path segmentation.
 * It maps route patterns containing parameter wildcards (e.g. `/api/v1/gyms/:gym_id/members/:member_id`)
 * to their corresponding `HandlerRoute` definitions.
 *
 * Architectural Features:
 * - **Adaptive Branching (Hybrid Linear/Hash):** For branches with $\le 8$ children, child lookup
 *   is performed via contiguous cache-friendly linear array scanning (`linear_children`). If fan-out
 *   exceeds 8 child branches, it automatically migrates to an `std::unordered_map` (`map_children`)
 *   using transparent string view hashing.
 * - **Zero-Allocation Parameter Binding:** Path segments matched by wildcards (`:param`) are
 *   extracted directly as `std::string_view` slices into the request URI without memory allocation.
 *
 * Where this is imported / used:
 * - Direct Include: `#include "Trie.h"`
 * - Router Core: Included and instantiated in `include/Router.h` (`Router::dynamic_routes_`).
 * - Request Dispatch: Traversed by `octane::Router::match()` when a static hash lookup misses.
 *
 * @author Octane Framework Team / FitOps Backend Core
 * @date 2026
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
    /**
     * @struct TrieNode
     * @brief A single node in the URL routing prefix tree representing a path segment.
     */
    struct TrieNode {
        /// Linear array for child branches (faster than hash map for typical route fan-out <= 8)
        std::vector<std::pair<std::string, std::unique_ptr<TrieNode>>> linear_children;
        
        /// Hash map fallback for wide branching (> 8 static subroutes)
        std::unordered_map<std::string, std::unique_ptr<TrieNode>, StringViewHash, std::equal_to<>> map_children;

        /// Child node representing a dynamic URL wildcard segment (e.g., `:id`)
        std::unique_ptr<TrieNode> wildcard;

        /// The name of the parameter associated with this wildcard node (e.g., "id")
        std::string               wildcard_name;

        /// The route handler attached if an HTTP route pattern terminates at this node
        std::optional<HandlerRoute> handler;

        /**
         * @brief Finds a matching static child node for a path segment.
         * @param part Path segment string view (without slashes).
         * @return Pointer to child TrieNode if found, or nullptr.
         */
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

        /**
         * @brief Retrieves existing child node or allocates a new one for insertion.
         * @details Automatically upgrades from linear array to unordered_map when fanout > 8.
         * @param part Path segment string view.
         * @return Pointer to the existing or newly created TrieNode.
         */
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

    /**
     * @class Trie
     * @brief Prefix Radix Trie organizing routes with path parameter support.
     */
    class Trie {
    public:
        /**
         * @brief Inserts a path template into the Trie, associating it with a route handler.
         * @param path Full URI path template (e.g. "/users/:id/profile").
         * @param route HandlerRoute containing the callback, execution mode, and queue config.
         */
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

        /**
         * @brief Matches an incoming URI path against registered Trie routes.
         * @param path Inbound request URI path view (e.g. "/users/42").
         * @param[out] out_route Pointer set to matching HandlerRoute if found.
         * @param[out] params Map populated with extracted parameter views (e.g. `{"id": "42"}`).
         * @return True if a matching route was found; false otherwise.
         */
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
        TrieNode root_; ///< Root node of the URL routing prefix tree
    };
} // namespace octane
