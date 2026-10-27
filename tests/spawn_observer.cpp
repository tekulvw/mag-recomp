#include "../src/mag_spawn_observer.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
using namespace mag_spawn_observer;
static uint64_t checks;
#define CHECK(v) do{++checks;if(!(v)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#v<<"\n";return 1;}}while(0)
static void put(std::vector<uint8_t>&m,uint64_t ea,uint32_t n){assert(ea+4<=m.size());m[ea]=n>>24;m[ea+1]=n>>16;m[ea+2]=n>>8;m[ea+3]=n;}
int main(int argc,char**argv){assert(argc==2);std::ifstream f(argv[1]);std::string raw((std::istreambuf_iterator<char>(f)),{});J c=parse(raw);CHECK(c.o.count("layout")==1);
 auto&l=c["layout"];l["manager"]["candidate_ea"]=J(uint64_t(0x1000));l["manager"]["candidate_initialization_state_ea"]=J(uint64_t(0xFF0));
 std::vector<uint8_t>m(0x80000);uint64_t fam=0x2000+35*160;put(m,0xFF0,2);put(m,0x1000+40,64);put(m,0x1000+48,0x2000);put(m,fam+12,224);put(m,fam+16,1024);put(m,fam+20,35);put(m,fam+156,0x10000);
 auto add=[&](uint32_t slot,uint32_t token,uint32_t type=0,bool active=true){uint64_t word=fam+24+4*(slot/32);uint32_t v=(m[word]<<24)|(m[word+1]<<16)|(m[word+2]<<8)|m[word+3];put(m,word,v|(1u<<(slot%32)));auto ea=0x10000+224*slot;put(m,ea+20,token);put(m,ea+24,0x23800000|slot);m[ea+124]=active;put(m,ea+128,1);put(m,ea+132,type);put(m,ea+136,0);put(m,ea+140,0);};
 for(unsigned i=0;i<20;i++){auto&p=c["authored_points"].a[i];uint32_t t=std::stoul(p.at("raw_token_expected").s,nullptr,0);add(i?3*i+1:0,t,uint32_t(p.at("authored_spawn_type").n),p.at("authored_active").b);}put(m,fam+152,20);
 uint64_t calls=0;Read rd=[&](uint64_t ea,void*out,size_t n){++calls;if(ea>m.size()||n>m.size()-ea)return false;std::memcpy(out,m.data()+ea,n);return true;};auto r=capture(c,rd);CHECK(r.at("status").s=="complete_concurrent");CHECK(r.at("runtime_rows").a.size()==20);CHECK(r.at("joins").at("unique").a.size()==20);CHECK(r.at("joins").at("missing").a.empty());CHECK(r.at("runtime_rows").a[1].at("slot").n==4);CHECK(r.at("runtime_rows").a[1].at("fields").at("active").n==0);CHECK(r.at("runtime_rows").a[1].at("raw_prefix_hex").s.size()==288);CHECK(r.at("stability").s=="UNKNOWN_CONCURRENT_BEST_EFFORT");CHECK(r.at("integrity").s=="metadata_unchanged_not_transaction");
 // No ordinal compaction: a sparse extra and a duplicate retain actual slot IDs.
 add(700,0x12345678);add(701,std::stoul(c["authored_points"].a[0].at("raw_token_expected").s,nullptr,0));put(m,fam+152,22);r=capture(c,rd);CHECK(r.at("runtime_rows").a.size()==22);CHECK(r.at("joins").at("duplicate").a.size()==1);CHECK(r.at("joins").at("extra").a.size()==1);CHECK(r.at("runtime_rows").a.back().at("slot").n==701);
 // Missing is reported, never called failed creation.
 put(m,fam+24,0);put(m,fam+152,14);r=capture(c,rd);CHECK(!r.at("joins").at("missing").a.empty());CHECK(dump(r).find("failed_creation")==std::string::npos);
 // Lower-only local caps and invalid widths/overflow refuse before unsafe read.
 for(auto key:{"maximum_slots","maximum_raw_guest_bytes","maximum_output_bytes"}){auto bad=c;bad["diagnostic_policy"][key]=J(uint64_t(1)<<40);calls=0;r=capture(bad,rd);CHECK(r.at("status").s=="refused");CHECK(calls==0);}
 auto low=c;low["diagnostic_policy"]["maximum_raw_guest_bytes"]=J(uint64_t(64));r=capture(low,rd);CHECK(r.at("status").s!="complete_concurrent");CHECK(r.at("raw_bytes_attempted").n<=64);
 auto bad=c;bad["layout"]["manager"]["candidate_ea"]=J(uint64_t(0xfffffff0));calls=0;r=capture(bad,rd);CHECK(r.at("status").s=="refused");CHECK(calls==0);
 bad=c;bad["layout"]["object_fields"]["object_id"]["offset"]=J(uint64_t(143));r=capture(bad,rd);CHECK(r.at("status").s=="refused");
 // Mapped-read errors after correct arithmetic preserve raw rows already copied.
 auto partial=Read([&](uint64_t ea,void*out,size_t n){if(ea==0x10000+224*34)return false;return rd(ea,out,n);});r=capture(c,partial);CHECK(r.at("status").s=="partial");CHECK(!r.at("runtime_rows").a.empty());CHECK(!r.at("errors").a.empty());
 // Integrity mismatch cannot be promoted to stable/coherent evidence.
 bool flipped=false;auto changing=Read([&](uint64_t ea,void*out,size_t n){bool ok=rd(ea,out,n);if(ea==fam&&n==160&&!flipped){m[fam+24]^=1;flipped=true;}return ok;});r=capture(c,changing);CHECK(r.at("integrity").s=="metadata_changed");CHECK(r.at("status").s=="partial");
 for(auto text:{"{\"a\":1,\"a\":2}","{\"x\":-1}","{\"x\":1.5}","{\"x\":18446744073709551616}"}){bool refused=false;try{parse(text);}catch(...){refused=true;}CHECK(refused);}
 CHECK(parse(dump(c)).at("layout").at("family").at("type").n==35);std::cout<<"PASS "<<checks<<" critical reader checks\n";return 0;}
