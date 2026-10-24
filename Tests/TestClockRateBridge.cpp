#include "../NetworkEngine/RTP/ClockRateBridge.hpp"
#include <array>
#include <cmath>
#include <iostream>
int main(){
    for(double ratio:{0.9998,1.0,1.0002}) {
        AES67::ClockRateBridge bridge(4);uint64_t produced=0;double expected=0;std::array<float,48*4> out{};
        for(int block=0;block<2000;++block){
            if(!bridge.render(out.data(),48,ratio,[&](float* target,size_t frames){for(size_t i=0;i<frames;++i)for(size_t ch=0;ch<4;++ch)target[i*4+ch]=float(produced+i)+float(ch)*100000.f;produced+=frames;return true;}))return 1;
            for(size_t i=0;i<48;++i)for(size_t ch=0;ch<4;++ch)if(std::abs(out[i*4+ch]-float(expected+i*ratio+ch*100000))>0.05f)return 2;
            expected+=48*ratio;
        }
        if(std::abs(double(produced)-expected)>3)return 3;
    }
    std::cout<<"PASS: continuous phase, four-channel isolation and +/-200ppm rate conversion\n";
}
