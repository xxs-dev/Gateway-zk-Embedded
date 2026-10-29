#include "edge_gateway/soe_store.hpp"
#include "edge_gateway/storage_budget.hpp"
#include <chrono>
#include <stdexcept>
#include <cstdlib>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace edge_gateway {
namespace {
struct sqlite3;
struct sqlite3_stmt;
constexpr const char* columns = "event_id,machine,device,name,state,raw_hex,raw_time,received_ms,device_ms,"
    "ca,type,cause,fun,inf,dpi,raw_dpi,relative_ms,fault,value_valid,time_valid,mapped";
bool fileMatches(const std::string& path, std::int64_t size) {
    struct stat info{};
    return size>0 && stat(path.c_str(),&info)==0 && (info.st_mode&S_IFMT)==S_IFREG && info.st_size==size;
}
}
struct SoeStore::Impl {
    void* module = nullptr;
    sqlite3* db = nullptr;
    bool readOnly;
    std::string path;
    std::chrono::steady_clock::time_point cleanup{};
    int (*open)(const char*, sqlite3**, int, const char*) = nullptr;
    int (*close)(sqlite3*) = nullptr;
    int (*exec)(sqlite3*, const char*, int(*)(void*,int,char**,char**),void*,char**) = nullptr;
    int (*prepare)(sqlite3*,const char*,int,sqlite3_stmt**,const char**) = nullptr;
    int (*finalize)(sqlite3_stmt*) = nullptr;
    int (*bindText)(sqlite3_stmt*,int,const char*,int,void(*)(void*)) = nullptr;
    int (*bindInt)(sqlite3_stmt*,int,long long) = nullptr;
    int (*step)(sqlite3_stmt*) = nullptr;
    long long (*columnInt)(sqlite3_stmt*,int) = nullptr;
    const unsigned char* (*columnText)(sqlite3_stmt*,int) = nullptr;
    int (*busyTimeout)(sqlite3*,int) = nullptr;
    const char* (*error)(sqlite3*) = nullptr;

