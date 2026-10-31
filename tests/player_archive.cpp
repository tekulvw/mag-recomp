#include "player_archive.h"
#include <fstream>
#include <iterator>
#include <iostream>
int main(int argc,char**argv) {
    if(argc!=3)return 2;
    try {
        std::ifstream in(argv[1],std::ios::binary);
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
        const std::string text="MAGClientNetConfig.MAPSServerAddr = \"192.0.2.5\"\n";
        auto result=mag::rewrite_psarc_member(bytes,"/settings.lua",{text.begin(),text.end()});
        std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<const char*>(result.bytes.data()),result.bytes.size());
        if(!out)return 3;
        return 0;
    } catch(const std::exception&e) {std::cerr<<e.what()<<'\n';return 1;}
}
