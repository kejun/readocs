#include "cache_merge.hpp"
#include "external_storage.hpp"
#include "cache_policy.hpp"
#include "source_metadata.hpp"
#include <cassert>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
namespace rd {
#include "sd_types.hpp"
struct Config { std::string server="https://proxy",token="test"; };
struct Model {
 std::mutex mutex;
 std::vector<Entry> entries;
 std::vector<Source> sources;
 std::string etag;
 bool sync_pending=false;
 std::string refresh_id,refresh_message;
 bool refresh_waiting=false,refresh_failed=false,check_notice=false;
 bool check_queued=false;
} model;
static bool sd=false,storage_full=false,sync_made_progress=false,redraw=false;
static bool last_not_ready=false;
static uint64_t free_bytes=16000000000ULL;
static int64_t clock_ms=0,deadline=0;
static int references=0,chunks=0,commits=0,version=1;
static std::string error_text,progress,visible_status;
static std::string saved_refresh,refresh_state="running";
static bool save_refresh_ok=true;
static bool interrupt_on_read=false;
static int refresh_code=202,status_code=200;
static std::vector<std::string> calls;
static bool save_refresh_request(const std::string &id) {if(!save_refresh_ok)return false;saved_refresh=id;return true;}
static std::map<std::string,std::string> files,bodies,ids;
static std::string sha256(const std::string &s) {
 std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(64)<<std::hash<std::string>{}(s);return out.str();
}
static bool valid_hash(const std::string &s) {return s.size()==64;}
static Config config_copy(){return {};}
static bool ensure_wifi(){return true;}
static bool ensure_time(){return true;}
bool external_cache_enabled(){return sd;}
static int64_t now_ms(){return clock_ms;}
bool cache_has_space(uint64_t size){return !sd || free_bytes>=2*size;}
bool cache_body_verified(const std::string &id){return files.count(id)>0;}
static std::string read_file(const std::string &id){if(interrupt_on_read)model.check_queued=true;return files.count(id)?files.at(id):"";}
static bool write_file(const std::string &id,const std::string &body){files[id]=body;if(sd)free_bytes-=body.size();return true;}
static void collect_cache(bool=false){}
static void prepare_download_space(){}
static void clear_directory_progress(){progress.clear();}
static std::string directory_progress(){return progress;}
static bool save_directory_progress_file(const std::string &s){progress=s;return true;}
static std::pair<std::string,std::string> current_article(){return {"rss","doc0"};}
static void set_status(const std::string &s,bool=true){visible_status=s;}
static bool commit_cache(const std::vector<Entry> &entries,const std::string &,const std::vector<Source> &) {
 for(const auto &e:entries) if(!e.snapshot_id.empty()) assert(sha256(read_file(e.snapshot_id))==e.snapshot_id);
 ++commits;return true;
}
template<typename...Args> static void log(Args...){}
#define ESP_LOGW(...) log(__VA_ARGS__)
static std::string etag(){return "etag"+std::to_string(version);}
static int request(const Config &,const std::string &path,Http &out,const std::string & = "",const std::string &condition="") {
 calls.push_back(path);
 ++clock_ms;
 Json root(cJSON_CreateObject());
 if(path=="/v1/refresh") {
  out.body=R"({"schema_version":1,"refresh_id":"abcdef123456"})";
  return refresh_code;
 } else if(path=="/v1/status?refresh_id=abcdef123456") {
  cJSON_AddNumberToObject(root.get(),"schema_version",1);
  auto *p=cJSON_AddObjectToObject(root.get(),"refresh");
  cJSON_AddStringToObject(p,"id","abcdef123456");cJSON_AddStringToObject(p,"state",refresh_state.c_str());
  cJSON_AddNumberToObject(p,"total_sources",2);cJSON_AddNumberToObject(p,"completed_sources",1);
  cJSON_AddNumberToObject(p,"total_documents",30);cJSON_AddNumberToObject(p,"processed_documents",15);
  out.body=serialize(root.get());return status_code;
 } else if(path=="/v1/sync") {
  if(condition==etag()) return 304;
  out.etag=etag();cJSON_AddNumberToObject(root.get(),"schema_version",1);
  auto *a=cJSON_AddArrayToObject(root.get(),"sources");auto *s=cJSON_CreateObject();
  cJSON_AddStringToObject(s,"id","rss");cJSON_AddStringToObject(s,"title","RSS");cJSON_AddNumberToObject(s,"count",30);
  cJSON_AddStringToObject(s,"catalog_revision",sha256("catalog").c_str());cJSON_AddStringToObject(s,"body_revision",etag().c_str());cJSON_AddItemToArray(a,s);
 } else if(path.find("/catalog?")!=std::string::npos) {
  cJSON_AddStringToObject(root.get(),"catalog_revision",sha256("catalog").c_str());
  auto *a=cJSON_AddArrayToObject(root.get(),"items");
  for(int i=0;i<30;++i) {auto *e=cJSON_CreateObject();auto doc="doc"+std::to_string(i);
   cJSON_AddStringToObject(e,"document_id",doc.c_str());cJSON_AddStringToObject(e,"title",doc.c_str());
   cJSON_AddStringToObject(e,"snapshot_id",last_not_ready&&i==29?"":ids[doc].c_str());cJSON_AddItemToArray(a,e);}
  cJSON_AddNullToObject(root.get(),"next_cursor");
 } else if(path.find("/documents/")!=std::string::npos) {
  ++references;clock_ms+=300;auto doc=path.substr(path.find_last_of('/')+1);
  cJSON_AddStringToObject(root.get(),"document_id",doc.c_str());
  if(last_not_ready&&doc=="doc29")cJSON_AddNullToObject(root.get(),"snapshot");
  else {auto *s=cJSON_AddObjectToObject(root.get(),"snapshot");auto id=ids[doc];auto body=bodies[id];
   cJSON_AddStringToObject(s,"snapshot_id",id.c_str());cJSON_AddStringToObject(s,"sha256",id.c_str());cJSON_AddNumberToObject(s,"bytes",body.size());
   auto *a=cJSON_AddArrayToObject(s,"chunks");auto *ch=cJSON_CreateObject();cJSON_AddNumberToObject(ch,"index",0);cJSON_AddNumberToObject(ch,"bytes",body.size());cJSON_AddStringToObject(ch,"sha256",id.c_str());cJSON_AddItemToArray(a,ch);}
 } else if(path.find("/chunks/")!=std::string::npos) {
  ++chunks;clock_ms+=600;auto id=path.substr(14,64);out.body=bodies.at(id);return 200;
 } else assert(false);
 out.body=serialize(root.get());return 200;
}
#include "sd_sync_under_test.hpp"
static unsigned cached(){return std::count_if(model.entries.begin(),model.entries.end(),[](const auto &e){return !e.snapshot_id.empty();});}
static void reset(bool external) {
 sd=external;model.entries.clear();model.sources.clear();model.etag.clear();model.sync_pending=false;
 storage_full=sync_made_progress=redraw=last_not_ready=false;clock_ms=references=chunks=commits=0;version=1;
 progress.clear();files.clear();bodies.clear();ids.clear();free_bytes=16000000000ULL;
 model.refresh_id.clear();model.refresh_message.clear();model.refresh_waiting=model.refresh_failed=model.check_notice=false;
 model.check_queued=false;
 interrupt_on_read=false;
 saved_refresh.clear();refresh_state="running";save_refresh_ok=true;refresh_code=202;status_code=200;calls.clear();
 for(int i=0;i<30;++i){auto doc="doc"+std::to_string(i);auto body="{\"schema_version\":1,\"blocks\":[{\"block_id\":\""+doc+"\",\"text\":\"Chinese reader\"}]}";auto id=sha256(body);ids[doc]=id;bodies[id]=body;}
}
static void round(int budget) {deadline=clock_ms+budget;sync_made_progress=storage_full=false;assert(sync(false));}
}
int main(){using namespace rd;
 assert(sd_can_fit({true,16000000000ULL,15000000000ULL},5000000000ULL));
 assert(!sd_can_fit({true,16000000000ULL,sd_reserved_bytes-1},1));
 assert(!sd_can_fit({false,16000000000ULL,15000000000ULL},1));
 assert(!sd_can_fit({true,16000000000ULL,15000000000ULL},UINT64_MAX));
 assert(!sd_can_fit({true,16000000000ULL,7*1024*1024},1024));
 assert(sd_can_publish({true,16000000000ULL,7*1024*1024},512*1024));
 assert(!sd_can_publish({true,16000000000ULL,7*1024*1024},UINT64_MAX));
 reset(false);round(20000);assert(cached()==5 && chunks==5);
 reset(true);round(8000);assert(cached()>0&&cached()<30&&model.sync_pending);
 for(int i=0;i<10&&model.sync_pending;++i)round(8000);
 assert(cached()==30&&!model.sync_pending&&references==30&&chunks==30&&commits>3);
 round(8000);assert(cached()==30&&chunks==30); // 304 must retain all SD bodies.
 files.erase(ids["doc21"]);round(120000);assert(cached()==30&&chunks==31);
 reset(true);round(8000);++version;
 auto fresh=std::string("{\"schema_version\":1,\"blocks\":[{\"block_id\":\"updated\",\"text\":\"new revision\"}]}");
 ids["doc0"]=sha256(fresh);bodies[ids["doc0"]]=fresh;
 for(int i=0;i<12&&model.sync_pending;++i)round(8000);
 assert(cached()==30&&!model.sync_pending&&model.entries[0].snapshot_id==ids["doc0"]);
 reset(true);free_bytes=900;round(120000);assert(storage_full&&cached()>0&&cached()<30&&!model.sync_pending);
 reset(true);last_not_ready=true;round(120000);assert(cached()==29&&!model.sync_pending);
 round(120000);assert(cached()==29&&!model.sync_pending&&!sync_made_progress);
 // A manual check starts all upstream sources before a directory-budget return.
 reset(true);deadline=clock_ms+2000;assert(sync(true));
 assert(calls[0]=="/v1/refresh" && calls[1]=="/v1/status?refresh_id=abcdef123456" && calls[2]=="/v1/sync");
 assert(!progress.empty() && model.sync_pending && model.refresh_waiting && saved_refresh==model.refresh_id);
 assert(std::count(calls.begin(),calls.end(),"/v1/refresh")==1);
 // Even another manual check joins the existing pass instead of resetting it.
 deadline=clock_ms+120000;assert(sync(true));assert(cached()==30 && model.sync_pending);
 assert(std::count(calls.begin(),calls.end(),"/v1/refresh")==1);
 assert(visible_status=="来源更新 1/2，正文 15/30");
 auto old_chunks=chunks;
 round(120000);assert(chunks==old_chunks && model.refresh_waiting);
 refresh_state="completed";round(120000);
 assert(!model.sync_pending&&!model.refresh_waiting&&model.refresh_id.empty()&&saved_refresh.empty());
 assert(visible_status=="已同步");
 reset(true);refresh_state="partial";deadline=120000;assert(sync(true));
 assert(cached()==30&&!model.sync_pending&&model.refresh_failed&&visible_status.find("失败")!=std::string::npos);
 reset(true);status_code=503;deadline=120000;assert(!sync(true));
 assert(!saved_refresh.empty()&&model.refresh_waiting&&calls.size()==2);
 status_code=200;round(120000);assert(std::count(calls.begin(),calls.end(),"/v1/refresh")==1);
 reset(true);refresh_code=429;deadline=120000;assert(!sync(true));
 assert(calls.size()==1&&saved_refresh.empty()); // Never replay POST or pull stale cache as success.
 reset(true);save_refresh_ok=false;deadline=120000;assert(!sync(true));
 assert(calls.size()==1&&saved_refresh.empty());
 reset(true);model.check_queued=true;deadline=120000;assert(sync(false));
 assert(calls.empty()&&files.empty()); // Cold background work yields before any catalog fetch.
 model.check_queued=false;assert(sync(true));assert(calls.front()=="/v1/refresh");
 reset(true);interrupt_on_read=true;
 std::vector<Entry> repairing{{"rss","doc0","First",ids["doc0"]},{"rss","doc1","Second",ids["doc1"]}};
 assert(!repair_external_entries(repairing,true));
 assert(repairing[0].snapshot_id.empty() && !repairing[1].snapshot_id.empty());
 assert(calls.empty()); // A newly queued check interrupts safely between files.
}
