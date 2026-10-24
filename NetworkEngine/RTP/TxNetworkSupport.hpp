#pragma once
#include "../PTP/ClockStatus.hpp"
#include "../../Driver/SDPParser.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <cstring>

namespace AES67 {
struct TxClockShared {
    ClockStatusReader clock;
    TxClockShared(const std::string& interface,int domain):clock(interface,domain){}
    uint64_t now()const {
        auto local=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return uint64_t(local-clock.getOffsetNs());
    }
};
inline std::pair<std::string,std::string> txInterface(const std::string& requested){
    std::pair<std::string,std::string> result;
    ifaddrs* entries=nullptr;
    if(getifaddrs(&entries)!=0)return result;
    for(auto* entry=entries;entry;entry=entry->ifa_next){
        if(!entry->ifa_addr || entry->ifa_addr->sa_family!=AF_INET)continue;
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET,&reinterpret_cast<sockaddr_in*>(entry->ifa_addr)->sin_addr,ip,sizeof(ip));
        if(requested==entry->ifa_name || requested==ip){result={entry->ifa_name,ip};break;}
    }
    freeifaddrs(entries);return result;
}
inline std::shared_ptr<TxClockShared> txClock(const std::string& name,int domain){
    // Every sender reads the same privileged PTP service snapshot. Per-thread
    // reader instances avoid sharing mutable cached snapshots across TX workers.
    return std::make_shared<TxClockShared>(name,domain);
}
inline uint32_t ptpSampleTimestamp(uint64_t ns,uint32_t rate){
    return uint32_t((ns/1000000000ULL)*rate+((ns%1000000000ULL)*rate)/1000000000ULL);
}
inline bool announceTx(const SDPSession& session,const std::string& ip,uint16_t hash,bool deletion){
    std::string text=SDPParser::generate(session);
    if(text.empty())return false;
    in_addr source{};if(inet_pton(AF_INET,ip.c_str(),&source)!=1)return false;
    std::vector<uint8_t> message{uint8_t(deletion?0x24:0x20),0,uint8_t(hash>>8),uint8_t(hash)};
    auto* bytes=reinterpret_cast<uint8_t*>(&source.s_addr);message.insert(message.end(),bytes,bytes+4);
    const char mime[]="application/sdp";message.insert(message.end(),mime,mime+sizeof(mime));
    message.insert(message.end(),text.begin(),text.end());
    int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return false;
    uint8_t ttl=1;
    bool ok=setsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&source,sizeof(source))==0 && setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl))==0;
    sockaddr_in dest{};dest.sin_family=AF_INET;dest.sin_port=htons(9875);inet_pton(AF_INET,"239.255.255.255",&dest.sin_addr);
    if(ok)ok=sendto(fd,message.data(),message.size(),0,reinterpret_cast<sockaddr*>(&dest),sizeof(dest))==ssize_t(message.size());
    close(fd);return ok;
}
}
