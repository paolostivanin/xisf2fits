#pragma once

#include <fitsio.h>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace x2f {
namespace fs = std::filesystem;

struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
struct Cancelled : std::runtime_error { Cancelled() : std::runtime_error("interrupted") {} };
extern volatile std::sig_atomic_t interrupted;
void checkpoint();
[[noreturn]] void fail(const std::string& message);
void checkFits(int status, const std::string& operation);
std::string upper(std::string value);
std::string trim(const std::string& value);
// Sets lossy when a character is replaced by '?' or control whitespace by a
// space; typographic quotes and dashes have exact ASCII equivalents.
std::string ascii(const std::string& value, bool& lossy);
uint64_t unsignedNumber(const std::string& value, const std::string& context);

struct Keyword { std::string name, value, comment; };
struct Property { std::string scope, id, type, value, comment; };
struct Source {
    uint64_t width = 0, height = 0;
    std::vector<uint16_t> pixels;
    std::vector<Keyword> keywords;
    std::vector<Property> properties;
    std::string cfa;
    std::vector<std::string> warnings;
    bool imageChecksumVerified = false;
};
Source readSource(const fs::path& path, bool dryRun);

enum class ValueType { Undefined, String, Logical, Integer, Real, Complex, Commentary };
struct Card {
    std::string name, value, comment;
    ValueType type = ValueType::Undefined;
};
struct Metadata {
    std::vector<Card> cards;
    std::vector<std::string> warnings;
    // Keywords whose comments did not fit their card, in header order. NINA
    // frames always have some, so this is summarized rather than a warning.
    std::vector<std::string> truncatedComments;
};
bool structuralKeyword(const std::string& name);
bool wcsKeyword(const std::string& name);
ValueType scalarType(const std::string& value);
Card parseKeyword(const Keyword& keyword);
Metadata prepareMetadata(const Source& source, bool bottomUp);
// The serialized records are also the verification contract: ordering, types,
// complete numeric tokens and comments must survive without CFITSIO coercion.
std::vector<std::string> headerRecords(Metadata& metadata);

void writeFits(const fs::path& path, const Source& source, bool bottomUp,
               const std::vector<std::string>& records);
void verifyFits(const fs::path& path, const Source& source, bool bottomUp,
                const std::vector<std::string>& records);
void publish(const fs::path& temporary, const fs::path& output, bool overwrite);
void syncPath(const fs::path& path, bool directory);

class TemporaryOutput {
public:
    explicit TemporaryOutput(const fs::path& output);
    ~TemporaryOutput();
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;
    fs::path file;
private:
    fs::path directory;
    std::vector<fs::path> createdDirectories;
};
}
