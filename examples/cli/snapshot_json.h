#pragma once
#include "fetchkit/fetchkit.h"
#include <ostream>
#include <string>

inline std::string jsonString(const std::string& value) {
    std::string output = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { output += '\\'; output += static_cast<char>(c); }
        else if (c < 32) { output += "\\u00"; output += hex[c >> 4]; output += hex[c & 15]; }
        else output += static_cast<char>(c);
    }
    return output + '"';
}

inline void printSnapshot(std::ostream& output, const fetchkit_task_snapshot& snapshot) {
    output << "{\"task_id\":" << snapshot.task_id << ",\"generation\":" << snapshot.generation
        << ",\"state\":" << snapshot.state << ",\"received_bytes\":" << snapshot.received_bytes
        << ",\"total_known\":" << (snapshot.total_known ? "true" : "false") << ",\"total_bytes\":" << snapshot.total_bytes
        << ",\"http_status\":" << snapshot.http_status << ",\"error\":" << jsonString(fetchkit_status_name(snapshot.error))
        << ",\"platform_code\":" << snapshot.platform_code << ",\"can_resume\":" << (snapshot.can_resume ? "true" : "false")
        << ",\"request_count\":" << snapshot.request_count << ",\"published\":false,\"staging_path\":"
        << jsonString(snapshot.staging_path) << '}';
}
