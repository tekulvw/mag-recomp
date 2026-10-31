#include "player_archive.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <zlib.h>

namespace {
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
uint64_t be(const uint8_t* p,unsigned n){uint64_t v=0;while(n--)v=(v<<8)|*p++;return v;}
void put(std::vector<uint8_t>& b,size_t off,uint64_t v,unsigned n){while(n){b[off+--n]=uint8_t(v);v>>=8;}}
struct Entry {size_t block,length,offset,blocks;};
class Archive {
public:
    const std::vector<uint8_t>& bytes;
    size_t toc{},block_size{},width{},table{};
    std::vector<size_t> sizes;
    std::vector<Entry> entries;
    std::vector<std::string> names;
    explicit Archive(const std::vector<uint8_t>& source):bytes(source) {
        require(bytes.size()>=32,"Truncated settings archive");
        auto p=bytes.data();
        require(!std::memcmp(p,"PSAR",4)&&be(p+4,4)==0x10003&&!std::memcmp(p+8,"zlib",4)&&be(p+16,4)==30,
                "Unsupported settings archive format");
        toc=be(p+12,4);size_t count=be(p+20,4);block_size=be(p+24,4);
        require(count>1&&count<=100000&&block_size>=2&&block_size<=1048576&&!(block_size&(block_size-1)),
                "Invalid settings archive header");
        width=0;for(size_t v=block_size-1;v;v>>=8)++width;
        table=32+count*30;
        require(table<=toc&&toc<=bytes.size()&&(toc-table)%width==0,"Invalid settings archive table");
        for(size_t at=table;at<toc;at+=width){size_t n=be(p+at,unsigned(width));sizes.push_back(n?n:block_size);}
        for(size_t i=0;i<count;++i){
            const auto e=p+32+30*i;
            Entry entry{size_t(be(e+16,4)),size_t(be(e+20,5)),size_t(be(e+25,5)),0};
            entry.blocks=(entry.length+block_size-1)/block_size;
            require(entry.block<=sizes.size()&&entry.blocks<=sizes.size()-entry.block&&entry.offset>=toc&&entry.offset<=bytes.size(),
                    "Invalid settings archive member");
            size_t left=bytes.size()-entry.offset;
            for(size_t j=0;j<entry.blocks;++j){require(sizes[entry.block+j]<=left,"Truncated settings archive member");left-=sizes[entry.block+j];}
            entries.push_back(entry);
        }
        auto raw=extract(0,16*1024*1024);
        std::string text(raw.begin(),raw.end());size_t from=0;
        for(size_t at=0;at<text.size();++at)if(text[at]=='\n'){names.push_back(text.substr(from,at-from));from=at+1;}
        if(from<text.size())names.push_back(text.substr(from));
        require(names.size()==entries.size()-1,"Invalid settings archive names");
    }
    size_t find(const std::string& member) const {
        require(std::count(names.begin(),names.end(),member)==1,"Settings archive member is missing or duplicated");
        return size_t(std::find(names.begin(),names.end(),member)-names.begin())+1;
    }
    std::vector<uint8_t> extract(size_t index,size_t limit=1048576) const {
        const auto& e=entries.at(index);require(e.length<=limit,"Settings archive member exceeds its limit");
        std::vector<uint8_t> result(e.length);size_t at=e.offset,dest=0;
        for(size_t j=0;j<e.blocks;++j){
            size_t stored=sizes[e.block+j],expected=std::min(block_size,e.length-dest);
            if(stored==expected)std::memcpy(result.data()+dest,bytes.data()+at,expected);
            else{uLongf length=expected;require(uncompress(result.data()+dest,&length,bytes.data()+at,stored)==Z_OK&&length==expected,
                                              "Cannot decode settings archive member");}
            at+=stored;dest+=expected;
        }
        return result;
    }
};
}

std::vector<uint8_t> mag::read_psarc_member(const std::vector<uint8_t>& source,const std::string& member) {
    Archive archive(source);return archive.extract(archive.find(member));
}

mag::PsarcReplacement mag::rewrite_psarc_member(const std::vector<uint8_t>& source,const std::string& member,
                                               const std::vector<uint8_t>& replacement) {
    Archive archive(source);size_t index=archive.find(member);const auto& e=archive.entries[index];
    require(e.blocks==1&&!replacement.empty()&&replacement.size()<=archive.block_size,"Settings replacement must fit one archive block");
    for(size_t i=0;i<archive.entries.size();++i)if(i!=index){const auto& other=archive.entries[i];
        require(!(other.block<=e.block&&e.block-other.block<other.blocks),"Settings archive shares its replacement block");}
    uLongf length=compressBound(replacement.size());std::vector<uint8_t> stored(length);
    require(compress2(stored.data(),&length,replacement.data(),replacement.size(),Z_BEST_COMPRESSION)==Z_OK,"Cannot compress settings");
    stored.resize(length);if(stored.size()>=replacement.size())stored=replacement;
    require(stored.size()<(uint64_t(1)<<(archive.width*8)),"Settings block length cannot be encoded");
    require(source.size()<(uint64_t(1)<<40),"Settings archive is too large");
    PsarcReplacement result{source,archive.extract(index)};
    put(result.bytes,32+30*index+20,replacement.size(),5);
    put(result.bytes,32+30*index+25,source.size(),5);
    put(result.bytes,archive.table+e.block*archive.width,stored.size(),unsigned(archive.width));
    result.bytes.insert(result.bytes.end(),stored.begin(),stored.end());
    return result;
}
