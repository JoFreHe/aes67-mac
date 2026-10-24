#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
namespace AES67 {
// Continuous-phase interpolation for small independent-clock drift. This is not
// a mastering sample-rate converter. All channels in a flow share one phase.
class ClockRateBridge {
public:
    explicit ClockRateBridge(size_t channels):channels_(channels),buffer_(1024*channels){}
    void reset(){frames_=0;phase_=0;}
    template<class Reader> bool render(float* output,size_t count,double ratio,Reader read){
        if(!channels_ || !count || count>512)return false;
        ratio=std::clamp(ratio,0.998,1.002);
        size_t needed=size_t(std::ceil(phase_+(count-1)*ratio))+2;
        bool complete=true;
        if(needed>frames_){complete=read(buffer_.data()+frames_*channels_,needed-frames_);frames_=needed;}
        for(size_t i=0;i<count;++i){
            double position=phase_+i*ratio;size_t index=size_t(position);float fraction=float(position-index);
            for(size_t ch=0;ch<channels_;++ch){float a=buffer_[index*channels_+ch],b=buffer_[(index+1)*channels_+ch];output[i*channels_+ch]=a+(b-a)*fraction;}
        }
        double next=phase_+count*ratio;size_t consumed=size_t(next);phase_=next-consumed;
        frames_-=consumed;std::memmove(buffer_.data(),buffer_.data()+consumed*channels_,frames_*channels_*sizeof(float));
        return complete;
    }
private:
    size_t channels_,frames_=0;double phase_=0;std::vector<float> buffer_;
};
}
