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

constexpr int latest_version=11;
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
constexpr std::string_view auth_smtp_migration_name="auth-smtp-hardened-security";
constexpr std::string_view auth_smtp_migration_checksum="e30687ca81045af756e2dd475d88f3bcd90b276cdf8112da6f8aa3b3a46ff2d9";
constexpr std::string_view evaluate_migration_name="morfo-dev-002-scientific-evaluation-foundation";
constexpr std::string_view evaluate_migration_checksum="67d8e20b785ae5198a23a19d45c697cda78ff799e15c83bf259b63d29f476c85";
constexpr std::string_view evaluation_link_migration_name="link-profile-runs-to-draft-evaluation-datasets";
constexpr std::string_view evaluation_link_migration_checksum="357bcb7bd83d17bb490708fd05a2c68f199538e879c05ef684601349634910df";
constexpr std::string_view evaluation_reference_migration_name="manage-draft-units-and-independent-references";
constexpr std::string_view evaluation_reference_migration_checksum="c832115104711345f823960adf0215720e269892332806e8209fd4313143f14d";
constexpr std::string_view pre_history_migration_name="persist-pre001-perception-history";
constexpr std::string_view pre_history_migration_checksum="d231ad9052ccb2889bddc5fd83866408ce86387e1f2e2962e5ad8581ed02c499";
constexpr std::string_view pre_review_migration_name="version-pre001-morphotype-reviews";
constexpr std::string_view pre_review_migration_checksum="9ae837e6576dca1af2d73feec133003439cb47d33779dcb407948425c310e16f";
constexpr std::string_view evaluation_split_update_migration_name="prevent-evaluation-split-leakage-on-update";
constexpr std::string_view evaluation_split_update_migration_checksum="e43dec98781fea7923af3b57020297bff0b88102f681e85ece601ad62cf62659";


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

