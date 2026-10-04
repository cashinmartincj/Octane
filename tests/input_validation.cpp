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

    check(is_valid_time_of_day("08:30"));
    check(is_valid_time_of_day("18:00:00"));
    check(!is_valid_time_of_day("24:00"));
    check(!is_valid_time_of_day("08:60"));
    check(!is_valid_time_of_day("8:30"));

    check(is_valid_iso_timestamp("2026-09-29T06:30:00Z"));
    check(is_valid_iso_timestamp("2026-09-29T06:30:00.123Z"));
    check(is_valid_iso_timestamp("2026-09-29T08:30:00+02:00"));
    check(!is_valid_iso_timestamp("2026-09-29 06:30:00"));
    check(!is_valid_iso_timestamp("2026-09-29T25:00:00Z"));

    check(is_valid_codice_fiscale("RSSMRA85M01H501Z"));
    check(!is_valid_codice_fiscale("RSSMRA85M01H501"));
    check(!is_valid_codice_fiscale("1234567890123456"));

    check(is_valid_partita_iva("04789290402"));
    check(!is_valid_partita_iva("0478929040"));
    check(!is_valid_partita_iva("0478929040A"));

    check(is_safe_pdf_header_and_content("%PDF-1.7\nnormal pdf content"));
    check(!is_safe_pdf_header_and_content("GIF89a"));
    check(!is_safe_pdf_header_and_content("%PDF-1.7\n/JavaScript (alert(1))"));
    check(!is_safe_pdf_header_and_content("%PDF-1.7\n/Launch (malware.exe)"));

    check(is_safe_spreadsheet_text("Regular workout note"));
    check(!is_safe_spreadsheet_text("=CMD('calc')"));
    check(!is_safe_spreadsheet_text("+12345"));
    check(!is_safe_spreadsheet_text("-500"));
    check(!is_safe_spreadsheet_text("@malicious"));

    check(parse_int64("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
    check(!parse_int64("12x"));
    check(parse_uint64("18446744073709551615") == std::numeric_limits<std::uint64_t>::max());
    check(!parse_uint64("-1"));
}
