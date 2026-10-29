#include "edge_gateway/iec103_soe.hpp"
#include "edge_gateway/config_loader.hpp"
#include "edge_gateway/soe_store.hpp"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>

using namespace edge_gateway;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){
    IecProtocolConfig protocol;protocol.linkAddressSize=1;protocol.soe.utcOffsetMinutes=0;
    const std::int64_t day=1793232000000LL;
    std::vector<std::uint8_t> frame={0x28,1,1,0x81,1,1,1,100,2,0x88,0x13,0,0,0};
    auto e=decodeIec103Soe(frame,protocol,day+5001).at(0);
    require(e.dpi==2&&e.deviceTimeMs==day+5000&&e.timeValid&&e.valueValid,"ASDU1 CP32/DPI");
    auto gi=frame;gi[4]=9;require(decodeIec103Soe(gi,protocol,day).empty(),"GI must not create SOE");
    gi[2]=44;require(decodeIec103Soe(gi,protocol,day).empty(),"ASDU44 not SOE");
    gi=frame;gi.pop_back();bool rejected=false;
    try{decodeIec103Soe(gi,protocol,day);}catch(...){rejected=true;}require(rejected,"truncation rejected");
    gi=frame;gi[11]=0x80;require(!decodeIec103Soe(gi,protocol,day).at(0).timeValid,"invalid time retained");
    gi=frame;gi[8]=3;require(!decodeIec103Soe(gi,protocol,day).at(0).valueValid,"DPI3 invalid");
    gi=frame;gi[9]=0x5f;gi[10]=0xea;gi[11]=59;gi[12]=23;
    require(decodeIec103Soe(gi,protocol,day+10).at(0).deviceTimeMs==day-1,"midnight previous day");
    require(decodeIec103Soe(frame,protocol,day-1).at(0).deviceTimeMs==day+5000,"midnight next day");
    auto relative=frame;relative[2]=2;relative.erase(relative.end()-1);relative.insert(relative.begin()+9,{0x34,0x12,15,0});
    auto r=decodeIec103Soe(relative,protocol,day+5001).at(0);
    require(r.relativeTimeMs==0x1234&&r.faultNumber==15&&r.deviceTimeMs==day+5000,"ASDU2 relative time and FAN");
    DeviceConfig config;config.machineCode="COMM_TEST";config.meterCode="AM5-F";
    describeIec103Soe(e,config);require(!e.mapped&&e.state=="DPI=2","no invented DPI meaning");
    config.protocol.iec.soe.mappings.push_back({1,1,1,100,"Breaker","Open","Closed"});
    describeIec103Soe(e,config);require(e.mapped&&e.state=="Closed","explicit mapping");
    auto settings=ConfigLoader::loadFromText(R"({"protocol":{"type":"iec103","iec":{"transportMode":"am5se_active_tcp","soe":{"enabled":true,"mappings":[{"commonAddress":1,"typeId":1,"functionType":1,"informationNumber":100,"name":"Breaker","state1":"Open","state2":"Closed","confirmed":true}]}}},"meters":[],"points":[]})");
    require(settings.protocol.iec.soe.enabled&&settings.protocol.iec.soe.mappings.size()==1,"SOE config parsed");
    rejected=false;try{ConfigLoader::loadFromText(R"({"protocol":{"iec":{"soe":{"enabled":true}}},"meters":[],"points":[]})");}
    catch(...){rejected=true;}require(rejected,"non-AM5 transport rejected");
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string path="soe-test-"+std::to_string(now)+".db";
    e.receivedMs=now;e.eventId="test-1";
    {
        SoeStore writer(path);writer.append({e});writer.append({e});
        e.eventId="test-2";e.dpi=1;e.state="Open";writer.append({e});
        // Identical device timestamps and content can be distinct arrivals.
        e.eventId="test-3";writer.append({e});
        auto rows=writer.read("COMM_TEST");require(rows.size()==3,"idempotent commit, distinct arrivals retained");
        require(rows[0].state=="Open"&&rows[2].state=="Closed","rapid action/return preserved");
        require(writer.read("OTHER").empty(),"machine isolation");
        require(writer.read("COMM_TEST",rows[0].id,1)[0].eventId=="test-2","keyset paging");
    }
    {SoeStore reader(path,true);require(reader.read("COMM_TEST").size()==3,"restart durability");
        rejected=false;try{reader.append({e});}catch(...){rejected=true;}require(rejected,"read-only cannot write");}
    std::remove(path.c_str());std::remove((path+"-wal").c_str());std::remove((path+"-shm").c_str());
    rejected=false;try{SoeStore missing(path,true);}catch(...){rejected=true;}require(rejected,"reader cannot create database");
    std::cout<<"AM5 SOE codec, mapping, durable history and pagination passed\n";
}
