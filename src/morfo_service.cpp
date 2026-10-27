#include "morfo_service.hpp"
#include "morfo_profiles.hpp"

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace morfo::service {

namespace {

std::string esc(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 32) {
                    char b[7];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string q(std::string_view s) {
    return "\"" + esc(s) + "\"";
}

std::string extract_field(std::string_view body, std::string_view key) {
    std::string pat = "\"" + std::string(key) + "\"";
    auto pos = body.find(pat);
    if (pos == std::string_view::npos) return "";
    auto colon = body.find(':', pos + pat.size());
    if (colon == std::string_view::npos) return "";
    auto start = body.find_first_not_of(" \t\r\n", colon + 1);
    if (start == std::string_view::npos) return "";

    if (body[start] == '"') {
        std::string val;
        for (std::size_t i = start + 1; i < body.size(); ++i) {
            char c = body[i];
            if (c == '"') return val;
            if (c != '\\') { val += c; continue; }
            if (++i >= body.size()) return "";
            char e = body[i];
            if (e == '"' || e == '\\' || e == '/') val += e;
            else if (e == 'n') val += '\n';
            else if (e == 'r') val += '\r';
            else if (e == 't') val += '\t';
        }
        return "";
    } else {
        auto end = body.find_first_of(",}] \t\r\n", start);
        return std::string(body.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    }
}

std::string extract_object(std::string_view body, std::string_view key) {
    std::string pat = "\"" + std::string(key) + "\"";
    auto pos = body.find(pat);
    if (pos == std::string_view::npos) return "{}";
    auto colon = body.find(':', pos + pat.size());
    if (colon == std::string_view::npos) return "{}";
    auto brace = body.find('{', colon + 1);
    if (brace == std::string_view::npos) return "{}";
    int depth = 0;
    for (std::size_t i = brace; i < body.size(); ++i) {
        if (body[i] == '{') depth++;
        else if (body[i] == '}') {
            depth--;
            if (depth == 0) {
                return std::string(body.substr(brace, i - brace + 1));
            }
        }
    }
    return "{}";
}

} // namespace

std::string ProfileService::catalog_json() {
    return morfo::profiles::registry_json();
}

bool ProfileService::is_valid_profile(std::string_view profile_id) {
    return profile_id == "roots-exp003" || profile_id == "discovery";
}

std::string ProfileService::get_profile_semantics(std::string_view profile_id) {
    if (profile_id == "roots-exp003") return "ANOMALY_CANDIDATE";
    if (profile_id == "discovery") return "UNNAMED_MORPHOTYPES";
    return "UNKNOWN";
}

McpDispatcher::McpDispatcher(sqlite3* db) : db_(db) {}

std::string McpDispatcher::make_error_response(std::string_view id, int code, std::string_view message) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":"
       << (id.empty() || id == "null" ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"error\":{\"code\":" << code << ",\"message\":" << q(message) << "}}";
    return ss.str();
}

std::string McpDispatcher::handle_initialize(std::string_view id) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"result\":{"
       << "\"protocolVersion\":\"2024-11-05\","
       << "\"capabilities\":{"
       << "\"tools\":{},"
       << "\"resources\":{},"
       << "\"prompts\":{}"
       << "},"
       << "\"serverInfo\":{"
       << "\"name\":\"morfo-mcp-server\","
       << "\"version\":\"0.2.0\""
       << "}"
       << "}}";
    return ss.str();
}

std::string McpDispatcher::handle_ping(std::string_view id) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"result\":{}}";
    return ss.str();
}

std::string McpDispatcher::handle_tools_list(std::string_view id) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"result\":{\"tools\":["
       << "{"
       << "\"name\":\"morfo_list_profiles\","
       << "\"description\":\"Lista todos os perfis temáticos cadastrados no núcleo MORFO, suas versões e estado epistêmico.\","
       << "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
       << "},"
       << "{"
       << "\"name\":\"morfo_list_images\","
       << "\"description\":\"Consulta as imagens originais registradas no acervo do MORFO, com dimensões e hashes SHA-256.\","
       << "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\",\"default\":20}}}"
       << "},"
       << "{"
       << "\"name\":\"morfo_get_analysis\","
       << "\"description\":\"Recupera os resultados quantitativos, evidências e estado ontológico de uma análise executada.\","
       << "\"inputSchema\":{\"type\":\"object\",\"required\":[\"analysis_id\"],\"properties\":{\"analysis_id\":{\"type\":\"string\"}}}"
       << "}"
       << "]}}";
    return ss.str();
}

