#include "storage/record.h"
#include "storage/sha256.h"
#include <algorithm>
#include <array>
#include <set>
#include <zlib.h>

namespace fetchkit::storage {
namespace {
constexpr std::uint8_t magic[] = {'F','K','R','E','C','0','1',0};
void put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned int size) {
    while (size--) { out.push_back(static_cast<std::uint8_t>(value)); value >>= 8; }
}
std::uint64_t get(const std::vector<std::uint8_t>& in, std::size_t at, unsigned int size) {
    if (at > in.size() || size > in.size() - at) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    std::uint64_t result = 0;
    for (unsigned int i = 0; i < size; ++i) result |= std::uint64_t(in[at + i]) << (8 * i);
    return result;
}
bool utf8(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        auto c = static_cast<unsigned char>(value[i++]);
        if (!c) return false;
        if (c < 128) continue;
        unsigned int n, v, minimum;
        if (c >= 0xc2 && c <= 0xdf) { n=1; v=c&31; minimum=0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n=2; v=c&15; minimum=0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n=3; v=c&7; minimum=0x10000; }
        else return false;
        while (n--) {
            if (i == value.size()) return false;
            c = static_cast<unsigned char>(value[i++]);
            if ((c & 0xc0) != 0x80) return false;
            v = (v<<6) | (c&63);
        }
        if (v < minimum || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return false;
    }
    return true;
}
void validate(const Record& r) {
    const auto& q = r.request;
    if (!validId(r.id) || r.id != resourceId(q) || !utf8(q.resource_namespace) || q.resource_namespace.empty() || q.resource_namespace.size()>256 ||
        !utf8(q.resource_key) || q.resource_key.empty() || q.resource_key.size()>1024 || !utf8(q.version) || q.version.empty() || q.version.size()>256 ||
        !utf8(q.representation_key) || q.representation_key.size()>1024 || !utf8(q.reference) || q.reference.size()>2048 ||
        r.etag.size()>1024 || !r.generation || r.generation == UINT64_MAX || r.committed>INT64_MAX ||
        (r.total && (*r.total>INT64_MAX || r.committed>*r.total)) || (q.expected_length && *q.expected_length>INT64_MAX) ||
        (q.expected_length && r.total && q.expected_length != r.total) || r.state>FETCHKIT_COMPLETED || r.integrity>FETCHKIT_DIGEST_VERIFIED ||
        r.error>FETCHKIT_UNSUPPORTED_METADATA || (q.require_digest && q.expected_sha256.empty())) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    if (!q.expected_sha256.empty() && (q.expected_sha256.size()!=64 ||
        q.expected_sha256.find_first_not_of("0123456789abcdef")!=std::string::npos)) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    if (!r.etag.empty()) {
        if (r.etag.size()<2 || r.etag.front()!='"' || r.etag.back()!='"') throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
        for (std::size_t i=1; i+1<r.etag.size(); ++i) {
            const auto c=static_cast<unsigned char>(r.etag[i]);
            if (c<33 || c==127 || c=='"') throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
        }
    }
    if (r.state==FETCHKIT_PUBLISHING || r.state==FETCHKIT_COMPLETED) {
        if (!r.total || r.committed!=*r.total || r.integrity==FETCHKIT_INTEGRITY_NONE ||
            (!q.expected_sha256.empty() && r.integrity!=FETCHKIT_DIGEST_VERIFIED) ||
            (q.expected_sha256.empty() && r.integrity!=FETCHKIT_TRANSPORT_COMPLETE)) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    }
}
}

std::string resourceId(const core::DownloadRequest& r) {
    const auto identity = r.resource_namespace+'\0'+r.resource_key+'\0'+r.version+'\0'+r.representation_key;
    picosha2::hash256_one_by_one hash;
    hash.process(identity.begin(),identity.end());hash.finish();return hashHex(hash);
}
bool validId(const std::string& id) { return id.size()==64 && id.find_first_not_of("0123456789abcdef")==std::string::npos; }

