#include "HttpParser.h"
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try { octane::HttpParser::parse(std::string_view(reinterpret_cast<const char*>(data), size)); }
    catch (const octane::HttpParseError&) {}
    return 0;
}