std::string McpDispatcher::handle_tools_call(std::string_view id, std::string_view params_json, const std::string& user_id) {
    const std::string name = extract_field(params_json, "name");
    const std::string args_json = extract_object(params_json, "arguments");

    if (user_id.empty()) {
        return make_error_response(id, -32001, "Authentication required");
    }

    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id))) << ",\"result\":{\"content\":[";

    if (name == "morfo_list_profiles") {
        std::string catalog = ProfileService::catalog_json();
        ss << "{\"type\":\"text\",\"text\":" << q(catalog) << "}";
    } else if (name == "morfo_list_images") {
        std::ostringstream img_list;
        img_list << "{\"images\":[";
        if (db_) {
            const char* sql = "SELECT id, filename, sha256, width, height, created_at FROM images WHERE deleted_at IS NULL AND user_id=? ORDER BY created_at DESC LIMIT 20";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, user_id.c_str(), -1, SQLITE_STATIC);
                bool first = true;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (!first) img_list << ",";
                    first = false;
                    const char* id_val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const char* fn_val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const char* sha_val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    int w = sqlite3_column_int(stmt, 3);
                    int h = sqlite3_column_int(stmt, 4);
                    const char* dt_val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                    img_list << "{\"id\":" << q(id_val ? id_val : "")
                             << ",\"filename\":" << q(fn_val ? fn_val : "")
                             << ",\"sha256\":" << q(sha_val ? sha_val : "")
                             << ",\"width\":" << w
                             << ",\"height\":" << h
                             << ",\"created_at\":" << q(dt_val ? dt_val : "") << "}";
                }
                sqlite3_finalize(stmt);
            }
        }
        img_list << "]}";
        ss << "{\"type\":\"text\",\"text\":" << q(img_list.str()) << "}";
    } else if (name == "morfo_get_analysis") {
        const std::string analysis_id = extract_field(args_json, "analysis_id");
        std::ostringstream res;
        res << "{\"analysis_id\":" << q(analysis_id);
        if (db_ && !analysis_id.empty()) {
            const char* sql = "SELECT image_id, profile, version, candidate_pixels, components, border_abstentions, status FROM morfo_profile_runs WHERE id=? AND user_id=? AND deleted_at IS NULL";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, analysis_id.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* img_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    const char* prof = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const char* ver = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    int cand_px = sqlite3_column_int(stmt, 3);
                    int comp = sqlite3_column_int(stmt, 4);
                    int abst = sqlite3_column_int(stmt, 5);
                    const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));

                    res << ",\"found\":true"
                        << ",\"image_id\":" << q(img_id ? img_id : "")
                        << ",\"profile\":" << q(prof ? prof : "")
                        << ",\"version\":" << q(ver ? ver : "")
                        << ",\"candidate_pixels\":" << cand_px
                        << ",\"components\":" << comp
                        << ",\"border_abstentions\":" << abst
                        << ",\"status\":" << q(st ? st : "")
                        << ",\"classification\":\"ANOMALY_CANDIDATE\""
                        << ",\"biological_identity\":\"UNKNOWN\"";
                } else {
                    res << ",\"found\":false,\"error\":\"Analysis ID not found\"";
                }
                sqlite3_finalize(stmt);
            } else {
                res << ",\"found\":false,\"error\":\"Database query failure\"";
            }
        } else {
            res << ",\"found\":false,\"error\":\"Database unavailable or analysis_id empty\"";
        }
        res << "}";
        ss << "{\"type\":\"text\",\"text\":" << q(res.str()) << "}";
    } else {
        return make_error_response(id, -32601, "Tool not found: " + name);
    }

    ss << "]}}";
    return ss.str();
}

