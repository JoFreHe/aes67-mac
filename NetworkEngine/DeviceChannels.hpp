#pragma once
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
namespace AES67 {
struct DeviceChannels { uint32_t input=32, output=32; };
// Only top-level settings count. Strings/nested stream metadata cannot override
// device size. Invalid settings disable configuration loading, never clamp routes.
inline std::optional<DeviceChannels> parseDeviceChannels(std::string_view json) {
    DeviceChannels counts; bool seenInput=false,seenOutput=false;
    size_t i=0; int depth=0;
    auto ws=[&]{while(i<json.size() && (json[i]==' '||json[i]=='\n'||json[i]=='\r'||json[i]=='\t'))++i;};
    ws(); if(i==json.size() || json[i]!='{') return std::nullopt;
    for(;i<json.size();++i) {
        char c=json[i];
        if(c=='"') {
            size_t end=i+1; bool escape=false;
            for(;end<json.size();++end){if(escape)escape=false;else if(json[end]=='\\')escape=true;else if(json[end]=='"')break;}
            if(end==json.size())return std::nullopt;
            auto key=json.substr(i+1,end-i-1);
            size_t next=end+1;
            while(next<json.size() && (json[next]==' '||json[next]=='\n'||json[next]=='\r'||json[next]=='\t'))++next;
            if(depth==1 && next<json.size() && json[next]==':' && (key=="inputChannels" || key=="outputChannels")) {
                bool& seen=key=="inputChannels"?seenInput:seenOutput;
                if(seen)return std::nullopt;seen=true;
                i=end+1;ws();if(i==json.size()||json[i++]!=':')return std::nullopt;ws();
                int number=0;auto result=std::from_chars(json.data()+i,json.data()+json.size(),number);
                if(result.ec!=std::errc{} || number<1 || number>32)return std::nullopt;
                i=size_t(result.ptr-json.data());ws();if(i==json.size() || (json[i]!=',' && json[i]!='}'))return std::nullopt;
                (key=="inputChannels"?counts.input:counts.output)=uint32_t(number);
                --i;continue;
            }
            i=end;continue;
        }
        if(c=='{' || c=='[')++depth;
        if(c=='}' || c==']'){if(--depth<0)return std::nullopt;if(depth==0){++i;ws();return i==json.size()?std::optional<DeviceChannels>(counts):std::nullopt;}}
    }
    return std::nullopt;
}
inline std::optional<DeviceChannels> readDeviceChannels(const std::string& path) {
    std::ifstream f(path); if(!f.is_open())return DeviceChannels{};
    std::string json((std::istreambuf_iterator<char>(f)),{});return parseDeviceChannels(json);
}
}
