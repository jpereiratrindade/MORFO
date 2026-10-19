#!/usr/bin/env python3
"""Controlled, fail-closed transformation of a RIZOMA worktree copy into MORFO."""
from pathlib import Path
import sys

base=Path(sys.argv[1]).resolve()

def sub_once(content, before, after, description):
    n=content.count(before)
    if n!=1:
        raise RuntimeError(f"Incompatible RIZOMA checkout: {description}, expected 1 anchor, saw {n}; no source modified")
    return content.replace(before,after,1)

main=base/'src/main.cpp'
cmake=base/'CMakeLists.txt'
html=base/'web/index.html'
app=base/'web/app.js'
for item in [main,cmake,html,app,base/'src/database.cpp']:
    if not item.is_file(): raise RuntimeError(f"Missing RIZOMA source file: {item}")
original_main=main.read_text(encoding='utf-8')
content=original_main
content=sub_once(content,'#include "learning.hpp"','#include "learning.hpp"\n#include "morfo_profiles.hpp"','main include')
content=content.replace('rizoma.sqlite3','morfo.sqlite3').replace('rizoma_session','morfo_session')
content=content.replace('RIZOMA_SMTP_', 'MORFO_SMTP_').replace('RIZOMA_HOST','MORFO_HOST').replace('RIZOMA_PORT','MORFO_PORT').replace('RIZOMA_DATA_DIR','MORFO_DATA_DIR')
content=content.replace('RIZOMA/0.2.0','MORFO/0.1.0').replace('RIZOMA -', 'MORFO -').replace('RIZOMA ·','MORFO ·')
content=content.replace('rizoma@embrapa.br','morfo@embrapa.br')
content=content.replace('RIZOMA SisTer','MORFO SisTer')
content=sub_once(content,
 'rizoma::database::migrate(app.root/"morfo.sqlite3");',
 'rizoma::database::migrate(app.root/"morfo.sqlite3");\n Db db(app.root/"morfo.sqlite3");\n morfo::profiles::create_schema(db.p);', 'schema startup')
