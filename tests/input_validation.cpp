#include "utils/InputValidation.h"

#include <cstdlib>
#include <limits>
#include <string>

namespace {

void check(bool condition) {
    if (!condition) std::abort();
}

} // namespace

int main() {
    using namespace octane::validation;

    check(is_valid_utf8("Cashin Martin"));
    check(is_valid_utf8("Citt\xc3\xa0"));
    check(!is_valid_utf8(std::string("\xc0\xaf", 2)));
    check(!is_valid_utf8(std::string("\xed\xa0\x80", 3)));

    check(is_valid_email("cashin.martin+gym@fitops.it"));
    check(!is_valid_email("cashin@@fitops.it"));
    check(!is_valid_email("cashin@-fitops.it"));
    check(!is_valid_email("cashin..martin@fitops.it"));

    check(is_valid_username("trainer_123@example.com"));
    check(!is_valid_username("trainer name"));
    check(!is_valid_username(""));
    check(is_valid_slug("gym-a_01"));
    check(!is_valid_slug("../gym-a"));

    check(is_valid_text("Robert'); DROP TABLE customers;--", {1, 128, false, false, false}));
    check(!is_valid_text(" leading", {1, 128, false, false, false}));
    check(!is_valid_text(std::string("nul\0byte", 8), {1, 128, false, false, true}));
    check(is_valid_password_input("long password with spaces"));
    check(!is_valid_password_input("short", 8));

    check(is_valid_uuid("550e8400-e29b-41d4-a716-446655440000"));
    check(!is_valid_uuid("550e8400-e29b-41d4-a716"));
    check(is_valid_iso_date("2024-02-29"));
    check(!is_valid_iso_date("2023-02-29"));
    check(is_valid_decimal("-123.45", 3, 2, true));
    check(!is_valid_decimal("123.456", 3, 2, true));
    check(!is_valid_decimal("1e6", 7, 2, false));
    check(is_valid_hex_colour("#a1B2c3"));
    check(is_valid_phone("+39 333-123-4567"));
    check(is_safe_filename("medical-certificate.pdf"));
    check(!is_safe_filename("../certificate.pdf"));

    check(parse_int64("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
    check(!parse_int64("12x"));
    check(parse_uint64("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
    check(!parse_uint64("-1"));
}
