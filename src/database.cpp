#include "database.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace rizoma::database {
namespace {

constexpr int latest_version=4;
constexpr std::string_view migration_name="establish-v0.2.0-schema-and-history";
// SHA-256 of the reviewed migration specification. Changing migration 1 requires
// a new migration, never replacement of this checksum.
constexpr std::string_view migration_checksum="98804c3a9e40c1a58b0d063abec68a58dfc3f1e441a88da8bd371e76fec6d53c";
constexpr std::string_view analysis_migration_name="analysis-review-foundation";
constexpr std::string_view analysis_migration_checksum="38fcc3cba031ac0f5228dd3c44d5ca1df8164bf9a185fe0a7d62c89ec34fbf0e";
constexpr std::string_view learning_migration_name="learning-feedback-foundation";
constexpr std::string_view learning_migration_checksum="b4e514bc655bb36bf04a03c7a07dc0e39b0694140ac1c53a606d70a67fcaf6fe";
constexpr std::string_view crud_migration_name="crud-soft-delete-and-lifecycle";
constexpr std::string_view crud_migration_checksum="4a7e2b1c8f39501a9d8e7c6b5a4f3e2d1c0b9a8f7e6d5c4b3a2f1e0d9c8b7a6f";

std::string sqlite_error(sqlite3* db,const std::string& context){
 return context+": "+(db?sqlite3_errmsg(db):"SQLite unavailable");
}

bool table_exists(sqlite3* db,const std::string& table){
 Statement st(db,"SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?");
 st.bind(1,table);
 return sqlite3_step(st.get())==SQLITE_ROW;
}

std::set<std::string> columns(sqlite3* db,const std::string& table){
 std::set<std::string> result;
 const std::string query="PRAGMA table_info("+table+")";
 Statement st(db,query.c_str());
 while(sqlite3_step(st.get())==SQLITE_ROW)result.insert(st.text(1));
 return result;
}

void require_columns(sqlite3* db,const std::string& table,std::initializer_list<const char*> required){
 if(!table_exists(db,table))return;
 const auto present=columns(db,table);
 for(const char* column:required)if(!present.contains(column))
  throw std::runtime_error("Incompatible legacy schema: "+table+"."+column+" is missing");
}

std::string utc_now(){
 const auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
 std::tm value{}; gmtime_r(&now,&value); char buffer[32]{};
 std::strftime(buffer,sizeof(buffer),"%Y-%m-%dT%H:%M:%SZ",&value);
 return buffer;
}

void validate_legacy_shape(sqlite3* db){
 require_columns(db,"images",{"id","filename","sample_id","capture_date","created_at","sha256","bytes","width","height","format","brightness","qc_state","pipeline","original_path","preview_path","grayscale_path"});
 require_columns(db,"events",{"id","image_id","event","at","detail"});
 require_columns(db,"users",{"id","email","name","created_at","last_login_at"});
 require_columns(db,"auth_codes",{"id","email","code","code_hash","expires_at","used_at","created_at"});
 require_columns(db,"sessions",{"token","user_id","email","created_at","expires_at"});
}

void apply_migration_1(sqlite3* db){
 validate_legacy_shape(db);
 execute(db,R"SQL(
CREATE TABLE IF NOT EXISTS images(
 id TEXT PRIMARY KEY, filename TEXT NOT NULL, sample_id TEXT NOT NULL, capture_date TEXT NOT NULL,
 created_at TEXT NOT NULL, sha256 TEXT NOT NULL, bytes INTEGER NOT NULL,
 width INTEGER NOT NULL, height INTEGER NOT NULL, format TEXT NOT NULL,
 brightness REAL NOT NULL, qc_state TEXT NOT NULL, pipeline TEXT NOT NULL,
 original_path TEXT NOT NULL, preview_path TEXT NOT NULL, grayscale_path TEXT NOT NULL,
 user_id TEXT NOT NULL DEFAULT '', user_email TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,image_id TEXT,event TEXT NOT NULL,at TEXT NOT NULL,detail TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS users(
 id TEXT PRIMARY KEY, email TEXT NOT NULL UNIQUE, name TEXT NOT NULL,
 created_at TEXT NOT NULL, last_login_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS auth_codes(
 id TEXT PRIMARY KEY, email TEXT NOT NULL, code TEXT NOT NULL, code_hash TEXT NOT NULL,
 expires_at TEXT NOT NULL, used_at TEXT, created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions(
 token TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id), email TEXT NOT NULL,
 created_at TEXT NOT NULL, expires_at TEXT NOT NULL
);
)SQL");
 auto image_columns=columns(db,"images");
 if(!image_columns.contains("user_email"))execute(db,"ALTER TABLE images ADD COLUMN user_email TEXT NOT NULL DEFAULT ''");
 if(!image_columns.contains("user_id"))execute(db,"ALTER TABLE images ADD COLUMN user_id TEXT NOT NULL DEFAULT ''");
 execute(db,R"SQL(
UPDATE images
 SET user_id=(SELECT users.id FROM users WHERE users.email=images.user_email)
 WHERE user_id='' AND user_email!=''
   AND EXISTS(SELECT 1 FROM users WHERE users.email=images.user_email);
CREATE INDEX IF NOT EXISTS idx_images_created_at ON images(created_at DESC);
CREATE INDEX IF NOT EXISTS idx_images_user ON images(user_id,created_at DESC);
CREATE INDEX IF NOT EXISTS idx_users_email ON users(email);
CREATE INDEX IF NOT EXISTS idx_sessions_token ON sessions(token);
CREATE INDEX IF NOT EXISTS idx_auth_codes_email ON auth_codes(email,expires_at);
)SQL");
}

void apply_migration_2(sqlite3* db){
 execute(db,R"SQL(
CREATE TABLE analysis_runs(
 id TEXT PRIMARY KEY,
 image_id TEXT NOT NULL REFERENCES images(id),
 requested_by_user_id TEXT NOT NULL REFERENCES users(id),
 algorithm TEXT NOT NULL,
 algorithm_version TEXT NOT NULL,
 parameters_json TEXT NOT NULL,
 source_sha256 TEXT NOT NULL,
 status TEXT NOT NULL CHECK(status IN('RUNNING','REVIEW_REQUIRED','FAILED')),
 created_at TEXT NOT NULL,
 completed_at TEXT,
 error TEXT
);
CREATE TABLE analysis_regions(
 id TEXT PRIMARY KEY,
 run_id TEXT NOT NULL REFERENCES analysis_runs(id),
 ordinal INTEGER NOT NULL,
 x INTEGER NOT NULL CHECK(x>=0), y INTEGER NOT NULL CHECK(y>=0),
 width INTEGER NOT NULL CHECK(width>0), height INTEGER NOT NULL CHECK(height>0),
 area INTEGER NOT NULL CHECK(area>0),
 evidence_json TEXT NOT NULL,
 current_version INTEGER NOT NULL DEFAULT 0 CHECK(current_version>=0),
 created_at TEXT NOT NULL,
 UNIQUE(run_id,ordinal)
);
CREATE TABLE analysis_judgments(
 id TEXT PRIMARY KEY,
 region_id TEXT NOT NULL REFERENCES analysis_regions(id),
 actor_type TEXT NOT NULL CHECK(actor_type IN('ALGORITHM','JEV','ENTE')),
 label TEXT NOT NULL CHECK(label IN('ROOT','UNKNOWN')),
 evidence_json TEXT NOT NULL,
 created_at TEXT NOT NULL
);
CREATE TABLE review_events(
 id TEXT PRIMARY KEY,
 region_id TEXT NOT NULL REFERENCES analysis_regions(id),
 reviewer_user_id TEXT NOT NULL REFERENCES users(id),
 version INTEGER NOT NULL CHECK(version>0),
 action TEXT NOT NULL CHECK(action IN('CONFIRM','CORRECT','SUSPEND')),
 label TEXT NOT NULL CHECK(label IN('ROOT','MATRIX','ARTIFACT','UNKNOWN')),
 justification TEXT NOT NULL,
 created_at TEXT NOT NULL,
 UNIQUE(region_id,version)
);
CREATE INDEX idx_analysis_runs_image ON analysis_runs(image_id,created_at DESC);
CREATE INDEX idx_analysis_regions_run ON analysis_regions(run_id,ordinal);
CREATE INDEX idx_analysis_judgments_region ON analysis_judgments(region_id,created_at);
CREATE INDEX idx_review_events_region ON review_events(region_id,version);
CREATE TRIGGER review_events_immutable_update BEFORE UPDATE ON review_events BEGIN
 SELECT RAISE(ABORT,'review_events are append-only');
END;
CREATE TRIGGER review_events_immutable_delete BEFORE DELETE ON review_events BEGIN
 SELECT RAISE(ABORT,'review_events are append-only');
END;
CREATE TRIGGER analysis_judgments_immutable_update BEFORE UPDATE ON analysis_judgments BEGIN
 SELECT RAISE(ABORT,'analysis_judgments are immutable');
END;
CREATE TRIGGER analysis_judgments_immutable_delete BEFORE DELETE ON analysis_judgments BEGIN
 SELECT RAISE(ABORT,'analysis_judgments are immutable');
END;
)SQL");
}

void apply_migration_3(sqlite3* db){
 execute(db,R"SQL(
CREATE TABLE IF NOT EXISTS learning_models(
 id TEXT PRIMARY KEY,
 user_id TEXT NOT NULL REFERENCES users(id),
 model_type TEXT NOT NULL,
 weights_json TEXT NOT NULL,
 sample_count INTEGER NOT NULL,
 updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS learning_exemplars(
 id TEXT PRIMARY KEY,
 user_id TEXT NOT NULL REFERENCES users(id),
 label TEXT NOT NULL,
 descriptor_json TEXT NOT NULL,
 created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_learning_models_user ON learning_models(user_id,model_type);
CREATE INDEX IF NOT EXISTS idx_learning_exemplars_user ON learning_exemplars(user_id,created_at DESC);
)SQL");
}

void apply_migration_4(sqlite3* db){
 auto image_cols=columns(db,"images");
 if(!image_cols.contains("deleted_at"))execute(db,"ALTER TABLE images ADD COLUMN deleted_at TEXT");
 auto run_cols=columns(db,"analysis_runs");
 if(!run_cols.contains("deleted_at"))execute(db,"ALTER TABLE analysis_runs ADD COLUMN deleted_at TEXT");
 if(!run_cols.contains("notes"))execute(db,"ALTER TABLE analysis_runs ADD COLUMN notes TEXT NOT NULL DEFAULT ''");
 execute(db,R"SQL(
  CREATE TABLE IF NOT EXISTS analysis_runs_v4(
   id TEXT PRIMARY KEY,
   image_id TEXT NOT NULL REFERENCES images(id),
   requested_by_user_id TEXT NOT NULL REFERENCES users(id),
   algorithm TEXT NOT NULL,
   algorithm_version TEXT NOT NULL,
   parameters_json TEXT NOT NULL,
   source_sha256 TEXT NOT NULL,
   status TEXT NOT NULL CHECK(status IN('RUNNING','REVIEW_REQUIRED','COMPLETED','ARCHIVED','FAILED')),
   created_at TEXT NOT NULL,
   completed_at TEXT,
   error TEXT,
   notes TEXT NOT NULL DEFAULT '',
   deleted_at TEXT
  );
  INSERT INTO analysis_runs_v4(id,image_id,requested_by_user_id,algorithm,algorithm_version,parameters_json,source_sha256,status,created_at,completed_at,error,notes,deleted_at)
   SELECT id,image_id,requested_by_user_id,algorithm,algorithm_version,parameters_json,source_sha256,status,created_at,completed_at,error,notes,deleted_at FROM analysis_runs;
  DROP TABLE analysis_runs;
  ALTER TABLE analysis_runs_v4 RENAME TO analysis_runs;
  CREATE INDEX IF NOT EXISTS idx_analysis_runs_image ON analysis_runs(image_id,created_at DESC);
  CREATE INDEX IF NOT EXISTS idx_images_deleted ON images(user_id,deleted_at);
  CREATE INDEX IF NOT EXISTS idx_analysis_runs_deleted ON analysis_runs(image_id,deleted_at);
 )SQL");
}

void verify_database(sqlite3* db){
 {
  Statement st(db,"PRAGMA integrity_check");
  if(sqlite3_step(st.get())!=SQLITE_ROW||st.text(0)!="ok")throw std::runtime_error("SQLite integrity_check failed");
 }
 {
  Statement st(db,"PRAGMA foreign_key_check");
  if(sqlite3_step(st.get())==SQLITE_ROW)throw std::runtime_error("SQLite foreign_key_check failed");
 }
 Statement links(db,R"SQL(
SELECT COUNT(*) FROM images i JOIN users u ON u.email=i.user_email
 WHERE i.user_id!='' AND i.user_id!=u.id
)SQL");
 if(sqlite3_step(links.get())!=SQLITE_ROW||sqlite3_column_int64(links.get(),0)!=0)
  throw std::runtime_error("Image/user linkage validation failed");
}

} // namespace

Connection::Connection(const std::filesystem::path& file){
 const int flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX;
 if(sqlite3_open_v2(file.c_str(),&p,flags,nullptr)!=SQLITE_OK){
  const std::string message=sqlite_error(p,"Cannot open database");
  if(p)sqlite3_close(p);
  p=nullptr;
  throw std::runtime_error(message);
 }
 sqlite3_busy_timeout(p,8000);
 char* error=nullptr;
 if(sqlite3_exec(p,"PRAGMA foreign_keys=ON",nullptr,nullptr,&error)!=SQLITE_OK){
  const std::string message=error?error:sqlite_error(p,"Cannot enable foreign keys");
  sqlite3_free(error); sqlite3_close(p); p=nullptr;
  throw std::runtime_error(message);
 }
}
Connection::~Connection(){if(p)sqlite3_close(p);}

Statement::Statement(sqlite3* db,const char* sql){
 if(sqlite3_prepare_v2(db,sql,-1,&p,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite_error(db,"Cannot prepare SQL"));
}
Statement::~Statement(){if(p)sqlite3_finalize(p);}
void Statement::bind(int index,const std::string& value){
 if(sqlite3_bind_text(p,index,value.data(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK)
  throw std::runtime_error("Cannot bind SQL value");
}
std::string Statement::text(int column)const{
 const auto* value=reinterpret_cast<const char*>(sqlite3_column_text(p,column));
 return value?value:"";
}

void execute(sqlite3* db,const char* sql){
 char* error=nullptr;
 if(sqlite3_exec(db,sql,nullptr,nullptr,&error)!=SQLITE_OK){
  const std::string message=error?error:sqlite_error(db,"SQL execution failed");
  sqlite3_free(error); throw std::runtime_error(message);
 }
}

void migrate(const std::filesystem::path& file){
 Connection connection(file); sqlite3* db=connection.get();
 execute(db,"PRAGMA foreign_keys=OFF");
 execute(db,"PRAGMA legacy_alter_table=ON");
 execute(db,"PRAGMA journal_mode=WAL");
 try{
  execute(db,"BEGIN IMMEDIATE");
  execute(db,R"SQL(CREATE TABLE IF NOT EXISTS schema_migrations(
   version INTEGER PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL, applied_at TEXT NOT NULL
  ))SQL");
  Statement known(db,"SELECT version,name,checksum FROM schema_migrations ORDER BY version");
  bool applied=false,analysis_applied=false,learning_applied=false,crud_applied=false;
  while(sqlite3_step(known.get())==SQLITE_ROW){
   const int version=sqlite3_column_int(known.get(),0);
   if(version>latest_version)throw std::runtime_error("Database was migrated by a newer RIZOMA version");
   if(version==1){
    applied=true;
    if(known.text(1)!=migration_name||known.text(2)!=migration_checksum)
     throw std::runtime_error("Migration 1 checksum or name does not match the application");
   }
   if(version==2){
    analysis_applied=true;
    if(known.text(1)!=analysis_migration_name||known.text(2)!=analysis_migration_checksum)
     throw std::runtime_error("Migration 2 checksum or name does not match the application");
   }
   if(version==3){
    learning_applied=true;
    if(known.text(1)!=learning_migration_name||known.text(2)!=learning_migration_checksum)
     throw std::runtime_error("Migration 3 checksum or name does not match the application");
   }
   if(version==4){
    crud_applied=true;
    if(known.text(1)!=crud_migration_name||known.text(2)!=crud_migration_checksum)
     throw std::runtime_error("Migration 4 checksum or name does not match the application");
   }
  }
  if(analysis_applied&&!applied)throw std::runtime_error("Migration history is not contiguous");
  if(learning_applied&&!analysis_applied)throw std::runtime_error("Migration history is not contiguous");
  if(crud_applied&&!learning_applied)throw std::runtime_error("Migration history is not contiguous");
  if(!applied){
   apply_migration_1(db);
   Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(1,?,?,?)");
   record.bind(1,std::string(migration_name)); record.bind(2,std::string(migration_checksum)); record.bind(3,utc_now());
   if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 1"));
  }
  if(!analysis_applied){
   apply_migration_2(db);
   Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(2,?,?,?)");
   record.bind(1,std::string(analysis_migration_name));record.bind(2,std::string(analysis_migration_checksum));record.bind(3,utc_now());
   if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 2"));
  }
  if(!learning_applied){
   apply_migration_3(db);
   Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(3,?,?,?)");
   record.bind(1,std::string(learning_migration_name));record.bind(2,std::string(learning_migration_checksum));record.bind(3,utc_now());
   if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 3"));
  }
  if(!crud_applied){
   apply_migration_4(db);
   Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(4,?,?,?)");
   record.bind(1,std::string(crud_migration_name));record.bind(2,std::string(crud_migration_checksum));record.bind(3,utc_now());
   if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 4"));
  }
  verify_database(db);
  execute(db,"COMMIT");
  execute(db,"PRAGMA legacy_alter_table=OFF");
  execute(db,"PRAGMA foreign_keys=ON");
 }catch(...){
  sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
  execute(db,"PRAGMA legacy_alter_table=OFF");
  execute(db,"PRAGMA foreign_keys=ON");
  throw;
 }
}

} // namespace rizoma::database