    template<class T> void symbol(T& target,const char* name) {
#ifdef _WIN32
        target = reinterpret_cast<T>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
        target = reinterpret_cast<T>(dlsym(module, name));
#endif
        if (!target) throw std::runtime_error(std::string("SOE SQLite symbol missing: ")+name);
    }
    void sql(const std::string& value) {
        if (exec(db,value.c_str(),nullptr,nullptr,nullptr) != 0) throw std::runtime_error(error(db));
    }
    struct Statement {
        Impl& owner; sqlite3_stmt* value = nullptr;
        Statement(Impl& o,const std::string& sql):owner(o) {
            if (o.prepare(o.db,sql.c_str(),-1,&value,nullptr) != 0) {
                if (value) o.finalize(value);
                throw std::runtime_error(o.error(o.db));
            }
        }
        ~Statement(){owner.finalize(value);}
        void text(int i,const std::string& s) {
            if(owner.bindText(value,i,s.c_str(),static_cast<int>(s.size()),nullptr)!=0) throw std::runtime_error(owner.error(owner.db));
        }
        void number(int i,std::int64_t n) {
            if(owner.bindInt(value,i,n)!=0) throw std::runtime_error(owner.error(owner.db));
        }
        std::string text(int i) {
            const auto* p=owner.columnText(value,i);return p?reinterpret_cast<const char*>(p):"";
        }
        std::int64_t number(int i){return owner.columnInt(value,i);}
    };
    Impl(const std::string& p,bool ro):readOnly(ro),path(p) {}
    void init() {
#ifdef _WIN32
        module=LoadLibraryA("sqlite3.dll");
#else
        module=dlopen("libsqlite3.so.0",RTLD_NOW|RTLD_LOCAL);
#endif
        if (!module) throw std::runtime_error("SOE SQLite library unavailable");
        symbol(open,"sqlite3_open_v2");symbol(close,"sqlite3_close_v2");symbol(exec,"sqlite3_exec");
        symbol(prepare,"sqlite3_prepare_v2");symbol(finalize,"sqlite3_finalize");symbol(bindText,"sqlite3_bind_text");
        symbol(bindInt,"sqlite3_bind_int64");symbol(step,"sqlite3_step");symbol(columnInt,"sqlite3_column_int64");
        symbol(columnText,"sqlite3_column_text");symbol(busyTimeout,"sqlite3_busy_timeout");symbol(error,"sqlite3_errmsg");
        if(open(path.c_str(),&db,(readOnly?1:6)|0x10000,nullptr)!=0) throw std::runtime_error("SOE database unavailable: "+path);
        if(busyTimeout(db,100)!=0) throw std::runtime_error(error(db));
        if(readOnly){sql("PRAGMA query_only=ON;");return;}
        sql("PRAGMA journal_mode=WAL;");sql("PRAGMA synchronous=FULL;");
        sql("CREATE TABLE IF NOT EXISTS soe_events(id INTEGER PRIMARY KEY AUTOINCREMENT,event_id TEXT NOT NULL UNIQUE,"
            "machine TEXT NOT NULL,device TEXT NOT NULL,name TEXT NOT NULL,state TEXT NOT NULL,raw_hex TEXT NOT NULL,raw_time TEXT NOT NULL,"
            "received_ms INTEGER NOT NULL,device_ms INTEGER NOT NULL,ca INTEGER NOT NULL,type INTEGER NOT NULL,cause INTEGER NOT NULL,"
            "fun INTEGER NOT NULL,inf INTEGER NOT NULL,dpi INTEGER NOT NULL,raw_dpi INTEGER NOT NULL,relative_ms INTEGER NOT NULL,"
            "fault INTEGER NOT NULL,value_valid INTEGER NOT NULL,time_valid INTEGER NOT NULL,mapped INTEGER NOT NULL);");
        sql("CREATE INDEX IF NOT EXISTS soe_machine_id ON soe_events(machine,id);");
        sql("CREATE INDEX IF NOT EXISTS soe_received_id ON soe_events(received_ms,id);");
        // Side tables leave original event evidence and legacy readers intact.
        sql("CREATE TABLE IF NOT EXISTS soe_recording_checks(machine TEXT NOT NULL,device TEXT NOT NULL,ca INTEGER NOT NULL,"
            "checked_ms INTEGER NOT NULL,complete INTEGER NOT NULL,success INTEGER NOT NULL,window_ms INTEGER NOT NULL,"
            "PRIMARY KEY(machine,device,ca));");
        sql("CREATE TABLE IF NOT EXISTS soe_recording_directory(machine TEXT NOT NULL,device TEXT NOT NULL,ca INTEGER NOT NULL,"
            "fan INTEGER NOT NULL,time_ms INTEGER NOT NULL,state INTEGER NOT NULL,raw_time TEXT NOT NULL,"
            "PRIMARY KEY(machine,device,ca,fan,raw_time,state));");
        sql("CREATE TABLE IF NOT EXISTS soe_recording_downloads(machine TEXT NOT NULL,device TEXT NOT NULL,ca INTEGER NOT NULL,"
            "fan INTEGER NOT NULL,time_ms INTEGER NOT NULL,state INTEGER NOT NULL,raw_time TEXT NOT NULL,downloaded_ms INTEGER NOT NULL,"
            "cfg_path TEXT NOT NULL,dat_path TEXT NOT NULL,cfg_bytes INTEGER NOT NULL,dat_bytes INTEGER NOT NULL,"
            "PRIMARY KEY(machine,device,ca,fan,raw_time));");
    }
    void admit(std::size_t count) {
        if(readOnly) throw std::runtime_error("SOE store is read-only");
        const auto budget=checkStorageWriteAdmission({0,256ULL*1024*1024},path,estimateStorageWriteBytes(count));
        if(!budget.allowed) throw std::runtime_error(std::string("SOE recording storage unavailable: ")+budget.reason);
    }
    void scope(Statement& s,const DeviceConfig& config) {
        s.text(1,config.machineCode);s.text(2,config.meterCode);s.number(3,config.protocol.iec.commonAddress);
    }
    void eventScope(Statement& s,const Iec103SoeEvent& e) {
        s.text(1,e.machineCode);s.text(2,e.deviceCode);s.number(3,e.commonAddress);
    }
    bool recordingTablesExist() {
        Statement s(*this,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN "
            "('soe_recording_checks','soe_recording_directory','soe_recording_downloads');");
        if(step(s.value)!=100)throw std::runtime_error(error(db));
        return s.number(0)==3;
    }
    void describeRecording(Iec103SoeEvent& e,bool available) {
        auto& link=e.recording;
        if(e.typeId!=2 || e.faultNumber<0 || e.faultNumber>65535 || !e.timeValid || e.deviceTimeMs<=0) {
            link.status="uncertain";return;
        }
        if(!available)return;
        bool checked=false,complete=false,success=false;int window=1000;
        {
            Statement s(*this,"SELECT checked_ms,complete,success,window_ms FROM soe_recording_checks WHERE machine=? AND device=? AND ca=?;");
            eventScope(s,e);const auto rc=step(s.value);
            if(rc==100){checked=true;link.checkedMs=s.number(0);complete=s.number(1);success=s.number(2);window=static_cast<int>(s.number(3));}
            else if(rc!=101)throw std::runtime_error(error(db));
        }
        // A FAN alone is not an identity. Reject ambiguous or invalid timestamps.
        int seen=0,matches=0;
        {
            Statement s(*this,"SELECT time_ms,state,raw_time FROM soe_recording_directory WHERE machine=? AND device=? AND ca=? AND fan=?;");
            eventScope(s,e);s.number(4,e.faultNumber);int rc;
            while((rc=step(s.value))==100){
                ++seen;const auto time=s.number(0);
                if(time>0 && std::llabs(time-e.deviceTimeMs)<=window){
                    ++matches;link.directoryTimeMs=time;link.directoryState=static_cast<int>(s.number(1));link.directoryRawTime=s.text(2);
                }
            }
            if(rc!=101)throw std::runtime_error(error(db));
        }
        if(matches==1 && seen==1 && complete && success)link.status="linked";
        else if(seen)link.status="uncertain";
        else if(checked && link.checkedMs>=e.receivedMs)link.status=!success?"query_failed":complete?"not_found":"uncertain";
        if(matches!=1 || seen!=1){link.directoryTimeMs=0;link.directoryState=-1;link.directoryRawTime.clear();}
        int downloaded=0;
        SoeRecordingLink saved=link;
        {
            Statement s(*this,"SELECT time_ms,state,raw_time,downloaded_ms,cfg_path,dat_path,cfg_bytes,dat_bytes FROM soe_recording_downloads "
                "WHERE machine=? AND device=? AND ca=? AND fan=? AND time_ms BETWEEN ? AND ? LIMIT 2;");
            eventScope(s,e);s.number(4,e.faultNumber);s.number(5,e.deviceTimeMs-window);s.number(6,e.deviceTimeMs+window);int rc;
            while((rc=step(s.value))==100){
                ++downloaded;saved.directoryTimeMs=s.number(0);saved.directoryState=static_cast<int>(s.number(1));
                saved.directoryRawTime=s.text(2);saved.downloadedMs=s.number(3);saved.cfgPath=s.text(4);saved.datPath=s.text(5);
                saved.cfgBytes=s.number(6);saved.datBytes=s.number(7);
            }
            if(rc!=101)throw std::runtime_error(error(db));
        }
        if(downloaded==1 && (!seen || (matches==1 && seen==1 && saved.directoryRawTime==link.directoryRawTime))){
            link=saved;link.status=fileMatches(link.cfgPath,link.cfgBytes)&&fileMatches(link.datPath,link.datBytes)?"downloaded":"download_missing";
        }else if(downloaded>1)link.status="uncertain";
    }
    ~Impl(){
        if(db && close) close(db);
        if(module){
#ifdef _WIN32
            FreeLibrary(static_cast<HMODULE>(module));
#else
            dlclose(module);
#endif
        }
    }
};
SoeStore::SoeStore(const std::string& path,bool ro):impl_(new Impl(path,ro)){impl_->init();}
SoeStore::~SoeStore()=default;

