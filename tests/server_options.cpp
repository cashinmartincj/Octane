#include "transport/TcpServer.h"

#include <cstdlib>
#include <stdexcept>

namespace {

void check(bool condition) {
    if (!condition) std::abort();
}

bool rejects_address(const char* address) {
    octane::transport::TcpServerOptions options;
    options.bind_address = address;
    try {
        options.validate();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    octane::transport::TcpServerOptions options;
    options.validate();

    options.bind_address = "127.0.0.1";
    options.validate();

    check(rejects_address(""));
    check(rejects_address("localhost"));
    check(rejects_address("127.0.0.1:8080"));
    check(rejects_address("999.0.0.1"));
}
