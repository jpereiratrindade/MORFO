#pragma once
#include <filesystem>
#include <string>
#include <cstdint>
#include <sqlite3.h>

namespace morfo::profiles {
namespace fs=std::filesystem;
struct RootsOutput {
 std::uint64_t width=0,height=0,candidate_pixels=0,components=0,border_abstentions=0;
 std::string mask_sha256,original_sha256;
};
void create_schema(sqlite3* db);
std::string registry_json();
RootsOutput run_exp003(const fs::path& source,const fs::path& out_dir);
}