helpers=r'''
// MORFO profile service. Protected by the existing image ownership/authentication gate.
// Distinct from legacy region suggestions: masks are immutable derivatives of explicit runs.
static std::string morfo_profile_list(const App& app,const std::string& image_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 Stmt owned(db.p,"SELECT 1 FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 owned.bind(1,image_id);owned.bind(2,user_id);
 if(sqlite3_step(owned.p)!=SQLITE_ROW)throw Failure("Image not found");
 Stmt st(db.p,"SELECT id,candidate_pixels,components,border_abstentions,status,created_at FROM morfo_profile_runs WHERE image_id=? AND user_id=? AND profile='roots-exp003' ORDER BY created_at DESC,rowid DESC");
 st.bind(1,image_id);st.bind(2,user_id);
 std::string result="{\"data\":[";bool first=true;
 while(sqlite3_step(st.p)==SQLITE_ROW){if(!first)result+=',';first=false;
  result+="{\"id\":"+q(col(st.p,0))+",\"candidate_pixels\":"+std::to_string(sqlite3_column_int64(st.p,1))+
   ",\"components\":"+std::to_string(sqlite3_column_int64(st.p,2))+
   ",\"border_abstentions\":"+std::to_string(sqlite3_column_int64(st.p,3))+
   ",\"status\":"+q(col(st.p,4))+",\"created_at\":"+q(col(st.p,5))+"}";
 }
 return result+"]}";
}
static std::string morfo_start_profile(const App& app,const std::string& image_id,const std::string& user_id){
 Db db(app.root/"morfo.sqlite3");
 Stmt image(db.p,"SELECT original_path,sha256,width,height FROM images WHERE id=? AND user_id=? AND deleted_at IS NULL");
 image.bind(1,image_id);image.bind(2,user_id);
 if(sqlite3_step(image.p)!=SQLITE_ROW)throw Failure("Image not found");
 const auto path=col(image.p,0),source_sha=col(image.p,1);
 const auto w=sqlite3_column_int64(image.p,2),h=sqlite3_column_int64(image.p,3);
 if(w<=0||h<=0||static_cast<std::uint64_t>(w)>12'000'000ULL/static_cast<std::uint64_t>(h))
  throw Failure("EXP-003 requires <= 12 MP full-resolution image; tiling is not validated");
 const std::string run_id=rand_id(),now=utc_offset(0);
 const fs::path relative=fs::path("derived")/image_id/"profiles"/"roots-exp003"/run_id;
 const fs::path output=app.root/relative;
 try {
  const auto result=morfo::profiles::run_exp003(app.root/path,output);
  if(result.original_sha256!=source_sha)throw Failure("Original hash changed since ingest");
  Stmt insert(db.p,"INSERT INTO morfo_profile_runs(id,image_id,user_id,profile,version,original_sha256,mask_sha256,width,height,candidate_pixels,components,border_abstentions,relative_dir,status,created_at) VALUES(?,?,?,'roots-exp003','exp003-original-0.1.0',?,?,?,?,?,?,?,?,'NOT_VALIDATED',?)");
  insert.bind(1,run_id);insert.bind(2,image_id);insert.bind(3,user_id);
  insert.bind(4,result.original_sha256);insert.bind(5,result.mask_sha256);
  sqlite3_bind_int64(insert.p,6,static_cast<sqlite3_int64>(result.width));
  sqlite3_bind_int64(insert.p,7,static_cast<sqlite3_int64>(result.height));
  sqlite3_bind_int64(insert.p,8,static_cast<sqlite3_int64>(result.candidate_pixels));
  sqlite3_bind_int64(insert.p,9,static_cast<sqlite3_int64>(result.components));
  sqlite3_bind_int64(insert.p,10,static_cast<sqlite3_int64>(result.border_abstentions));
  insert.bind(11,relative.generic_string());insert.bind(12,now);
  if(sqlite3_step(insert.p)!=SQLITE_DONE)throw Failure("Could not persist MORFO profile run");
  return "{\"data\":{\"id\":"+q(run_id)+",\"profile\":\"roots-exp003\",\"status\":\"COMPLETED\",\"scientific_status\":\"NOT_VALIDATED\",\"biological_identity\":\"UNKNOWN\",\"candidate_pixels\":"+std::to_string(result.candidate_pixels)+",\"components\":"+std::to_string(result.components)+",\"border_abstentions\":"+std::to_string(result.border_abstentions)+",\"mask_sha256\":"+q(result.mask_sha256)+"}}";
 }catch(...){std::error_code ignored;fs::remove_all(output,ignored);throw;}
}
static std::string morfo_profile_artifact(const App& app,const std::string& image_id,const std::string& user_id,const std::string& suffix,std::string& mime){
 const auto slash=suffix.find('/');if(slash==std::string::npos)throw Failure("Invalid profile artifact");
 const auto run_id=suffix.substr(0,slash),kind=suffix.substr(slash+1);
 if(!valid_id(run_id)||(kind!="mask"&&kind!="overlay"&&kind!="report"))throw Failure("Invalid profile artifact");
 Db db(app.root/"morfo.sqlite3");
 Stmt st(db.p,"SELECT r.relative_dir FROM morfo_profile_runs r JOIN images i ON i.id=r.image_id WHERE r.id=? AND r.image_id=? AND r.user_id=? AND i.user_id=? AND i.deleted_at IS NULL");
 st.bind(1,run_id);st.bind(2,image_id);st.bind(3,user_id);st.bind(4,user_id);
 if(sqlite3_step(st.p)!=SQLITE_ROW)throw Failure("Profile run not found");
 const fs::path name=(kind=="report"?"report.json":kind=="mask"?"mask.png":"overlay.png");
 mime=kind=="report"?"application/json; charset=utf-8":"image/png";
 return slurp(app.root/col(st.p,0)/name);
}
'''
content=sub_once(content,'static Response handle(',helpers+'\nstatic Response handle(','insert profile helpers')
content=sub_once(content,' // Protected routes', ' if(req.method()==http::verb::get&&p=="/api/v1/profiles")return respond(http::status::ok,v,morfo::profiles::registry_json());\n\n // Protected routes', 'registry route')
anchor='    if(req.method()==http::verb::post&&kind=="analysis")'
block=r'''    if(kind=="profiles/roots-exp003"){
     try{
      if(req.method()==http::verb::post)return respond(http::status::created,v,morfo_start_profile(app,id,user->id));
      if(req.method()==http::verb::get)return respond(http::status::ok,v,morfo_profile_list(app,id,user->id));
     }catch(const std::exception& e){return problem(v,std::string(e.what())=="Image not found"?http::status::not_found:http::status::unprocessable_entity,e.what());}
    }
    if(kind.starts_with("profiles/roots-exp003/")&&req.method()==http::verb::get){
     try{std::string mime;auto bytes=morfo_profile_artifact(app,id,user->id,kind.substr(std::string("profiles/roots-exp003/").size()),mime);return respond(http::status::ok,v,std::move(bytes),mime);}
     catch(const std::exception& e){return problem(v,http::status::not_found,e.what());}
    }
'''
content=sub_once(content,anchor,block+anchor,'protected profile routes')
anchor='{"/app.js",{"app.js","application/javascript; charset=utf-8"}}'
content=sub_once(content,anchor,anchor[:-1]+',{"/morfo-profiles.js",{"morfo-profiles.js","application/javascript; charset=utf-8"}}}', 'frontend asset')
# Unique process identity (no change to legacy upload client contract for this first fork).
content=content.replace('std::string host="127.0.0.1"; unsigned short port=8089','std::string host="127.0.0.1"; unsigned short port=8091')
content=content.replace('app.port=8089','app.port=8091')
content=content.replace('RIZOMA v0.2.0','MORFO v0.1.0')

