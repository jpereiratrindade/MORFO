#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <sqlite3.h>

namespace morfo::service {

// Shared Profile Service: Single source of truth for perceptual profiles across Web, REST and MCP
class ProfileService {
public:
    static std::string catalog_json();
    static bool is_valid_profile(std::string_view profile_id);
    static std::string get_profile_semantics(std::string_view profile_id);
};

// McpDispatcher: Implements Model Context Protocol 2026 (JSON-RPC 2.0)
// Exposes tools, resources, and prompts over Streamable HTTP (/mcp) or stdio
class McpDispatcher {
public:
    explicit McpDispatcher(sqlite3* db = nullptr);

    // Process a single JSON-RPC 2.0 message and return the JSON response
    std::string dispatch(std::string_view json_request, const std::string& user_id = "");

private:
    sqlite3* db_{nullptr};

    std::string handle_initialize(std::string_view id);
    std::string handle_ping(std::string_view id);
    std::string handle_tools_list(std::string_view id);
    std::string handle_tools_call(std::string_view id, std::string_view params_json, const std::string& user_id);
    std::string handle_resources_list(std::string_view id);
    std::string handle_resources_read(std::string_view id, std::string_view params_json);
    std::string handle_prompts_list(std::string_view id);
    std::string handle_prompts_get(std::string_view id, std::string_view params_json);

    std::string make_error_response(std::string_view id, int code, std::string_view message);
};

} // namespace morfo::service
