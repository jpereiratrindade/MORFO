#pragma once

#include <filesystem>
#include <sqlite3.h>
#include <string>

namespace rizoma::database {

class Connection {
public:
 explicit Connection(const std::filesystem::path& file);
 ~Connection();
 Connection(const Connection&) = delete;
 Connection& operator=(const Connection&) = delete;
 sqlite3* get() const { return p; }
 sqlite3* p{}; // Compatibility handle while the server remains minimally refactored.
};

class Statement {
public:
 Statement(sqlite3* db,const char* sql);
 ~Statement();
 Statement(const Statement&) = delete;
 Statement& operator=(const Statement&) = delete;
 void bind(int index,const std::string& value);
 std::string text(int column) const;
 sqlite3_stmt* get() const { return p; }
 sqlite3_stmt* p{}; // Compatibility handle while the server remains minimally refactored.
};

void execute(sqlite3* db,const char* sql);
void migrate(const std::filesystem::path& file);

} // namespace rizoma::database
