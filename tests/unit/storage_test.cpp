#include "storage/resource_store.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace fetchkit;
using namespace fetchkit::storage;
namespace {
void require(bool value) { if(!value) throw std::runtime_error("storage assertion failed"); }
void vector(const std::filesystem::path& path,const std::string& content,const std::string& expected) {
    StagingFile file(path,true);file.write(reinterpret_cast<const std::uint8_t*>(content.data()),content.size());file.sync();
    require(sha256File(file)==expected);
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3);
        const auto root=std::filesystem::u8path(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        StagingStore store(root.u8string());
        vector(root/"empty","","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        vector(root/"abc","abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        vector(root/"padding","abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq","248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        vector(root/"million",std::string(1000000,'a'),"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        {
            StagingFile file(root/"offset",true);
            const std::uint8_t data=0x91;
            // Keep the regular unit suite small. Real >4 GiB writes and resume
            // offsets are exercised by the opt-in recovery_large_file test.
            file.seek(4294967301ULL);require(file.size()==0);
            file.seek(0);file.write(&data,1);require(file.size()==1);
            file.truncate();require(file.size()==0);
        }
        removeFile(root/"offset");
        Record r;r.request.resource_namespace="golden";r.request.resource_key="resource";r.request.version="1";
        r.request.reference="ref";r.request.expected_length=123;r.total=123;r.etag="\"v1\"";r.committed=17;
        r.id=resourceId(r.request);
        const auto bytes=encodeRecord(r);const auto decoded=decodeRecord(bytes);
        require(decoded.committed==17 && decoded.id==r.id && encodeRecord(decoded)==bytes);
        std::ifstream golden(argv[2]);std::string hex;golden>>hex;
        constexpr char digits[]="0123456789abcdef";std::string actual;
        for(auto byte:bytes) {actual+=digits[byte>>4];actual+=digits[byte&15];}
        require(actual==hex);
        auto damaged=bytes;damaged.back()^=1;
        bool rejected=false;
        try {decodeRecord(damaged);}catch(const core::Failure& error){rejected=error.code==FETCHKIT_INTEGRITY_MISMATCH;}
        require(rejected);
        damaged=bytes;damaged[8]=2;rejected=false;
        try {decodeRecord(damaged);}catch(const core::Failure& error){rejected=error.code==FETCHKIT_UNSUPPORTED_METADATA;}
        require(rejected);
        std::cout<<"4 SHA-256 vectors, 64-bit offsets and record golden/corruption/version passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