cm=cmake.read_text(encoding='utf-8')
for a,b in [('project(rizoma VERSION 0.2.0','project(morfo VERSION 0.1.0'),('rizoma-server','morfo-server')]:cm=cm.replace(a,b)
cm=sub_once(cm,
 'add_executable(morfo-server src/main.cpp src/database.cpp src/analysis.cpp src/tiles.cpp src/learning.cpp)',
 '''add_library(morfo_roots_exp003 STATIC
 profiles/roots/exp003/src/core.cpp
 profiles/roots/exp003/src/image_io.cpp
 profiles/roots/exp003/src/comparison.cpp)
target_include_directories(morfo_roots_exp003 PUBLIC ${CMAKE_SOURCE_DIR}/profiles/roots/exp003/include)
target_link_libraries(morfo_roots_exp003 PUBLIC TIFF::TIFF PNG::PNG JPEG::JPEG OpenSSL::Crypto)
target_compile_options(morfo_roots_exp003 PRIVATE -Wall -Wextra -Wpedantic)

add_executable(morfo-server src/main.cpp src/database.cpp src/analysis.cpp src/tiles.cpp src/learning.cpp src/morfo_profiles.cpp)
target_link_libraries(morfo-server PRIVATE morfo_roots_exp003)''', 'Cmake executable')
cm+='''
add_executable(morfo-roots-golden tests/morfo_roots_golden.cpp)
target_link_libraries(morfo-roots-golden PRIVATE morfo_roots_exp003)
if(BUILD_TESTING)
 add_test(NAME morfo.roots.exp003.golden
  COMMAND morfo-roots-golden
    ${CMAKE_SOURCE_DIR}/tests/fixtures/private/monolito_rgb.png
    ${CMAKE_SOURCE_DIR}/tests/fixtures/private/exp003_expected_mask.png)
endif()
'''

h=html.read_text(encoding='utf-8')
h=h.replace('RIZOMA','MORFO').replace('Rizoma','Morfo')
if '</body>' not in h:raise RuntimeError('Missing body anchor')
h=h.replace('</body>','<script src="/morfo-profiles.js" defer></script>\n</body>',1)
# Preserve inherited regression coverage after isolating MORFO's cookie and database.
for test_name in ('smoke.py','migrations.py'):
    test_path=base/'tests'/test_name
    if test_path.exists():
        test_src=test_path.read_text(encoding='utf-8')
        test_src=test_src.replace('rizoma_session','morfo_session').replace('rizoma.sqlite3','morfo.sqlite3')
        test_src=test_src.replace('RIZOMA migrations PASS','MORFO migrations PASS').replace('RIZOMA smoke PASS','MORFO smoke PASS')
        if test_name == 'smoke.py':
            smoke_anchor='            # 7. Multi-user isolation verification (User 2)'
            smoke_block='''            # MORFO thematic profile: backend EXP-003 runs, records, isolates derivatives.
            status, body, _ = req(base+'/api/v1/profiles')
            assert status == 200 and b'roots-exp003' in body and b'PLANNED' in body
            profile_url = base+f'/api/v1/images/{analysis_image_id}/profiles/roots-exp003'
            status, body, _ = req(profile_url, 'POST', b'', auth_headers)
            assert status == 201, (status, body)
            profile = json.loads(body)['data']
            assert profile['scientific_status'] == 'NOT_VALIDATED', profile
            assert profile['biological_identity'] == 'UNKNOWN', profile
            profile_id = profile['id']
            status, data, _ = req(profile_url+'/'+profile_id+'/mask', headers=auth_headers)
            assert status == 200 and data.startswith(b'\\x89PNG'), (status, data[:16])
            status, data, _ = req(profile_url+'/'+profile_id+'/report', headers=auth_headers)
            assert status == 200 and json.loads(data)['classification'] == 'ANOMALY_CANDIDATE'
            status, data, _ = req(profile_url, headers=auth_headers)
            assert status == 200 and profile_id in data.decode(), (status, data)
            status, data, _ = req(base+f'/api/v1/images/{large_id}/profiles/roots-exp003', 'POST', b'', auth_headers)
            assert status == 422 and b'12 MP' in data, (status, data)

'''
            if test_src.count(smoke_anchor) != 1:
                raise RuntimeError('Could not install MORFO smoke profile gate')
            test_src=test_src.replace(smoke_anchor,smoke_block+smoke_anchor,1)
        test_path.write_text(test_src,encoding='utf-8')

a=app.read_text(encoding='utf-8').replace('RIZOMA','MORFO').replace('Rizoma','Morfo')
a+='\n// MORFO bridge is read-only; no exposure of credentials or private file paths.\nwindow.MORFO_Bridge = { activeImageId: () => current?.id ?? null };\n'
# Branding of existing CSS deliberately preserved.
files={main:content,cmake:cm,html:h,app:a}
# Validate transformations completely before modifying any file.
if 'morfo_start_profile' not in content or 'morfo_roots_exp003' not in cm or '/morfo-profiles.js' not in content: raise RuntimeError('MORFO bridge validation failed')
for path,value in files.items():path.write_text(value,encoding='utf-8')
print('MORFO transformation: source anchors verified, API + UI + EXP-003 library integrated')