void SoeStore::recordDirectory(const DeviceConfig& config,const std::vector<Iec103DisturbanceRecord>& records,
    bool complete,bool success,std::int64_t checkedMs) {
    auto& p=*impl_;
    if(config.machineCode.empty() || config.meterCode.empty() || checkedMs<=0 || records.size()>4096 || (complete&&!success))
        throw std::invalid_argument("invalid SOE recording directory snapshot");
    p.admit(records.size()+1);p.sql("BEGIN IMMEDIATE;");
    try {
        {Impl::Statement s(p,"DELETE FROM soe_recording_directory WHERE machine=? AND device=? AND ca=?;");
            p.scope(s,config);if(p.step(s.value)!=101)throw std::runtime_error(p.error(p.db));}
        for(const auto& r:records){
            if(r.fan<0 || r.fan>65535 || r.state<0 || r.state>255 || r.rawTimeHex.size()>32)throw std::invalid_argument("invalid directory record");
            Impl::Statement s(p,"INSERT OR IGNORE INTO soe_recording_directory VALUES(?,?,?,?,?,?,?);");
            p.scope(s,config);s.number(4,r.fan);s.number(5,soeRecordingTimeUtc(r.rawTimeHex,config.protocol.iec.soe.utcOffsetMinutes));
            s.number(6,r.state);s.text(7,r.rawTimeHex);if(p.step(s.value)!=101)throw std::runtime_error(p.error(p.db));
        }
        {Impl::Statement s(p,"INSERT OR REPLACE INTO soe_recording_checks VALUES(?,?,?,?,?,?,?);");
            p.scope(s,config);s.number(4,checkedMs);s.number(5,complete);s.number(6,success);s.number(7,config.protocol.iec.soe.recordingMatchWindowMs);
            if(p.step(s.value)!=101)throw std::runtime_error(p.error(p.db));}
        p.sql("COMMIT;");
    }catch(...){try{p.sql("ROLLBACK;");}catch(...){}throw;}
}

