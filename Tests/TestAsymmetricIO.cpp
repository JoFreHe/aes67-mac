#include "../Driver/AES67IOHandler.h"
#include <aspl/Device.hpp>
#include <cassert>
#include <iostream>
using namespace AES67;
template<size_t... I> auto buffers(std::index_sequence<I...>){return std::array<SPSCRingBuffer<float>,128>{((void)I,SPSCRingBuffer<float>(128))...};}
int main(){
 auto in=buffers(std::make_index_sequence<128>{}),out=buffers(std::make_index_sequence<128>{});
 std::atomic<uint64_t> iu{0},ou{0};std::atomic<bool> running{true};
 RTSafeStreamInterface rt(in,out,iu,ou,running);AES67IOHandler handler(rt,4,sizeof(float),8);
 float tx[24];for(int i=0;i<24;++i)tx[i]=float(i+1);
 handler.OnWriteClientOutput(nullptr,nullptr,0,0,tx,3,8);
 for(int ch=0;ch<8;++ch){float a[3]{};assert(out[ch].read(a,3)==3);for(int f=0;f<3;++f)assert(a[f]==tx[f*8+ch]);}
 for(int ch=0;ch<4;++ch){float a[3]={float(ch+1),float(ch+11),float(ch+21)};assert(in[ch].write(a,3)==3);}
 float rx[12]{};handler.OnReadClientInput(nullptr,nullptr,0,0,rx,sizeof(rx));
 for(int f=0;f<3;++f)for(int ch=0;ch<4;++ch)assert(rx[f*4+ch]==float(f*10+ch+1));
 handler.OnWriteClientOutput(nullptr,nullptr,0,0,tx,3,4);assert(out[0].available()==0);
 // Exercise the same default-mixing dispatch used by HAL, not just a direct
 // call to the per-client callback (which HAL never uses in this mode).
 auto context=std::make_shared<aspl::Context>();
 auto device=std::make_shared<aspl::Device>(context);
 aspl::StreamParameters params;
 params.Direction=aspl::Direction::Output;
 params.Format={48000,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked,32,1,32,8,32,0};
 auto stream=std::make_shared<aspl::Stream>(context,device,params);
 device->AddStreamAsync(stream);
 device->SetIOHandler(std::make_shared<AES67IOHandler>(rt,4,sizeof(float),8));
 Boolean willDo=false,inPlace=false;
 assert(device->WillDoIOOperation(device->GetID(),0,kAudioServerPlugInIOOperationWriteMix,&willDo,&inPlace)==noErr);
 assert(willDo && inPlace);
 AudioServerPlugInIOCycleInfo cycle{};
 assert(device->DoIOOperation(device->GetID(),stream->GetID(),0,kAudioServerPlugInIOOperationWriteMix,3,&cycle,tx,nullptr)==noErr);
 for(int ch=0;ch<8;++ch){float a[3]{};assert(out[ch].read(a,3)==3);for(int f=0;f<3;++f)assert(a[f]==tx[f*8+ch]);}
 handler.OnWriteMixedOutput(nullptr,0,0,nullptr,sizeof(tx));
 handler.OnWriteMixedOutput(nullptr,0,0,tx,sizeof(tx)-1);
 handler.OnWriteMixedOutput(nullptr,0,0,tx,0);
 assert(out[0].available()==0);
 assert(out[8].available()==0);std::cout<<"PASS: 4 input / 8 output mapping, HAL WriteMix dispatch, malformed output rejected\n";
}
