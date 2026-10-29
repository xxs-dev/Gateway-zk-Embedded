#include "edge_gateway/soe_store.hpp"
#include "edge_gateway/config_loader.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace edge_gateway;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void removeDb(const std::string& p){std::remove(p.c_str());std::remove((p+"-wal").c_str());std::remove((p+"-shm").c_str());}
}
int main(int argc,char** argv){
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string path="soe-recording-"+std::to_string(now)+".db";
    DeviceConfig c;c.machineCode="TEST";c.meterCode="AM5";c.protocol.iec.commonAddress=1;c.protocol.iec.soe.utcOffsetMinutes=0;
    Iec103DisturbanceRecord r;r.fan=15;r.state=6;r.rawTimeHex="881300001d0a1a";
    const auto time=soeRecordingTimeUtc(r.rawTimeHex,0);
    require(time==1793232005000LL,"CP56 UTC is independent of host timezone");
    require(soeRecordingTimeUtc("881300081d0a1a",480)==time,"fixed device UTC offset");
    require(soeRecordingTimeUtc("881380001d0a1a",0)==0,"invalid CP56 flag");
    require(soeRecordingTimeUtc("881300001d0217",0)==0,"invalid calendar date must not normalize");
    require(soeRecordingTimeUtc("zz",0)==0,"invalid CP56 hex");
    Iec103SoeEvent e;e.eventId="recording-event";e.machineCode=c.machineCode;e.deviceCode=c.meterCode;
    e.typeId=2;e.commonAddress=1;e.faultNumber=15;e.deviceTimeMs=time;e.receivedMs=now;
    e.timeValid=e.valueValid=true;e.rawDpi=e.dpi=2;e.name="Original event";e.state="DPI=2";
    const auto cfg=path+".CFG",dat=path+".DAT";
    {
        SoeStore s(path);s.append({e});
        auto status=[&](){return s.read("TEST").at(0).recording.status;};
        require(status()=="pending","ASDU2 starts pending");
        s.recordDirectory(c,{},true,true,now-1);require(status()=="pending","older directory is not negative evidence");
        s.recordDirectory(c,{},true,true,now+1);require(status()=="not_found","complete empty directory");
        s.recordDirectory(c,{},false,true,now+2);require(status()=="uncertain","truncated directory never means absent");
        s.recordDirectory(c,{},false,false,now+3);require(status()=="query_failed","query failure distinct from absence");
        s.recordDirectory(c,{r},true,true,now+4);require(status()=="linked","FAN and time match");
        require(s.read("TEST")[0].recording.directoryState==6,"raw directory state preserved without invented flags");
        s.recordDirectory(c,{r},false,true,now+5);require(status()=="uncertain","partial positive catalogue is uncertain");
        auto duplicate=r;duplicate.rawTimeHex="891300001d0a1a";
        s.recordDirectory(c,{r,duplicate},true,true,now+6);require(status()=="uncertain","duplicate FAN is ambiguous");
        auto recycled=r;recycled.rawTimeHex="881300001e0a1a";
        s.recordDirectory(c,{recycled},true,true,now+7);require(status()=="uncertain","reused FAN with different time");
        auto bad=r;bad.rawTimeHex="881380001d0a1a";
        s.recordDirectory(c,{bad},true,true,now+8);require(status()=="uncertain","invalid directory timestamp cannot link");
        auto other=c;other.machineCode="OTHER";s.recordDirectory(other,{r},true,true,now+9);
        require(status()=="uncertain","machine isolation");
        other=c;other.meterCode="OTHER";s.recordDirectory(other,{r},true,true,now+9);
        require(status()=="uncertain","device isolation");
        other=c;other.protocol.iec.commonAddress=2;s.recordDirectory(other,{r},true,true,now+9);
        require(status()=="uncertain","CA isolation");
        s.recordDirectory(c,{r},true,true,now+10);
        {std::ofstream f(cfg,std::ios::binary);f<<"CFG";}{std::ofstream f(dat,std::ios::binary);f<<"DAT";}
        Iec103ComtradeFiles files;files.fan=15;files.cfgPath=cfg;files.datPath=dat;files.cfgBytes=3;files.datBytes=3;
        s.recordDownload(c,r,files,now+11);require(status()=="downloaded","completed file association");
        s.recordDirectory(c,{},true,true,now+12);require(status()=="downloaded","download remains after device directory rollover");
        std::remove(dat.c_str());require(status()=="download_missing","missing local file must be visible");
        bool rejected=false;try{s.recordDownload(c,r,files,now+13);}catch(...){rejected=true;}
        require(rejected,"cannot claim missing file downloaded");
        auto a=e;a.eventId="asdu1";a.typeId=1;a.faultNumber=-1;s.append({a});
        require(s.read("TEST")[0].recording.status=="uncertain","ASDU1 cannot imply no recording");
        a=e;a.eventId="bad-time";a.timeValid=false;s.append({a});
        require(s.read("TEST")[0].recording.status=="uncertain","invalid event time cannot link");
        const auto rows=s.read("TEST");require(rows.back().name=="Original event"&&rows.back().state=="DPI=2","association never rewrites event evidence");
    }
    {SoeStore reader(path,true);require(reader.read("TEST").back().recording.status=="download_missing","association survives restart");
        bool rejected=false;try{reader.recordDirectory(c,{},true,true,now);}catch(...){rejected=true;}require(rejected,"reader cannot update directory");}
    std::remove(cfg.c_str());removeDb(path);
    auto settings=ConfigLoader::loadFromText(R"({"protocol":{"type":"iec103","iec":{"transportMode":"am5se_active_tcp","soe":{"enabled":true,"recordings":{"enabled":true}}}},"meters":[],"points":[]})");
    require(settings.protocol.iec.soe.recordingLinkEnabled,"recording association opt-in");
    bool rejected=false;try{ConfigLoader::loadFromText(R"({"protocol":{"iec":{"soe":{"recordings":{"enabled":true,"refreshIntervalMs":1}}}},"meters":[],"points":[]})");}catch(...){rejected=true;}
    require(rejected,"unsafe directory polling rejected");
    if(argc>1){
        {SoeStore reader(argv[1],true);const auto rows=reader.read("LEGACY");require(rows.size()==1&&rows[0].recording.status=="pending","legacy database read without migration");}
        {SoeStore writer(argv[1]);const auto rows=writer.read("LEGACY");require(rows.size()==1&&rows[0].eventId=="legacy-event","additive migration preserves events");}
    }
    std::cout<<"SOE recording association, ambiguity, migration and read-only tests passed\n";
}
