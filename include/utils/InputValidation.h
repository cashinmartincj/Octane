#pragma once

/**
 * @file InputValidation.h
 * @brief Zero-allocation, high-performance input validation utilities for Octane applications.
 *
 * @details
 * This library provides zero-allocation sanity checks and data validators designed
 * specifically for high-throughput HTTP request processing in C++20. Every validator
 * operates on non-owning `std::string_view` buffers directly over incoming network bytes,
 * avoiding temporary heap allocations or regex engine overhead.
 *
 * @section usage How to Import & Use in Backend Handlers:
 * Include this header in your controller, service, or request validation DTO files:
 * @code
 * #include "utils/InputValidation.h"
 *
 * // Optional convenience using-directive:
 * using namespace octane::validation;
 *
 * class UserRegistrationController : public octane::routes::Post<UserRegistrationController> {
 * public:
 *     void handle(const octane::HttpRequest& req, octane::HttpResponse& res) {
 *         std::string_view email = req.query("email");
 *         if (!octane::validation::is_valid_email(email)) {
 *             res.status(400).json(R"({"error":"invalid_email"})");
 *             return;
 *         }
 *         // proceed with validated input...
 *     }
 * };
 * @endcode
 *
 * @important SQL Safety Invariant:
 * These validation functions verify that inputs conform to expected format invariants.
 * They DO NOT escape or sanitize SQL strings. All inputs destined for database queries
 * MUST be passed as positional bind parameters ($1, $2) using libpqxx.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace octane::validation {

/**
 * @struct TextRules
 * @brief Configurable boundary rules for general text and string validation.
 */
struct TextRules {
    std::size_t min_bytes{0};                      ///< Minimum required byte length (inclusive)
    std::size_t max_bytes{4096};                   ///< Maximum permitted byte length (inclusive)
    bool allow_newlines{false};                    ///< If true, allows CR (\\r) and LF (\\n) characters
    bool allow_tabs{false};                        ///< If true, allows tab (\\t) characters
    bool allow_leading_or_trailing_space{false};   ///< If false, rejects text with leading or trailing spaces
};

/**
 * @brief Validates that a string is strictly valid UTF-8.
 * @details Rejects non-shortest forms, surrogate halves (0xD800-0xDFFF), codepoints
 * above 0x10FFFF, and truncated multi-byte sequences.
 * @param value Byte sequence to validate.
 * @return true if string contains only valid UTF-8 sequences, false otherwise.
 */
