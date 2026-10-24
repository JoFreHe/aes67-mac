// Passive SAP discovery for the X32 receive-only prototype. No Dante control writes.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <cstring>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>

static std::string parseSAP(const unsigned char* data, size_t n) {
    if (n < 8 || n > 65507 || (data[0] >> 5) != 1) return {};
    // R, deletion, encrypted and compressed packets are not descriptions to import.
    if (data[0] & 0x0f) return {};
    const size_t header = (data[0] & 0x10) ? 20 : 8; // IPv6 or IPv4 origin
    const size_t offset = header + size_t(data[1]) * 4;
    if (offset >= n) return {};
    const char* begin = reinterpret_cast<const char*>(data + offset);
    size_t size = n - offset;
    // RFC2974 MIME type, plus legacy SAP senders without the MIME prefix.
    if (size < 3 || std::memcmp(begin, "v=0", 3) != 0) {
        const void* zero = std::memchr(begin, 0, size);
        if (!zero) return {};
        size_t mimeLen = static_cast<const char*>(zero) - begin;
        if (std::string(begin, mimeLen) != "application/sdp") return {};
        begin += mimeLen + 1; size -= mimeLen + 1;
    }
    while (size && begin[size-1] == 0) --size;
    if (size < 4 || std::memcmp(begin, "v=0", 3) != 0 || std::memchr(begin, 0, size)) return {};
    std::string sdp(begin, size);
    if (sdp.find("\nm=audio ") == std::string::npos || sdp.find("\na=rtpmap:") == std::string::npos) return {};
    return sdp;
}

static int selfTest() {
    const std::string sdp = "v=0\r\ns=Test\r\nc=IN IP4 239.69.1.1/32\r\nm=audio 5004 RTP/AVP 96\r\na=rtpmap:96 L24/48000/4\r\n";
    std::vector<unsigned char> packet{0x20,0,0,1,127,0,0,1};
    const char mime[]="application/sdp";
    packet.insert(packet.end(),mime,mime+sizeof(mime));
    packet.insert(packet.end(),sdp.begin(),sdp.end());
    if (parseSAP(packet.data(),packet.size()) != sdp) return 1;
    for (unsigned char flags : {0x24,0x22,0x21,0x40}) {
        packet[0]=flags; if (!parseSAP(packet.data(),packet.size()).empty()) return 2;
    }
    packet[0]=0x20; packet[1]=255;
    if (!parseSAP(packet.data(),packet.size()).empty()) return 3;
    packet[1]=0;
    if (!parseSAP(packet.data(),7).empty()) return 4;
    // IPv6 origin shifts MIME offset by 12 bytes.
    packet.insert(packet.begin()+8,12,0); packet[0]=0x30;
    if (parseSAP(packet.data(),packet.size())!=sdp) return 5;
    std::cout << "SAP parser self-test passed\n"; return 0;
}

int main(int argc,char** argv) {
    if (argc==2 && std::string(argv[1])=="--self-test") return selfTest();
    std::string iface,out; int seconds=30;
    try {
        for (int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if (arg=="--interface" && i+1<argc) iface=argv[++i];
            else if (arg=="--output" && i+1<argc) out=argv[++i];
            else if (arg=="--seconds" && i+1<argc) seconds=std::stoi(argv[++i]);
            else throw std::runtime_error("Unknown/missing argument");
        }
        in_addr interfaceAddr{};
        if (iface.empty() || out.empty() || seconds<1 || seconds>300 || inet_pton(AF_INET,iface.c_str(),&interfaceAddr)!=1 || interfaceAddr.s_addr==INADDR_ANY)
            throw std::runtime_error("Usage: AES67Discover --interface LOCAL_IP --output DIRECTORY [--seconds 30]");
        std::filesystem::create_directories(out);
        int fd=socket(AF_INET,SOCK_DGRAM,0);
        if(fd<0) throw std::runtime_error(std::strerror(errno));
        struct CloseSocket { int fd; ~CloseSocket(){::close(fd);} } guard{fd};
#ifdef __APPLE__
        // Membership alone does not constrain a shared multicast socket to the
        // selected adapter. Bind discovery to its actual interface as well.
        ifaddrs* addresses=nullptr;
        if(getifaddrs(&addresses)!=0) throw std::runtime_error(std::strerror(errno));
        unsigned int interfaceIndex=0;
        for(auto* item=addresses;item;item=item->ifa_next) {
            if(item->ifa_addr && item->ifa_addr->sa_family==AF_INET &&
               reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr==interfaceAddr.s_addr) {
                interfaceIndex=if_nametoindex(item->ifa_name); break;
            }
        }
        freeifaddrs(addresses);
        if(!interfaceIndex) throw std::runtime_error("Selected interface is no longer available");
        if(setsockopt(fd,IPPROTO_IP,IP_BOUND_IF,&interfaceIndex,sizeof(interfaceIndex))<0)
            throw std::runtime_error(std::strerror(errno));
#endif
        int yes=1;
        if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes))<0 || setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof(yes))<0)
            throw std::runtime_error(std::strerror(errno));
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_port=htons(9875); address.sin_addr.s_addr=INADDR_ANY;
        if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0) throw std::runtime_error(std::strerror(errno));
        for(const char* group : {"239.255.255.255","224.2.127.254"}) {
            ip_mreq request{}; inet_pton(AF_INET,group,&request.imr_multiaddr); request.imr_interface=interfaceAddr;
            if(setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&request,sizeof(request))<0) throw std::runtime_error(std::strerror(errno));
        }
        std::cerr << "Listening for AES67 SAP announcements on " << iface << " for " << seconds << " seconds.\n";
        std::set<std::string> seen;
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
        std::array<unsigned char,65536> buffer{};
        while(std::chrono::steady_clock::now()<end && seen.size()<128) {
            pollfd p{fd,POLLIN,0}; int ready=poll(&p,1,200);
            if(ready<0 && errno==EINTR) continue;
            if(ready<0) throw std::runtime_error(std::strerror(errno));
            if(!(p.revents&POLLIN)) continue;
            ssize_t length=recv(fd,buffer.data(),buffer.size(),0);
            if(length<=0) continue;
            auto sdp=parseSAP(buffer.data(),size_t(length));
            if(sdp.empty() || !seen.insert(sdp).second) continue;
            // Names never come from untrusted network strings. mkstemps avoids overwrite/symlinks.
            auto pattern=(std::filesystem::path(out)/"AES67-XXXXXX.sdp").string();
            std::vector<char> name(pattern.begin(),pattern.end()); name.push_back(0);
            int file=mkstemps(name.data(),4);
            if(file<0) throw std::runtime_error(std::strerror(errno));
            size_t done=0;
            while(done<sdp.size()) {
                auto wrote=write(file,sdp.data()+done,sdp.size()-done);
                if(wrote<0 && errno==EINTR) continue;
                if(wrote<=0) {close(file); throw std::runtime_error("Failed to save SDP");}
                done+=size_t(wrote);
            }
            close(file); std::cout << name.data() << std::endl;
        }
        std::cerr << "Found " << seen.size() << " announcements. No results does not prove AES67 unavailable; check firmware, interface, flows and SAP advertisement.\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
