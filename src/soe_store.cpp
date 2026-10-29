#include "edge_gateway/soe_store.hpp"
#include "edge_gateway/storage_budget.hpp"
#include <chrono>
#include <stdexcept>
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
        e.valueValid=s.number(19);e.timeValid=s.number(20);e.mapped=s.number(21);result.push_back(e);
    }
    if(rc!=101)throw std::runtime_error(p.error(p.db));
    return result;
}
} // namespace edge_gateway
