// Privileged PTP-only helper. No audio processing, no shell execution.
#include "../NetworkEngine/PTP/PTPSlave.h"
#include "../NetworkEngine/PTP/ClockStatus.hpp"
#include "../NetworkEngine/RTP/TxNetworkSupport.hpp"
#include "../NetworkEngine/StreamConfig.h"
#include "../Driver/DebugLog.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <memory>
static volatile sig_atomic_t active=1;
static void stop(int){active=0;}
// State changes only (selection, lock, grandmaster): small enough to keep, so a
// failed field test can still be explained afterwards.
static const char* kLogPath="/Library/Application Support/AES67Driver/clock-service.log";
// The PTP slave reports socket, multicast and announce-timeout errors on stderr;
// launchd discards a daemon's stderr unless it points at a file.
static void attachStderr(){
    int fd=open(kLogPath,O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW,0644);
    if(fd>=0){dup2(fd,STDERR_FILENO);close(fd);}
}
static void note(const std::string& message){
    const char* path=kLogPath;
    struct stat info{};
    if(stat(path,&info)==0 && info.st_size>1024*1024){rename(path,"/Library/Application Support/AES67Driver/clock-service.log.1");attachStderr();}
    FILE* f=fopen(path,"a");if(!f)return;
    time_t now=time(nullptr);char stamp[32];strftime(stamp,sizeof(stamp),"%Y-%m-%d %H:%M:%S",localtime(&now));
    fprintf(f,"[%s] %s\n",stamp,message.c_str());fclose(f);chmod(path,0644);
}
static void publish(const std::string& iface,const std::string& ip,int domain,AES67::PTPSlave* clock){
    char path[]="/Library/Application Support/AES67Driver/.clock-status-XXXXXX";
    int fd=mkstemp(path);if(fd<0)return;
    std::ostringstream text;text<<"1 "<<AES67::ClockStatus::now()<<' '<<(clock?clock->getOffsetNs():0)<<' '<<(clock&&clock->isLocked()?1:0)<<' '<<domain<<' '<<(iface.empty()?"-":iface)<<' '<<(ip.empty()?"-":ip)<<' ';
    auto master=clock?clock->getGrandmasterID():"";text<<(master.empty()?"-":master)<<'\n';
    auto data=text.str();size_t done=0;
    while(done<data.size()){auto n=write(fd,data.data()+done,data.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0)break;done+=size_t(n);}
    fchmod(fd,0644);close(fd);
    if(done==data.size())rename(path,"/Library/Application Support/AES67Driver/clock-status.txt");else unlink(path);
}
int main(int argc,char** argv){
    // Own log (clock-service.log); the config is re-read every second and would
    // otherwise flood the driver log, written as root into world-writable /tmp.
    AES67::Debug::SetEnabled(false);
    bool validate=argc==3 && std::string(argv[1])=="--validate";
    if(argc!=1 && !validate){std::cerr<<"Usage: AES67ClockService [--validate config.json]\n";return 2;}
    AES67::StreamConfigManager manager;manager.setConfigPath(validate?argv[2]:"/Library/Application Support/AES67Driver/streams.json");
    if(validate){auto configs=manager.loadConfig();if(!configs)return 1;size_t rx=0,tx=0;for(auto& c:*configs){if(!c.isValid())return 1;if(c.sdp.direction=="sendonly")tx+=c.sdp.numChannels;else rx+=c.sdp.numChannels;}std::cout<<"Valid driver configuration: RX="<<rx<<" TX="<<tx<<'\n';return 0;}
    if(geteuid()!=0){std::cerr<<"This PTP helper must run through the installed launchd service.\n";return 1;}
    attachStderr();
    signal(SIGTERM,stop);signal(SIGINT,stop);
    std::unique_ptr<AES67::PTPSlave> clock;std::string currentName,currentIP;int currentDomain=-1;
    std::string lastSelection,lastLock,lastConfig;
    note("clock service started");
    while(active){
        auto configs=manager.loadConfig();std::set<std::pair<std::string,int>> requests;
        size_t invalid=0;
        if(configs)for(auto& c:*configs){
            if(!c.isValid()){++invalid;continue;}
            if(c.enabled && c.sdp.direction=="sendonly" && c.sdp.mediaClockType!="local-test")requests.insert({c.networkInterface,c.sdp.ptpDomain});
        }
        std::string configState=!configs?"streams.json missing or unreadable; PTP idle":
            invalid?std::to_string(invalid)+" invalid stream(s) in streams.json ignored":"";
        if(configState!=lastConfig){if(!configState.empty())note(configState);else if(!lastConfig.empty())note("streams.json readable again");lastConfig=configState;}
        std::string name,ip;int domain=-1;
        if(requests.size()==1){auto pair=AES67::txInterface(requests.begin()->first);name=pair.first;ip=pair.second;domain=requests.begin()->second;}
        std::string selection=requests.empty()?"no AES67 sender configured; PTP idle":
            requests.size()>1?"senders use "+std::to_string(requests.size())+" different interfaces/domains; PTP disabled":
            name.empty()?"interface "+requests.begin()->first+" has no IPv4 address; PTP waiting":
            "PTP on "+name+" "+ip+" domain "+std::to_string(domain);
        if(name!=currentName || domain!=currentDomain || ip!=currentIP || (!clock && !name.empty())){
            clock.reset();currentName=name;currentIP=ip;currentDomain=domain;
            if(!name.empty() && domain>=0 && domain<=127){AES67::PTPSlaveConfig c;c.interfaceName=name;c.domain=domain;clock=std::make_unique<AES67::PTPSlave>(c);if(!clock->start()){clock.reset();selection+="; PTP slave failed to start";}}
        }
        if(selection!=lastSelection){note(selection);lastSelection=selection;}
        std::string lock=!clock?"":clock->isLocked()?"locked to grandmaster "+clock->getGrandmasterID():"not locked: no PTP master seen on "+currentName+" domain "+std::to_string(currentDomain);
        if(lock!=lastLock){if(!lock.empty())note(lock);lastLock=lock;}
        publish(currentName,currentIP,currentDomain,clock.get());
        for(int i=0;i<10 && active;++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    clock.reset();publish(currentName,currentIP,currentDomain,nullptr);note("clock service stopped");return 0;
}
