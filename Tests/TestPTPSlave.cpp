#include "../NetworkEngine/PTP/PTPSlave.h"
#include <array>
#include <iostream>
#include <stdexcept>
namespace AES67 {
struct PTPSlaveTestAccess {
    static void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
    static void stamp(uint8_t* p,uint64_t ns){auto sec=ns/1000000000ULL;auto nano=ns%1000000000ULL;for(int i=0;i<6;++i)p[i]=sec>>(8*(5-i));for(int i=0;i<4;++i)p[6+i]=nano>>(8*(3-i));}
    static void largeEpochOffset(uint64_t masterBase, uint64_t localBase) {
        PTPSlave slave(PTPSlaveConfig{});
        PTPHeader head{}; head.sourcePortIdentity.clockIdentity.id[0]=1; head.flagField=0x0200;
        slave.hasMaster_=true; slave.currentMaster_.masterPortId=head.sourcePortIdentity;
        for(int i=0;i<20;++i) {
            uint64_t elapsed=uint64_t(i)*125000000ULL;
            uint64_t t1=masterBase+elapsed, t2=localBase+elapsed+100000ULL;
            head.sequenceId=uint16_t(i); std::array<uint8_t,54> data{};
            slave.handleSync(head,data.data(),44,t2);
            stamp(data.data()+34,t1); slave.handleFollowUp(head,data.data(),44);
            slave.t3_delayReqSendTimeNs_=t2+2000000ULL;
            slave.waitingForDelayResp_=true; slave.delayReqSequenceId_=head.sequenceId;
            stamp(data.data()+34,masterBase+elapsed+2200000ULL);
            slave.handleDelayResp(head,data.data(),54);
        }
        int64_t expected=int64_t(localBase)-int64_t(masterBase);
        require(slave.isLocked(),"Large stable epoch difference must lock without filter overflow");
        require(std::abs(slave.getOffsetNs()-expected)<1000,"Large epoch offset incorrect");
        require(slave.getMeanPathDelayNs()==100000,"Large epoch path delay incorrect");
        // A single heavily delayed Delay_Req must not poison otherwise precise
        // Sync timestamps. Keep the last validated path measurement until stale.
        for(int i=20;i<29;++i) {
            uint64_t elapsed=uint64_t(i)*125000000ULL;
            uint64_t t1=masterBase+elapsed, t2=localBase+elapsed+100000ULL;
            head.sequenceId=uint16_t(i); std::array<uint8_t,54> data{};
            slave.handleSync(head,data.data(),44,t2);
            stamp(data.data()+34,t1); slave.handleFollowUp(head,data.data(),44);
            if(i==20) {
                slave.t3_delayReqSendTimeNs_=t2+2000000ULL;
                slave.waitingForDelayResp_=true; slave.delayReqSequenceId_=head.sequenceId;
                stamp(data.data()+34,masterBase+elapsed+102200000ULL);
                slave.handleDelayResp(head,data.data(),54);
            }
        }
        require(std::abs(slave.getOffsetNs()-expected)<1000,"Delay outlier must not corrupt filtered clock offset");
        require(slave.isLocked(),"Precise sync with a fresh valid path estimate must remain locked");
        slave.lastDelaySteadyNs_=0;
        require(!slave.isLocked(),"Stale validated path measurement must unlock");

    }
    static void run(){
        largeEpochOffset(1000000000000ULL,1792433000000000000ULL);
        largeEpochOffset(1792433000000000000ULL,1000000000000ULL);
        PTPSlave slave(PTPSlaveConfig{});
        PTPHeader head{};head.sourcePortIdentity.clockIdentity.id[0]=1;head.flagField=0x0200;
        slave.hasMaster_=true;slave.currentMaster_.masterPortId=head.sourcePortIdentity;
        for(int i=0;i<12;++i){
            uint64_t t1=1700000037000000000ULL+uint64_t(i)*125000000ULL;
            uint64_t t2=t1-37000000000ULL+100000ULL;
            head.sequenceId=uint16_t(i);
            std::array<uint8_t,54> data{};
            slave.handleSync(head,data.data(),44,t2);
            stamp(data.data()+34,t1);
            slave.handleFollowUp(head,data.data(),44); // upstream deadlocked here
            slave.t3_delayReqSendTimeNs_=t2+2000000;
            slave.waitingForDelayResp_=true;slave.delayReqSequenceId_=head.sequenceId;
            stamp(data.data()+34,slave.t3_delayReqSendTimeNs_+37000000000ULL+100000);
            slave.handleDelayResp(head,data.data(),54); // upstream deadlocked here too
        }
        require(slave.isLocked(),"Stable measured epoch offset should lock");
        require(std::abs(slave.getOffsetNs()+37000000000LL)<1000,"Offset sign/epoch incorrect");
        require(slave.getMeanPathDelayNs()==100000,"Path delay incorrect");
        auto previous=slave.syncCount_.load();head.sourcePortIdentity.clockIdentity.id[0]=2;
        std::array<uint8_t,54> data{};slave.handleSync(head,data.data(),44,1);
        require(slave.syncCount_.load()==previous,"Foreign master must be ignored");
        slave.lastSyncSteadyNs_=0; require(!slave.isLocked(),"Stale sync must unlock");
        data[1]=2; data[3]=64;
        require(!slave.parseHeader(data.data(),34,head),"Truncated declared PTP message must fail");
        std::cout<<"PASS: PTP two-step/delay exchange, epoch offset, foreign source, stale lock, length checks\n";
    }
};}
int main(){try{AES67::PTPSlaveTestAccess::run();return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