std::string McpDispatcher::handle_resources_list(std::string_view id) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"result\":{\"resources\":["
       << "{"
       << "\"uri\":\"morfo://profiles/catalog\","
       << "\"name\":\"Catálogo de Perfis Perceptivos MORFO\","
       << "\"mimeType\":\"application/json\""
       << "},"
       << "{"
       << "\"uri\":\"morfo://system/status\","
       << "\"name\":\"Estado Operacional do Núcleo MORFO\","
       << "\"mimeType\":\"application/json\""
       << "}"
       << "]}}";
    return ss.str();
}

std::string McpDispatcher::handle_resources_read(std::string_view id, std::string_view params_json) {
    const std::string uri = extract_field(params_json, "uri");
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"result\":{\"contents\":[";

    if (uri == "morfo://profiles/catalog") {
        ss << "{\"uri\":\"morfo://profiles/catalog\",\"mimeType\":\"application/json\",\"text\":"
           << q(ProfileService::catalog_json()) << "}";
    } else if (uri == "morfo://system/status") {
        std::string status = "{\"system\":\"MORFO\",\"version\":\"0.2.0\",\"standard\":\"C++26\",\"rule\":\"Sempre pronto. Sempre incompleto.\"}";
        ss << "{\"uri\":\"morfo://system/status\",\"mimeType\":\"application/json\",\"text\":"
           << q(status) << "}";
    } else {
        return make_error_response(id, -32602, "Resource not found: " + uri);
    }

    ss << "]}}";
    return ss.str();
}

std::string McpDispatcher::handle_prompts_list(std::string_view id) {
    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"result\":{\"prompts\":["
       << "{"
       << "\"name\":\"morfo_audit_divergence\","
       << "\"description\":\"Auditoria epistêmica de divergências entre métodos de segmentação radicular.\","
       << "\"arguments\":[{\"name\":\"image_id\",\"required\":true},{\"name\":\"profile_a\",\"required\":true},{\"name\":\"profile_b\",\"required\":true}]"
       << "}"
       << "]}}";
    return ss.str();
}

std::string McpDispatcher::handle_prompts_get(std::string_view id, std::string_view params_json) {
    const std::string name = extract_field(params_json, "name");
    if (name != "morfo_audit_divergence") {
        return make_error_response(id, -32601, "Prompt not found: " + name);
    }

    std::ostringstream ss;
    ss << "{\"jsonrpc\":\"2.0\",\"id\":" << (id.empty() ? "null" : (id.front() == '"' ? std::string(id) : q(id)))
       << ",\"result\":{"
       << "\"description\":\"Instruções de auditoria epistêmica para assistentes de IA.\","
       << "\"messages\":[{"
       << "\"role\":\"user\","
       << "\"content\":{\"type\":\"text\",\"text\":\"Analise os resultados das análises da imagem selecionada. Examine as métricas de candidate_pixels, components e abstenções de borda. Identifique as principais divergências sem atribuir confirmação biológica automática, reportando apenas evidências computacionais observáveis.\""
       << "}}]"
       << "}}";
    return ss.str();
}

std::string McpDispatcher::dispatch(std::string_view json_request, const std::string& user_id) {
    std::string method = extract_field(json_request, "method");
    std::string id = extract_field(json_request, "id");
    if (id.empty()) id = "null";

    std::string jsonrpc = extract_field(json_request, "jsonrpc");
    if (jsonrpc != "2.0") {
        return make_error_response(id, -32600, "Invalid JSON-RPC version, expected '2.0'");
    }

    if (method == "initialize") {
        return handle_initialize(id);
    }
    if (method == "ping") {
        return handle_ping(id);
    }
    if (method == "tools/list") {
        return handle_tools_list(id);
    }
    if (method == "tools/call") {
        std::string params = extract_object(json_request, "params");
        return handle_tools_call(id, params, user_id);
    }
    if (method == "resources/list") {
        return handle_resources_list(id);
    }
    if (method == "resources/read") {
        std::string params = extract_object(json_request, "params");
        return handle_resources_read(id, params);
    }
    if (method == "prompts/list") {
        return handle_prompts_list(id);
    }
    if (method == "prompts/get") {
        std::string params = extract_object(json_request, "params");
        return handle_prompts_get(id, params);
    }

    return make_error_response(id, -32601, "Method not found: " + method);
}

} // namespace morfo::service