[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;

/**
 * @brief Validates human-readable text according to length, control characters, and whitespace rules.
 * @param value Text string to validate.
 * @param rules Configuration constraints (length bounds, newlines, tabs, space trimming).
 * @return true if text conforms to all rules and is valid UTF-8, false otherwise.
 */
[[nodiscard]] bool is_valid_text(std::string_view value,
                                 TextRules rules = {}) noexcept;

/**
 * @brief Validates an email address according to RFC 5322 syntax rules.
 * @details Checks for valid local part characters, single '@' delimiter, max length 254 bytes,
 * domain label boundaries (1-63 chars), and absence of consecutive dots.
 * @param value Candidate email address string.
 * @return true if email conforms to RFC address syntax, false otherwise.
 */
[[nodiscard]] bool is_valid_email(std::string_view value) noexcept;

/**
 * @brief Validates an account username.
 * @details Permits ASCII alphanumeric characters and safe symbols: `_`, `.`, `@`, `+`, `-`.
 * @param value Candidate username.
 * @param min_bytes Minimum allowable length (default: 1).
 * @param max_bytes Maximum allowable length (default: 150).
 * @return true if username contains only allowed characters within length bounds.
 */
[[nodiscard]] bool is_valid_username(std::string_view value,
                                     std::size_t min_bytes = 1,
                                     std::size_t max_bytes = 150) noexcept;

/**
 * @brief Validates password input bounds before expensive hashing algorithms.
 * @details Enforces byte length limits and rejects embedded null bytes or line breaks
 * to protect Argon2id/PBKDF2 hashing workers from denial-of-service or poisoning.
 * @param value Candidate plaintext password.
 * @param min_bytes Minimum allowable length (default: 8).
 * @param max_bytes Maximum allowable length (default: 1024).
 * @return true if password conforms to length and byte safety invariants.
 */
[[nodiscard]] bool is_valid_password_input(std::string_view value,
                                           std::size_t min_bytes = 8,
                                           std::size_t max_bytes = 1024) noexcept;

/**
 * @brief Validates a URL/tenant slug (e.g. gym identifier in `*.fitops.it`).
 * @details Must start and end with an alphanumeric character; interior may contain `-` or `_`.
 * Path traversal (`/`, `..`) is strictly rejected.
 * @param value Candidate slug string.
 * @param max_bytes Maximum allowable length (default: 100).
 * @return true if slug is safe for URL subdomains and route identifiers.
 */
[[nodiscard]] bool is_valid_slug(std::string_view value,
                                 std::size_t max_bytes = 100) noexcept;

/**
 * @brief Validates standard 36-character UUID format (8-4-4-4-12 hex).
 * @details Example: `550e8400-e29b-41d4-a716-446655440000`. Case-insensitive.
 * @param value Candidate UUID string.
 * @return true if string matches standard UUID hyphenation and hexadecimal pattern.
 */
[[nodiscard]] bool is_valid_uuid(std::string_view value) noexcept;

/**
 * @brief Validates an ISO 8601 calendar date (`YYYY-MM-DD`).
 * @details Enforces valid days per month, leap year February 29th calculations,
 * and valid year bounds (non-zero).
 * @param value Date string to validate.
 * @return true if date is a real calendar date, false otherwise.
 */
[[nodiscard]] bool is_valid_iso_date(std::string_view value) noexcept;

/**
 * @brief Validates fixed-point decimal and currency values.
 * @param value Numeric string (e.g., `"123.45"` or `"-50.00"`).
 * @param max_integer_digits Maximum allowed digits before the decimal point.
 * @param max_fraction_digits Maximum allowed digits after the decimal point.
 * @param allow_negative Whether leading minus sign `'-'` is accepted.
 * @return true if value matches exact decimal bounds without scientific notation.
 */
[[nodiscard]] bool is_valid_decimal(std::string_view value,
                                    std::size_t max_integer_digits,
                                    std::size_t max_fraction_digits,
                                    bool allow_negative = false) noexcept;

/**
 * @brief Validates a 7-character hexadecimal web colour code (`#RRGGBB`).
 * @param value Colour string (e.g., `"#FF5733"`).
 * @return true if string begins with `#` followed by 6 valid hex characters.
 */
[[nodiscard]] bool is_valid_hex_colour(std::string_view value) noexcept;

/**
 * @brief Validates an international telephone number.
 * @details Allows optional leading `+`, digits, spaces, hyphens, and parentheses.
 * Ensures total digit count is between 5 and 15 (ITU-T E.164 standard).
 * @param value Phone number string.
 * @param max_bytes Maximum string buffer length (default: 32).
 * @return true if phone number conforms to formatting and length limits.
 */
[[nodiscard]] bool is_valid_phone(std::string_view value,
                                  std::size_t max_bytes = 32) noexcept;

/**
 * @brief Validates that a filename is safe for filesystem and storage operations.
 * @details Prohibits directory traversal (`/`, `\\`, `..`), control characters, colons,
 * reserved names (`.` or `..`), and invalid UTF-8.
 * @param value Candidate filename (e.g., `"medical-report.pdf"`).
 * @param max_bytes Maximum length in bytes (default: 255).
 * @return true if filename cannot cause directory traversal or path injection.
 */
[[nodiscard]] bool is_safe_filename(std::string_view value,
                                    std::size_t max_bytes = 255) noexcept;

/**
 * @brief Validates an ISO 8601 UTC timestamp instant.
 * @details Supports formats:
 * - `YYYY-MM-DDTHH:MM:SSZ`
 * - `YYYY-MM-DDTHH:MM:SS.fffZ` (with subsecond fractions)
 * - `YYYY-MM-DDTHH:MM:SS+HH:MM` (with timezone offset)
 * Required under Architecture Decision D-044 for all booking and system instants.
 * @param value Timestamp string.
 * @return true if timestamp conforms to valid ISO 8601 instant syntax.
 */
[[nodiscard]] bool is_valid_iso_timestamp(std::string_view value) noexcept;

/**
 * @brief Validates 24-hour time of day (`HH:MM` or `HH:MM:SS`).
 * @details Used for gym working hours, schedule slots, and trainer availability.
 * Hours must be 00-23, minutes 00-59, seconds 00-59.
 * @param value Time string (e.g., `"08:30"` or `"18:00:00"`).
 * @return true if time is a valid 24-hour clock time.
 */
[[nodiscard]] bool is_valid_time_of_day(std::string_view value) noexcept;

/**
 * @brief Validates Italian Codice Fiscale (Tax Code).
 * @details Validates 16-character alphanumeric pattern:
 * 6 letters (surname/name), 2 digits (year), 1 letter (month), 2 digits (day/gender),
 * 1 letter (cadastral code prefix), 3 digits (cadastral code), 1 letter (check character).
 * @param value Candidate fiscal code string.
 * @return true if format strictly matches Italian Codice Fiscale structure.
 */
[[nodiscard]] bool is_valid_codice_fiscale(std::string_view value) noexcept;

/**
 * @brief Validates Italian Partita IVA (VAT Number).
 * @details Enforces exactly 11 numeric ASCII digits. Used for gym billing settings
 * and FattureInCloud corporate configuration.
 * @param value Candidate VAT number string.
 * @return true if string consists of exactly 11 numeric digits.
 */
[[nodiscard]] bool is_valid_partita_iva(std::string_view value) noexcept;

/**
 * @brief Sanity checks uploaded medical certificate PDF byte buffers for malware vectors.
 * @details
 * 1. Validates magic bytes (`%PDF-`).
 * 2. Enforces maximum size bounds (default: 10 MiB).
 * 3. Scans for known PDF active exploit dictionaries and executable tokens:
 *    `/JavaScript`, `/JS`, `/Launch`, `/EmbeddedFiles`, `/RichMedia`.
 * Used under Architecture Decisions D-020 and D-063 before persisting blobs to private storage.
 * @param bytes Raw binary buffer of the uploaded file.
 * @param max_bytes Maximum permitted file size in bytes (default: 10 MiB).
 * @return true if file header is a valid PDF and contains no active executable vectors.
 */
[[nodiscard]] bool is_safe_pdf_header_and_content(std::string_view bytes,
                                                  std::size_t max_bytes = 10 * 1024 * 1024) noexcept;

/**
 * @brief Defends against CSV / Spreadsheet Formula Injection (CWE-1236).
 * @details Rejects strings that begin with formula trigger characters (`=`, `+`, `-`, `@`,
 * tab `\\t`, carriage return `\\r`) or contain null bytes (`\\0`), preventing remote command
 * execution when exported XLSX/CSV spreadsheets are opened in Microsoft Excel or LibreOffice.
 * Used under Architecture Decision D-028 for user-controlled workout notes and export fields.
 * @param value Text to be placed into a spreadsheet cell.
 * @return true if text cannot trigger formula execution, false if it begins with an injection trigger.
 */
[[nodiscard]] bool is_safe_spreadsheet_text(std::string_view value) noexcept;

/**
 * @brief Strictly parses a 64-bit signed integer.
 * @details Prohibits leading `+`, whitespace, or trailing non-numeric characters.
 * @param value Numeric string.
 * @return Parsed std::int64_t on success, std::nullopt on overflow or format error.
 */
[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view value) noexcept;

/**
 * @brief Strictly parses a 64-bit unsigned integer.
 * @details Prohibits signs (`+`, `-`), whitespace, or trailing non-numeric characters.
 * @param value Numeric string.
 * @return Parsed std::uint64_t on success, std::nullopt on overflow or format error.
 */
[[nodiscard]] std::optional<std::uint64_t> parse_uint64(std::string_view value) noexcept;

} // namespace octane::validation
