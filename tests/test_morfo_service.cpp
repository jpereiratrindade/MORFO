#include "morfo_service.hpp"
#include <cassert>
#include <iostream>
#include <sqlite3.h>
#include <string>

int main() {
    using namespace morfo::service;

    std::cout << "[TEST] ProfileService catalog and validation..." << std::endl;
    {
        std::string cat = ProfileService::catalog_json();
        assert(cat.find("roots-exp003") != std::string::npos);
        assert(cat.find("discovery") != std::string::npos);
        assert(ProfileService::is_valid_profile("roots-exp003"));
        assert(ProfileService::is_valid_profile("discovery"));
        assert(!ProfileService::is_valid_profile("non-existent-profile"));
        assert(ProfileService::get_profile_semantics("roots-exp003") == "ANOMALY_CANDIDATE");
    }

    std::cout << "[TEST] McpDispatcher JSON-RPC initialize & ping..." << std::endl;
    {
        McpDispatcher dispatcher(nullptr);
        std::string init_req = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
        std::string init_res = dispatcher.dispatch(init_req);
        assert(init_res.find("\"name\":\"morfo-mcp-server\"") != std::string::npos);
        assert(init_res.find("\"version\":\"0.2.0\"") != std::string::npos);

        std::string ping_req = R"({"jsonrpc":"2.0","id":"req-2","method":"ping"})";
        std::string ping_res = dispatcher.dispatch(ping_req);
        assert(ping_res.find("\"result\":{}") != std::string::npos);
    }

    std::cout << "[TEST] McpDispatcher tools/list and tools/call..." << std::endl;
    {
        McpDispatcher dispatcher(nullptr);
        std::string list_req = R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})";
        std::string list_res = dispatcher.dispatch(list_req);
        assert(list_res.find("morfo_list_profiles") != std::string::npos);
        assert(list_res.find("morfo_list_images") != std::string::npos);
        assert(list_res.find("morfo_get_analysis") != std::string::npos);

        std::string call_req = R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"morfo_list_profiles","arguments":{}}})";
        std::string denied_res = dispatcher.dispatch(call_req);
        assert(denied_res.find("Authentication required") != std::string::npos);
        std::string call_res = dispatcher.dispatch(call_req, "user-1");
        assert(call_res.find("roots-exp003") != std::string::npos);
        assert(call_res.find("discovery") != std::string::npos);
    }

    std::cout << "[TEST] McpDispatcher resources..." << std::endl;
    {
        McpDispatcher dispatcher(nullptr);
        std::string r_list_req = R"({"jsonrpc":"2.0","id":5,"method":"resources/list"})";
        std::string r_list_res = dispatcher.dispatch(r_list_req);
        assert(r_list_res.find("morfo://profiles/catalog") != std::string::npos);

        std::string r_read_req = R"({"jsonrpc":"2.0","id":6,"method":"resources/read","params":{"uri":"morfo://profiles/catalog"}})";
        std::string r_read_res = dispatcher.dispatch(r_read_req);
        assert(r_read_res.find("roots-exp003") != std::string::npos);
    }

    std::cout << "[TEST] McpDispatcher prompts..." << std::endl;
    {
        McpDispatcher dispatcher(nullptr);
        std::string p_list_req = R"({"jsonrpc":"2.0","id":7,"method":"prompts/list"})";
        std::string p_list_res = dispatcher.dispatch(p_list_req);
        assert(p_list_res.find("morfo_audit_divergence") != std::string::npos);

        std::string p_get_req = R"({"jsonrpc":"2.0","id":8,"method":"prompts/get","params":{"name":"morfo_audit_divergence"}})";
        std::string p_get_res = dispatcher.dispatch(p_get_req);
        assert(p_get_res.find("candidate_pixels") != std::string::npos);
    }

    std::cout << "[TEST] McpDispatcher with in-memory SQLite..." << std::endl;
    {
        sqlite3* db = nullptr;
        assert(sqlite3_open(":memory:", &db) == SQLITE_OK);

        const char* schema = R"SQL(
            CREATE TABLE images (
                id TEXT PRIMARY KEY, filename TEXT NOT NULL, sha256 TEXT NOT NULL,
                width INTEGER NOT NULL, height INTEGER NOT NULL, user_id TEXT NOT NULL,
                deleted_at TEXT, created_at TEXT NOT NULL
            );
            CREATE TABLE morfo_profile_runs (
                id TEXT PRIMARY KEY, image_id TEXT NOT NULL, user_id TEXT NOT NULL,
                profile TEXT NOT NULL, version TEXT NOT NULL, candidate_pixels INTEGER NOT NULL,
                components INTEGER NOT NULL, border_abstentions INTEGER NOT NULL,
                status TEXT NOT NULL, deleted_at TEXT, created_at TEXT NOT NULL
            );
            INSERT INTO images VALUES('img-1','test.tif','sha_abc',800,600,'user-1',NULL,'2026-10-22T10:00:00Z');
            INSERT INTO images VALUES('img-2','private.tif','sha_private',640,480,'user-2',NULL,'2026-10-22T10:01:00Z');
            INSERT INTO morfo_profile_runs VALUES('run-1','img-1','user-1','roots-exp003','exp003-0.1.0',3339,30,2,'NOT_VALIDATED',NULL,'2026-10-22T10:05:00Z');
            INSERT INTO morfo_profile_runs VALUES('run-2','img-2','user-2','roots-exp003','exp003-0.1.0',9999,99,9,'NOT_VALIDATED',NULL,'2026-10-22T10:06:00Z');
        )SQL";
        char* err = nullptr;
        assert(sqlite3_exec(db, schema, nullptr, nullptr, &err) == SQLITE_OK);

        McpDispatcher dispatcher(db);

        // Test querying images via MCP tool
        std::string img_req = R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"morfo_list_images","arguments":{}}})";
        std::string img_res = dispatcher.dispatch(img_req, "user-1");
        assert(img_res.find("img-1") != std::string::npos);
        assert(img_res.find("test.tif") != std::string::npos);
        assert(img_res.find("img-2") == std::string::npos);
        assert(img_res.find("private.tif") == std::string::npos);

        // Test querying analysis result via MCP tool
        std::string ana_req = R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"morfo_get_analysis","arguments":{"analysis_id":"run-1"}}})";
        std::string ana_res = dispatcher.dispatch(ana_req, "user-1");
        assert(ana_res.find("candidate_pixels") != std::string::npos);
        assert(ana_res.find("3339") != std::string::npos);
        assert(ana_res.find("ANOMALY_CANDIDATE") != std::string::npos);
        assert(ana_res.find("UNKNOWN") != std::string::npos);

        std::string foreign_req = R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"morfo_get_analysis","arguments":{"analysis_id":"run-2"}}})";
        std::string foreign_res = dispatcher.dispatch(foreign_req, "user-1");
        assert(foreign_res.find("\\\"found\\\":false") != std::string::npos);
        assert(foreign_res.find("9999") == std::string::npos);

        std::string empty_identity_res = dispatcher.dispatch(img_req);
        assert(empty_identity_res.find("Authentication required") != std::string::npos);

        sqlite3_close(db);
    }

    std::cout << "[SUCCESS] All MORFO service and MCP tests passed!" << std::endl;
    return 0;
}