void apply_migration_5(sqlite3* db){
 execute(db,R"SQL(
CREATE TABLE IF NOT EXISTS auth_audit_events(
 id TEXT PRIMARY KEY,
 email TEXT NOT NULL,
 ip_address TEXT NOT NULL,
 action TEXT NOT NULL,
 decision TEXT NOT NULL,
 details TEXT NOT NULL,
 occurred_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_auth_audit_email ON auth_audit_events(email, occurred_at);
)SQL");
 auto user_cols=columns(db,"users");
 if(!user_cols.contains("role")){
  execute(db,"ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'user' CHECK(role IN ('admin','researcher','user'));");
 }
 if(!user_cols.contains("is_active")){
  execute(db,"ALTER TABLE users ADD COLUMN is_active INTEGER NOT NULL DEFAULT 1;");
 }
 auto sess_cols=columns(db,"sessions");
 if(!sess_cols.contains("token_hash")){
  execute(db,"ALTER TABLE sessions ADD COLUMN token_hash TEXT NOT NULL DEFAULT '';");
 }
 if(!sess_cols.contains("last_seen_at")){
  execute(db,"ALTER TABLE sessions ADD COLUMN last_seen_at TEXT NOT NULL DEFAULT '';");
 }
 auto code_cols=columns(db,"auth_codes");
 if(!code_cols.contains("code_hmac")){
  execute(db,"ALTER TABLE auth_codes ADD COLUMN code_hmac TEXT NOT NULL DEFAULT '';");
 }
 if(!code_cols.contains("attempts")){
  execute(db,"ALTER TABLE auth_codes ADD COLUMN attempts INTEGER NOT NULL DEFAULT 0;");
 }
 if(!code_cols.contains("max_attempts")){
  execute(db,"ALTER TABLE auth_codes ADD COLUMN max_attempts INTEGER NOT NULL DEFAULT 5;");
 }
 execute(db,"CREATE INDEX IF NOT EXISTS idx_auth_codes_lookup ON auth_codes(email, expires_at, used_at);");
}

void apply_migration_6(sqlite3* db){
 execute(db,R"SQL(
CREATE TABLE evaluation_protocols(
 id TEXT PRIMARY KEY, owner_user_id TEXT NOT NULL REFERENCES users(id),
 title TEXT NOT NULL, hypothesis TEXT NOT NULL, task TEXT NOT NULL
  CHECK(task IN('CLASSIFICATION','SEGMENTATION','CLUSTERING','MEASUREMENT','ROBUSTNESS','REPRODUCIBILITY')),
 experimental_unit TEXT NOT NULL, split_strategy TEXT NOT NULL,
 acceptance_criteria_json TEXT NOT NULL, limitations TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN('DRAFT','FROZEN')),
 protocol_version INTEGER NOT NULL CHECK(protocol_version>0), created_at TEXT NOT NULL,
 UNIQUE(title,protocol_version)
);
CREATE TABLE evaluation_datasets(
 id TEXT PRIMARY KEY, protocol_id TEXT NOT NULL REFERENCES evaluation_protocols(id),
 name TEXT NOT NULL, content_sha256 TEXT NOT NULL, reference_kind TEXT NOT NULL
  CHECK(reference_kind IN('NONE','SPECIALIST','EXPERIMENTAL')),
 frozen_at TEXT NOT NULL, created_at TEXT NOT NULL,
 UNIQUE(protocol_id,name,content_sha256)
);
CREATE TABLE evaluation_units(
 id TEXT PRIMARY KEY, dataset_id TEXT NOT NULL REFERENCES evaluation_datasets(id),
 external_key TEXT NOT NULL, split TEXT NOT NULL CHECK(split IN('DEVELOPMENT','TEST')),
 group_key TEXT NOT NULL, input_sha256 TEXT NOT NULL, metadata_json TEXT NOT NULL,
 UNIQUE(dataset_id,external_key), UNIQUE(dataset_id,input_sha256)
);
CREATE TABLE evaluation_runs(
 id TEXT PRIMARY KEY, protocol_id TEXT NOT NULL REFERENCES evaluation_protocols(id),
 dataset_id TEXT NOT NULL REFERENCES evaluation_datasets(id),
 requested_by_user_id TEXT NOT NULL REFERENCES users(id), algorithm TEXT NOT NULL,
 algorithm_version TEXT NOT NULL, source_revision TEXT NOT NULL,
 parameters_json TEXT NOT NULL, environment_json TEXT NOT NULL, random_seed INTEGER NOT NULL,
 status TEXT NOT NULL CHECK(status IN('RUNNING','COMPLETED','FAILED')),
 started_at TEXT NOT NULL, completed_at TEXT, error TEXT
);
CREATE TABLE evaluation_observations(
 id TEXT PRIMARY KEY, run_id TEXT NOT NULL REFERENCES evaluation_runs(id),
 unit_id TEXT NOT NULL REFERENCES evaluation_units(id), metric TEXT NOT NULL,
 value REAL NOT NULL, reference_sha256 TEXT, detail_json TEXT NOT NULL,
 created_at TEXT NOT NULL, UNIQUE(run_id,unit_id,metric)
);
CREATE TABLE evaluation_summaries(
 id TEXT PRIMARY KEY, run_id TEXT NOT NULL REFERENCES evaluation_runs(id), metric TEXT NOT NULL,
 estimate REAL NOT NULL, ci_lower REAL, ci_upper REAL, sample_count INTEGER NOT NULL CHECK(sample_count>0),
 method TEXT NOT NULL, created_at TEXT NOT NULL, UNIQUE(run_id,metric)
);
CREATE INDEX idx_evaluation_runs_protocol ON evaluation_runs(protocol_id,started_at DESC);
CREATE INDEX idx_evaluation_units_dataset_split ON evaluation_units(dataset_id,split,group_key);
CREATE INDEX idx_evaluation_observations_run ON evaluation_observations(run_id,metric);
CREATE TRIGGER evaluation_protocols_no_delete BEFORE DELETE ON evaluation_protocols BEGIN SELECT RAISE(ABORT,'evaluation protocols are retained'); END;
CREATE TRIGGER evaluation_datasets_immutable_update BEFORE UPDATE ON evaluation_datasets BEGIN SELECT RAISE(ABORT,'evaluation datasets are immutable'); END;
CREATE TRIGGER evaluation_datasets_immutable_delete BEFORE DELETE ON evaluation_datasets BEGIN SELECT RAISE(ABORT,'evaluation datasets are immutable'); END;
CREATE TRIGGER evaluation_units_immutable_update BEFORE UPDATE ON evaluation_units BEGIN SELECT RAISE(ABORT,'evaluation units are immutable'); END;
CREATE TRIGGER evaluation_units_immutable_delete BEFORE DELETE ON evaluation_units BEGIN SELECT RAISE(ABORT,'evaluation units are immutable'); END;
CREATE TRIGGER evaluation_observations_immutable_update BEFORE UPDATE ON evaluation_observations BEGIN SELECT RAISE(ABORT,'evaluation observations are immutable'); END;
CREATE TRIGGER evaluation_observations_immutable_delete BEFORE DELETE ON evaluation_observations BEGIN SELECT RAISE(ABORT,'evaluation observations are immutable'); END;
CREATE TRIGGER evaluation_summaries_immutable_update BEFORE UPDATE ON evaluation_summaries BEGIN SELECT RAISE(ABORT,'evaluation summaries are immutable'); END;
CREATE TRIGGER evaluation_summaries_immutable_delete BEFORE DELETE ON evaluation_summaries BEGIN SELECT RAISE(ABORT,'evaluation summaries are immutable'); END;
CREATE TRIGGER evaluation_no_group_leakage BEFORE INSERT ON evaluation_units
WHEN EXISTS(SELECT 1 FROM evaluation_units WHERE dataset_id=NEW.dataset_id AND group_key=NEW.group_key AND split!=NEW.split)
BEGIN SELECT RAISE(ABORT,'experimental group cannot cross development and test splits'); END;
CREATE TRIGGER evaluation_accuracy_requires_reference BEFORE INSERT ON evaluation_observations
WHEN NEW.metric IN('accuracy','precision','recall','f1','dice','iou','ari')
 AND (NEW.reference_sha256 IS NULL OR NEW.reference_sha256='')
BEGIN SELECT RAISE(ABORT,'reference-based metric requires independent reference'); END;
)SQL");
}