std::vector<std::uint8_t> encodeRecord(const Record& r) {
    validate(r);
    std::vector<std::uint8_t> payload;
    auto field = [&](unsigned int tag, const std::string& value) {
        put(payload, tag, 2); put(payload, 1, 2); put(payload, value.size(), 4);
        payload.insert(payload.end(), value.begin(), value.end());
    };
    auto integer = [&](unsigned int tag, std::uint64_t value) {
        std::vector<std::uint8_t> bytes; put(bytes, value, 8);
        field(tag, std::string(bytes.begin(), bytes.end()));
    };
    field(1,r.id); field(2,r.request.resource_namespace); field(3,r.request.resource_key); field(4,r.request.version);
    field(5,r.request.representation_key); field(6,r.request.reference); field(7,r.request.expected_sha256);
    integer(8,r.request.require_digest); integer(9,r.committed); integer(10,r.generation); field(11,r.etag);
    integer(12,r.state); integer(13,r.integrity); integer(14,r.error);
    if (r.request.expected_length) integer(15,*r.request.expected_length);
    if (r.total) integer(16,*r.total);
    std::vector<std::uint8_t> out(std::begin(magic), std::end(magic));
    put(out,1,4); put(out,payload.size(),4);
    auto crc = crc32(0, out.data(), static_cast<uInt>(out.size()));
    crc = crc32(crc,payload.data(),static_cast<uInt>(payload.size()));
    put(out,crc,4); out.insert(out.end(),payload.begin(),payload.end());
    return out;
}

Record decodeRecord(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size()<20 || bytes.size()>65556 || !std::equal(std::begin(magic),std::end(magic),bytes.begin())) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    if (get(bytes,8,4)!=1) throw core::Failure(FETCHKIT_UNSUPPORTED_METADATA);
    if (get(bytes,12,4)!=bytes.size()-20) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    auto crc=crc32(0,bytes.data(),16); crc=crc32(crc,bytes.data()+20,static_cast<uInt>(bytes.size()-20));
    if (crc!=get(bytes,16,4)) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    Record r;
    std::set<unsigned int> seen;
    for (std::size_t at=20; at<bytes.size();) {
        const auto tag=static_cast<unsigned int>(get(bytes,at,2)), flags=static_cast<unsigned int>(get(bytes,at+2,2));
        const auto length=get(bytes,at+4,4); at+=8;
        if (flags>1 || length>bytes.size()-at || !seen.insert(tag).second) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
        const std::string value(bytes.begin()+at,bytes.begin()+at+length);
        auto integer=[&]() { if (length!=8) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH); return get(bytes,at,8); };
        switch (tag) {
            case 1:r.id=value;break; case 2:r.request.resource_namespace=value;break;
            case 3:r.request.resource_key=value;break; case 4:r.request.version=value;break;
            case 5:r.request.representation_key=value;break; case 6:r.request.reference=value;break;
            case 7:r.request.expected_sha256=value;break;
            case 8:{const auto v=integer(); if(v>1) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);r.request.require_digest=v!=0;break;}
            case 9:r.committed=integer();break; case 10:r.generation=integer();break; case 11:r.etag=value;break;
            case 12:{const auto v=integer();if(v>FETCHKIT_COMPLETED) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);r.state=static_cast<std::uint32_t>(v);break;}
            case 13:{const auto v=integer();if(v>FETCHKIT_DIGEST_VERIFIED) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);r.integrity=static_cast<std::uint32_t>(v);break;}
            case 14:{const auto v=integer();if(v>FETCHKIT_UNSUPPORTED_METADATA) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);r.error=static_cast<fetchkit_status>(v);break;}
            case 15:r.request.expected_length=integer();break;case 16:r.total=integer();break;
            default:if(flags&1) throw core::Failure(FETCHKIT_UNSUPPORTED_METADATA);break;
        }
        at+=static_cast<std::size_t>(length);
    }
    for (unsigned int i=1;i<=14;++i) if (!seen.count(i)) throw core::Failure(FETCHKIT_INTEGRITY_MISMATCH);
    validate(r); return r;
}
}
