#include "edge_gateway/json_value.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void sameNumber(const std::string& text) {
    std::istringstream reference(text);
    reference.imbue(std::locale::classic());
    double expected=0;
    reference >> std::noskipws >> expected;
    const bool accepted=bool(reference)&&reference.eof()&&std::isfinite(expected);
    bool parsed=false; double actual=0;
    try { actual=edge_gateway::json::JsonParser(text,64,100).parse().asNumber(); parsed=true; }
    catch(const std::runtime_error&) {}
    require(parsed==accepted,"acceptance differs: "+text);
    if(parsed)require(actual==expected&&std::signbit(actual)==std::signbit(expected),"value differs: "+text);
}
struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};
void checks() {
    for(const auto* text:{"0","-0","1","-1","999999999999999","-999999999999999",
        "1000000000000000","-1000000000000000","9007199254740991","9007199254740992",
        "9007199254740993","18446744073709551615","-9223372036854775809",
        "1.25","-0.0","0e100","-0e100","1e-300","1e-320","1e-400",
        "1.7976931348623157e308","1e309","-1e309"})sameNumber(text);
    std::uint64_t state=0x123456789abcdefULL;
    for(int i=0;i<10000;++i) {
        state^=state<<13;state^=state>>7;state^=state<<17;
        const auto text=std::to_string(state%1000000000000000ULL);
        sameNumber(text);sameNumber("-"+text);
    }
    for(const auto* text:{"-","01","-01","1.",".1","1e","1e+","1e-","+1","1x","--1","NaN","Infinity","1,5"}) {
        bool rejected=false;
        try { edge_gateway::json::JsonParser(text,64,100).parse(); }catch(const std::runtime_error&){rejected=true;}
        require(rejected,std::string("invalid number accepted: ")+text);
    }
    const std::string nested="{\"index\":150000,\"values\":[-0,999999999999999,1.25]}";
    const auto parsed=edge_gateway::json::JsonParser(nested,64,100).parse();
    require(parsed.find("index")->asNumber()==150000,"object number");
    require(std::signbit(parsed.find("values")->asArray().values[0]->asNumber()),"nested negative zero");
    for(const auto limits:{std::make_pair(1,100),std::make_pair(64,2)}) {
        bool rejected=false;
        try{edge_gateway::json::JsonParser(nested,limits.first,limits.second).parse();}catch(const std::runtime_error&){rejected=true;}
        require(rejected,"depth/node budget bypassed");
    }
    const std::string legacy="01";
    require(edge_gateway::json::JsonParser(legacy).parse().asNumber()==1,"legacy mode changed");
}
}
int main() {
    try {
        checks();
        const auto old=std::locale();
        std::locale::global(std::locale(std::locale::classic(),new CommaDecimal));
        try{checks();}catch(...){std::locale::global(old);throw;}
        std::locale::global(old);
        std::cout<<"json_value_number_test passed: 40000 generated signed integers, boundaries, locale, syntax and budgets\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
