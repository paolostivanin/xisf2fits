#pragma once

#include <fitsio.h>
#include <atomic>
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
// Set by signal handlers and read by every worker thread.
extern std::atomic<int> interrupted;
static_assert(std::atomic<int>::is_always_lock_free, "signal handlers need a lock-free flag");
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
struct Image {
    std::string id;
    uint64_t width = 0, height = 0;
    bool float32 = false;
    std::vector<uint16_t> pixels;  // UInt16 samples
    std::vector<float> samples;    // Float32 samples, compared bitwise
    std::vector<Keyword> keywords;
    // The first image also holds the container (FILE) properties, after its own.
    std::vector<Property> properties;
    std::string cfa;
    bool checksumVerified = false;
};
// One compressed FITS image extension per XISF image, in document order.
struct Source {
    std::vector<Image> images;
    std::vector<std::string> warnings;
};
// libxml2, CFITSIO and OpenSSL initialize lazily, which is not thread-safe:
// call this once before starting worker threads.
void initializeLibraries();
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
// named: add EXTNAME from the image id, for multi-image sources.
Metadata prepareMetadata(const Image& image, bool bottomUp, bool named = false);
// The serialized records are also the verification contract: ordering, types,
// complete numeric tokens and comments must survive without CFITSIO coercion.
std::vector<std::string> headerRecords(Metadata& metadata);

// records[i] holds the header records of source.images[i].
void writeFits(const fs::path& path, const Source& source, bool bottomUp,
               const std::vector<std::vector<std::string>>& records);
void verifyFits(const fs::path& path, const Source& source, bool bottomUp,
                const std::vector<std::vector<std::string>>& records);
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