void apply_migration_7(sqlite3* db){
 execute(db,R"SQL(
DROP TRIGGER evaluation_datasets_immutable_update;
ALTER TABLE evaluation_datasets ADD COLUMN state TEXT NOT NULL DEFAULT 'FROZEN' CHECK(state IN('DRAFT','FROZEN'));
ALTER TABLE evaluation_units ADD COLUMN image_id TEXT REFERENCES images(id);
ALTER TABLE evaluation_units ADD COLUMN profile_run_id TEXT REFERENCES morfo_profile_runs(id);
CREATE UNIQUE INDEX idx_evaluation_unit_profile_run ON evaluation_units(dataset_id,profile_run_id) WHERE profile_run_id IS NOT NULL;
CREATE TRIGGER evaluation_datasets_frozen_update BEFORE UPDATE ON evaluation_datasets WHEN OLD.state='FROZEN' BEGIN SELECT RAISE(ABORT,'frozen evaluation datasets are immutable'); END;
)SQL");
}

void apply_migration_8(sqlite3* db){
 execute(db,R"SQL(
DROP TRIGGER evaluation_units_immutable_update;
DROP TRIGGER evaluation_units_immutable_delete;
ALTER TABLE evaluation_units ADD COLUMN reference_sha256 TEXT;
ALTER TABLE evaluation_units ADD COLUMN reference_relative_path TEXT;
ALTER TABLE evaluation_units ADD COLUMN reference_filename TEXT;
ALTER TABLE evaluation_units ADD COLUMN reference_created_by_user_id TEXT REFERENCES users(id);
ALTER TABLE evaluation_units ADD COLUMN reference_created_at TEXT;
CREATE TRIGGER evaluation_units_frozen_update BEFORE UPDATE ON evaluation_units
 WHEN (SELECT state FROM evaluation_datasets WHERE id=OLD.dataset_id)='FROZEN'
 BEGIN SELECT RAISE(ABORT,'frozen evaluation units are immutable'); END;
CREATE TRIGGER evaluation_units_frozen_delete BEFORE DELETE ON evaluation_units
 WHEN (SELECT state FROM evaluation_datasets WHERE id=OLD.dataset_id)='FROZEN'
 BEGIN SELECT RAISE(ABORT,'frozen evaluation units are immutable'); END;
)SQL");
}

