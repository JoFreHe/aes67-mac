#include "../NetworkEngine/PTP/ClockStatus.hpp"
#include <cassert>
#include <iostream>
int main(){
 using AES67::ClockStatus;
 auto line=std::string("1 ")+std::to_string(ClockStatus::now())+" -37000000000 1 0 en0 192.168.1.2 00-11-22-ff-fe-33-44-55";
 auto s=ClockStatus::parse(line);assert(s.locked && s.fresh() && s.offset==-37000000000LL && s.domain==0);
 s.written=ClockStatus::now()-4000000000ULL;assert(!s.fresh());
 s.written=ClockStatus::now()+2000000000ULL;assert(!s.fresh());
 assert(!ClockStatus::parse("garbage").locked);
 assert(!ClockStatus::parse("2 1 1 1 0 en0 1.2.3.4 gm").locked);
 assert(!ClockStatus::parse("1 1 1 3 0 en0 1.2.3.4 gm").locked);
 // Log reasons for a waiting sender (field test: adapter renamed, no master).
 auto fresh=[](std::string rest){return AES67::ClockStatus::parse("1 "+std::to_string(ClockStatus::now())+" 0 "+rest);};
 assert(AES67::describeClock(ClockStatus{},"en18",0).find("not running")!=std::string::npos);
 assert(AES67::describeClock(fresh("1 0 en13 192.168.0.125 gm"),"en18",0).find("needs en18")!=std::string::npos);
 assert(AES67::describeClock(fresh("1 7 en18 192.168.0.125 gm"),"en18",0).find("domain 7")!=std::string::npos);
 assert(AES67::describeClock(fresh("0 0 en18 192.168.0.125 -"),"en18",0).find("no PTP master")!=std::string::npos);
 assert(AES67::describeClock(fresh("1 0 en18 192.168.0.125 00-11"),"en18",0).rfind("PTP locked on en18",0)==0);
 std::cout<<"Clock status freshness and malformed records passed\n";
}
