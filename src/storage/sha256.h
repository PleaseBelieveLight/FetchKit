#pragma once
#include "picosha2/picosha2.h"
#include <array>
#include <cstdint>
#include <string>

namespace fetchkit::storage {
// Hex formatting is kept outside the hash implementation to avoid pulling the
// stream/locale formatting runtime into small SDK integrations.
inline std::string hashHex(const picosha2::hash256_one_by_one& hash) {
    std::array<std::uint8_t,32> bytes{};
    hash.get_hash_bytes(bytes.begin(),bytes.end());
    constexpr char digits[]="0123456789abcdef";
    std::string result(64,'0');
    for(std::size_t i=0;i<bytes.size();++i) {
        result[2*i]=digits[bytes[i]>>4];result[2*i+1]=digits[bytes[i]&15];
    }
    return result;
}
}