void apply_migration_9(sqlite3* db){
 execute(db,R"SQL(
CREATE TABLE morfo_pre_runs(
 id TEXT PRIMARY KEY,image_id TEXT NOT NULL REFERENCES images(id),user_id TEXT NOT NULL REFERENCES users(id),
 version TEXT NOT NULL,original_sha256 TEXT NOT NULL,configuration_json TEXT NOT NULL,
 report_relative_path TEXT NOT NULL,map_relative_path TEXT NOT NULL,map_sha256 TEXT NOT NULL,
 morphotype_count INTEGER NOT NULL,cell_count INTEGER NOT NULL,status TEXT NOT NULL CHECK(status IN('COMPLETED','FAILED')),
 created_at TEXT NOT NULL,deleted_at TEXT
);
CREATE TABLE morfo_pre_cells(
 id TEXT PRIMARY KEY,run_id TEXT NOT NULL REFERENCES morfo_pre_runs(id),ordinal INTEGER NOT NULL,
 x INTEGER NOT NULL,y INTEGER NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL,morphotype TEXT NOT NULL,
 distance REAL NOT NULL,novelty REAL NOT NULL,descriptor_json TEXT NOT NULL,UNIQUE(run_id,ordinal)
);
CREATE INDEX idx_morfo_pre_runs_image ON morfo_pre_runs(image_id,user_id,created_at DESC);
CREATE TRIGGER morfo_pre_runs_no_update BEFORE UPDATE ON morfo_pre_runs
 WHEN OLD.deleted_at IS NOT NULL OR NEW.deleted_at IS NULL OR OLD.deleted_at IS NOT NULL
 BEGIN SELECT RAISE(ABORT,'PRE runs are immutable except soft deletion'); END;
CREATE TRIGGER morfo_pre_runs_no_delete BEFORE DELETE ON morfo_pre_runs BEGIN SELECT RAISE(ABORT,'PRE runs are retained'); END;
CREATE TRIGGER morfo_pre_cells_no_update BEFORE UPDATE ON morfo_pre_cells BEGIN SELECT RAISE(ABORT,'PRE cells are immutable'); END;
CREATE TRIGGER morfo_pre_cells_no_delete BEFORE DELETE ON morfo_pre_cells BEGIN SELECT RAISE(ABORT,'PRE cells are retained'); END;
)SQL");
}
void apply_migration_10(sqlite3* db){execute(db,R"SQL(
CREATE TABLE morfo_pre_revisions(
 id TEXT PRIMARY KEY,run_id TEXT NOT NULL REFERENCES morfo_pre_runs(id),user_id TEXT NOT NULL REFERENCES users(id),
 previous_revision_id TEXT REFERENCES morfo_pre_revisions(id),ordinal INTEGER NOT NULL,
 action TEXT NOT NULL CHECK(action IN('ACCEPT','REJECT','MERGE','SPLIT')),
 source_morphotype TEXT NOT NULL,target_morphotype TEXT,result_morphotype TEXT,
 novelty_threshold REAL,notes TEXT NOT NULL,payload_json TEXT NOT NULL,created_at TEXT NOT NULL,
 UNIQUE(run_id,ordinal)
);
CREATE INDEX idx_morfo_pre_revisions_run ON morfo_pre_revisions(run_id,ordinal);
CREATE TRIGGER morfo_pre_revisions_no_update BEFORE UPDATE ON morfo_pre_revisions BEGIN SELECT RAISE(ABORT,'PRE revisions are immutable'); END;
CREATE TRIGGER morfo_pre_revisions_no_delete BEFORE DELETE ON morfo_pre_revisions BEGIN SELECT RAISE(ABORT,'PRE revisions are retained'); END;
)SQL");}

void apply_migration_11(sqlite3* db){
 Statement leakage(db,R"SQL(
SELECT 1 FROM evaluation_units a
JOIN evaluation_datasets da ON da.id=a.dataset_id
JOIN evaluation_units b ON b.group_key=a.group_key AND b.split!=a.split
JOIN evaluation_datasets db ON db.id=b.dataset_id AND db.protocol_id=da.protocol_id
LIMIT 1
)SQL");
 if(sqlite3_step(leakage.get())==SQLITE_ROW)
  throw std::runtime_error("Existing evaluation data crosses development and test splits");
 execute(db,R"SQL(
DROP TRIGGER evaluation_no_group_leakage;
CREATE TRIGGER evaluation_no_group_leakage BEFORE INSERT ON evaluation_units
WHEN EXISTS(
 SELECT 1 FROM evaluation_units u
 JOIN evaluation_datasets existing_dataset ON existing_dataset.id=u.dataset_id
 JOIN evaluation_datasets new_dataset ON new_dataset.id=NEW.dataset_id
 WHERE existing_dataset.protocol_id=new_dataset.protocol_id
  AND u.group_key=NEW.group_key AND u.split!=NEW.split
)
BEGIN SELECT RAISE(ABORT,'experimental group cannot cross development and test splits'); END;
CREATE TRIGGER evaluation_no_group_leakage_update BEFORE UPDATE OF dataset_id,group_key,split ON evaluation_units
WHEN EXISTS(
 SELECT 1 FROM evaluation_units u
 JOIN evaluation_datasets existing_dataset ON existing_dataset.id=u.dataset_id
 JOIN evaluation_datasets new_dataset ON new_dataset.id=NEW.dataset_id
 WHERE existing_dataset.protocol_id=new_dataset.protocol_id
  AND u.group_key=NEW.group_key AND u.split!=NEW.split AND u.id!=OLD.id
)
BEGIN SELECT RAISE(ABORT,'experimental group cannot cross development and test splits'); END;
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
  bool applied=false,analysis_applied=false,learning_applied=false,crud_applied=false,auth_smtp_applied=false,evaluate_applied=false,evaluation_link_applied=false,evaluation_reference_applied=false,pre_history_applied=false,pre_review_applied=false,evaluation_split_update_applied=false;
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
   if(version==5){
    auth_smtp_applied=true;
    if(known.text(1)!=auth_smtp_migration_name||known.text(2)!=auth_smtp_migration_checksum)
     throw std::runtime_error("Migration 5 checksum or name does not match the application");
   }
   if(version==6){
    evaluate_applied=true;
    if(known.text(1)!=evaluate_migration_name||known.text(2)!=evaluate_migration_checksum)
     throw std::runtime_error("Migration 6 checksum or name does not match the application");
   }
   if(version==7){evaluation_link_applied=true;if(known.text(1)!=evaluation_link_migration_name||known.text(2)!=evaluation_link_migration_checksum)throw std::runtime_error("Migration 7 checksum or name does not match the application");}
   if(version==8){evaluation_reference_applied=true;if(known.text(1)!=evaluation_reference_migration_name||known.text(2)!=evaluation_reference_migration_checksum)throw std::runtime_error("Migration 8 checksum or name does not match the application");}
   if(version==9){pre_history_applied=true;if(known.text(1)!=pre_history_migration_name||known.text(2)!=pre_history_migration_checksum)throw std::runtime_error("Migration 9 checksum or name does not match the application");}
   if(version==10){pre_review_applied=true;if(known.text(1)!=pre_review_migration_name||known.text(2)!=pre_review_migration_checksum)throw std::runtime_error("Migration 10 checksum or name does not match the application");}
   if(version==11){evaluation_split_update_applied=true;if(known.text(1)!=evaluation_split_update_migration_name||known.text(2)!=evaluation_split_update_migration_checksum)throw std::runtime_error("Migration 11 checksum or name does not match the application");}
  }
  if(analysis_applied&&!applied)throw std::runtime_error("Migration history is not contiguous");
  if(learning_applied&&!analysis_applied)throw std::runtime_error("Migration history is not contiguous");
  if(crud_applied&&!learning_applied)throw std::runtime_error("Migration history is not contiguous");
  if(auth_smtp_applied&&!crud_applied)throw std::runtime_error("Migration history is not contiguous");
  if(evaluate_applied&&!auth_smtp_applied)throw std::runtime_error("Migration history is not contiguous");
  if(evaluation_link_applied&&!evaluate_applied)throw std::runtime_error("Migration history is not contiguous");
  if(evaluation_reference_applied&&!evaluation_link_applied)throw std::runtime_error("Migration history is not contiguous");
  if(pre_history_applied&&!evaluation_reference_applied)throw std::runtime_error("Migration history is not contiguous");
  if(pre_review_applied&&!pre_history_applied)throw std::runtime_error("Migration history is not contiguous");
  if(evaluation_split_update_applied&&!pre_review_applied)throw std::runtime_error("Migration history is not contiguous");
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
  if(!auth_smtp_applied){
   apply_migration_5(db);
   Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(5,?,?,?)");
   record.bind(1,std::string(auth_smtp_migration_name));record.bind(2,std::string(auth_smtp_migration_checksum));record.bind(3,utc_now());
   if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 5"));
  }
  if(!evaluate_applied){
   apply_migration_6(db);
   Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(6,?,?,?)");
   record.bind(1,std::string(evaluate_migration_name));record.bind(2,std::string(evaluate_migration_checksum));record.bind(3,utc_now());
   if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 6"));
  }
  if(!evaluation_link_applied){apply_migration_7(db);Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(7,?,?,?)");record.bind(1,std::string(evaluation_link_migration_name));record.bind(2,std::string(evaluation_link_migration_checksum));record.bind(3,utc_now());if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 7"));}
  if(!evaluation_reference_applied){apply_migration_8(db);Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(8,?,?,?)");record.bind(1,std::string(evaluation_reference_migration_name));record.bind(2,std::string(evaluation_reference_migration_checksum));record.bind(3,utc_now());if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 8"));}
  if(!pre_history_applied){apply_migration_9(db);Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(9,?,?,?)");record.bind(1,std::string(pre_history_migration_name));record.bind(2,std::string(pre_history_migration_checksum));record.bind(3,utc_now());if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 9"));}
  if(!pre_review_applied){apply_migration_10(db);Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(10,?,?,?)");record.bind(1,std::string(pre_review_migration_name));record.bind(2,std::string(pre_review_migration_checksum));record.bind(3,utc_now());if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 10"));}
  if(!evaluation_split_update_applied){apply_migration_11(db);Statement record(db,"INSERT INTO schema_migrations(version,name,checksum,applied_at) VALUES(11,?,?,?)");record.bind(1,std::string(evaluation_split_update_migration_name));record.bind(2,std::string(evaluation_split_update_migration_checksum));record.bind(3,utc_now());if(sqlite3_step(record.get())!=SQLITE_DONE)throw std::runtime_error(sqlite_error(db,"Cannot record migration 11"));}
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