void SoeStore::recordDownload(const DeviceConfig& config,const Iec103DisturbanceRecord& record,
    const Iec103ComtradeFiles& files,std::int64_t downloadedMs) {
    auto& p=*impl_;const auto time=soeRecordingTimeUtc(record.rawTimeHex,config.protocol.iec.soe.utcOffsetMinutes);
    if(config.machineCode.empty() || config.meterCode.empty() || time<=0 || downloadedMs<=0 || record.fan!=files.fan ||
        !fileMatches(files.cfgPath,files.cfgBytes) || !fileMatches(files.datPath,files.datBytes))
        throw std::invalid_argument("unverified SOE recording download");
    p.admit(1);
    Impl::Statement s(p,"INSERT OR REPLACE INTO soe_recording_downloads VALUES(?,?,?,?,?,?,?,?,?,?,?,?);");
    p.scope(s,config);s.number(4,record.fan);s.number(5,time);s.number(6,record.state);s.text(7,record.rawTimeHex);
    s.number(8,downloadedMs);s.text(9,files.cfgPath);s.text(10,files.datPath);s.number(11,files.cfgBytes);s.number(12,files.datBytes);
    if(p.step(s.value)!=101)throw std::runtime_error(p.error(p.db));
}

void SoeStore::append(const std::vector<Iec103SoeEvent>& events) {
    auto& p=*impl_;
    if(p.readOnly) throw std::runtime_error("SOE store is read-only");
    if(events.empty())return;
    if(events.size()>256) throw std::runtime_error("SOE batch too large");
    for(const auto& e:events) if(e.eventId.empty() || e.eventId.size()>128 || e.machineCode.empty() ||
        e.rawHex.size()>1024 || e.name.size()>1024 || e.state.size()>256) throw std::runtime_error("invalid SOE record");
    const auto admission=checkStorageWriteAdmission({0,256ULL*1024*1024},p.path,estimateStorageWriteBytes(events.size()));
    if(!admission.allowed)throw std::runtime_error(std::string("SOE storage unavailable: ")+admission.reason);
    p.sql("BEGIN IMMEDIATE;");
    try {
        for(const auto& e:events){
            Impl::Statement s(p,std::string("INSERT OR IGNORE INTO soe_events(")+columns+") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
            s.text(1,e.eventId);s.text(2,e.machineCode);s.text(3,e.deviceCode);s.text(4,e.name);s.text(5,e.state);
            s.text(6,e.rawHex);s.text(7,e.rawTimeHex);
            const std::int64_t numbers[]={e.receivedMs,e.deviceTimeMs,e.commonAddress,e.typeId,e.cause,e.functionType,
                e.informationNumber,e.dpi,e.rawDpi,e.relativeTimeMs,e.faultNumber,e.valueValid,e.timeValid,e.mapped};
            for(int i=0;i<14;++i)s.number(i+8,numbers[i]);
            if(p.step(s.value)!=101)throw std::runtime_error(p.error(p.db));
        }
        p.sql("COMMIT;");
    } catch(...){try{p.sql("ROLLBACK;");}catch(...){}throw;}
    // Bound maintenance work; device clock changes never determine retention.
    const auto now=std::chrono::steady_clock::now();
    if(now>=p.cleanup){
        const auto wall=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        Impl::Statement s(p,"DELETE FROM soe_events WHERE id IN (SELECT id FROM soe_events WHERE received_ms<? ORDER BY received_ms,id LIMIT 512);");
        s.number(1,wall-30LL*86400000);
        if(p.step(s.value)!=101)throw std::runtime_error(p.error(p.db));
        p.cleanup=now+std::chrono::minutes(1);
    }
}

std::vector<Iec103SoeEvent> SoeStore::read(const std::string& machine,std::int64_t before,int limit){
    if(machine.empty() || before<0 || limit<1 || limit>200)throw std::invalid_argument("invalid SOE history query");
    auto& p=*impl_;
    p.sql("BEGIN;");
    try {
    const bool recordingTables=p.recordingTablesExist();
    Impl::Statement s(p,std::string("SELECT id,")+columns+" FROM soe_events WHERE machine=?"+
        (before>0?" AND id<?":"")+" ORDER BY id DESC LIMIT ?;");
    s.text(1,machine);
    if(before>0)s.number(2,before);
    s.number(before>0?3:2,limit);
    std::vector<Iec103SoeEvent> result;
    int rc;
    while((rc=p.step(s.value))==100){
        Iec103SoeEvent e;e.id=s.number(0);e.eventId=s.text(1);e.machineCode=s.text(2);e.deviceCode=s.text(3);
        e.name=s.text(4);e.state=s.text(5);e.rawHex=s.text(6);e.rawTimeHex=s.text(7);
        e.receivedMs=s.number(8);e.deviceTimeMs=s.number(9);e.commonAddress=s.number(10);e.typeId=s.number(11);
        e.cause=s.number(12);e.functionType=s.number(13);e.informationNumber=s.number(14);e.dpi=s.number(15);
        e.rawDpi=s.number(16);e.relativeTimeMs=s.number(17);e.faultNumber=s.number(18);
        e.valueValid=s.number(19);e.timeValid=s.number(20);e.mapped=s.number(21);
        p.describeRecording(e,recordingTables);result.push_back(e);
    }
    if(rc!=101)throw std::runtime_error(p.error(p.db));
    p.sql("COMMIT;");return result;
    }catch(...){try{p.sql("ROLLBACK;");}catch(...){}throw;}
}
} // namespace edge_gateway
