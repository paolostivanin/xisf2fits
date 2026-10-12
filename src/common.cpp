#include "converter.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace x2f {
volatile std::sig_atomic_t interrupted = 0;
void checkpoint() { if (interrupted) throw Cancelled(); }
[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void checkFits(int status, const std::string& operation) {
    if (!status) return;
    char message[FLEN_STATUS] = {}, detail[FLEN_ERRMSG] = {};
    fits_get_errstatus(status, message);
    std::string error = operation + ": " + message;
    while (fits_read_errmsg(detail)) error += "\n  " + std::string(detail);
    fail(error);
}
std::string upper(std::string value) {
    for (char& c : value) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return value;
}
std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
uint64_t unsignedNumber(const std::string& value, const std::string& context) {
    uint64_t result = 0;
    auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        fail("invalid " + context + ": " + value);
    return result;
}
std::string ascii(const std::string& input, bool& lossy) {
    std::string output;
    for (size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c >= 32 && c <= 126) { output += static_cast<char>(c); ++i; continue; }
        const std::string_view rest(input.data() + i, input.size() - i);
        if (c == '\t' || c == '\r' || c == '\n') { output += ' '; ++i; lossy = true; }
        else if (rest.substr(0, 3) == "\xE2\x80\x98" || rest.substr(0, 3) == "\xE2\x80\x99") {
            output += '\''; i += 3;
        } else if (rest.substr(0, 3) == "\xE2\x80\x9C" || rest.substr(0, 3) == "\xE2\x80\x9D") {
            output += '"'; i += 3;
        } else if (rest.substr(0, 3) == "\xE2\x80\x93" || rest.substr(0, 3) == "\xE2\x80\x94") {
            output += '-'; i += 3;
        } else {
            output += '?'; ++i; lossy = true;
            while (i < input.size() && (static_cast<unsigned char>(input[i]) & 0xC0) == 0x80) ++i;
        }
    }
    return output;
}
}
