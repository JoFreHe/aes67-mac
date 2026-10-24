#pragma once
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdint>
namespace AES67 {
struct ClockStatus {
    uint64_t written=0;int64_t offset=0;bool locked=false;int domain=-1;
    std::string interfaceName,interfaceIP,master;
    static uint64_t now(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
    static ClockStatus parse(const std::string& line){
        ClockStatus s;std::istringstream in(line);int version=0,lock=0;
        if(!(in>>version>>s.written>>s.offset>>lock>>s.domain>>s.interfaceName>>s.interfaceIP>>s.master) || version!=1 || (lock!=0 && lock!=1))return {};
        s.locked=lock==1;return s;
    }
    bool fresh()const{auto n=now();return written<=n+500000000ULL && n<=written+3000000000ULL;}
};
// Why a sender on `interface`/`domain` may or may not transmit, for the log.
inline std::string describeClock(const ClockStatus& s,const std::string& interface,int domain){
    if(!s.fresh())return "no current clock status (clock service not running or blocked)";
    std::string where=s.interfaceName+" "+s.interfaceIP+" domain "+std::to_string(s.domain);
    if(s.interfaceName!=interface && s.interfaceIP!=interface)
        return "clock service runs PTP on "+where+", this sender needs "+interface+" domain "+std::to_string(domain);
    if(s.domain!=domain)return "clock service uses domain "+std::to_string(s.domain)+", this sender needs domain "+std::to_string(domain);
    if(!s.locked)return "no PTP master locked on "+where;
    return "PTP locked on "+where+", grandmaster "+s.master;
}
// Network worker only: never used from the real-time CoreAudio callback.
class ClockStatusReader {
public:
    ClockStatusReader(std::string iface,int domain):interface_(std::move(iface)),domain_(domain){}
    bool isLocked()const{
        auto current=std::chrono::steady_clock::now();
        if(current>=nextRead_){
            std::ifstream f("/Library/Application Support/AES67Driver/clock-status.txt");std::string line;
            snapshot_=(std::getline(f,line)?ClockStatus::parse(line):ClockStatus{});
            nextRead_=current+std::chrono::milliseconds(250);
        }
        return snapshot_.locked && snapshot_.fresh() && snapshot_.domain==domain_ &&
            (snapshot_.interfaceName==interface_ || snapshot_.interfaceIP==interface_);
    }
    // Why the sender may or may not transmit, for the driver log.
    std::string describe()const{return describeClock(snapshot_,interface_,domain_);}
    int64_t getOffsetNs()const{return snapshot_.offset;}
    std::string getGrandmasterID()const{return snapshot_.master;}
private:
    std::string interface_;int domain_;mutable ClockStatus snapshot_;
    mutable std::chrono::steady_clock::time_point nextRead_{};
};
}
