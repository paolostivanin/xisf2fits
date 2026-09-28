#include "converter.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <system_error>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace x2f {
namespace {
class FitsFile {
public:
    fitsfile* ptr = nullptr;
    FitsFile() = default;
    FitsFile(const FitsFile&) = delete;
    FitsFile& operator=(const FitsFile&) = delete;
    ~FitsFile() { if (ptr) { int status = 0; fits_close_file(ptr, &status); } }
    void close() {
        int status = 0;
        fits_close_file(ptr, &status); ptr = nullptr;
        checkFits(status, "close FITS file");
    }
};
[[noreturn]] void ioError(const std::string& operation, const fs::path& path) {
    throw std::system_error(errno, std::generic_category(), operation + ": " + path.string());
}

// CFITSIO's type inference accepts prefixes such as "TecnoSky" as logicals.
// Check the entire physical value field before using any typed read routines.
void validateRecord(const std::string& record) {
    if (record.size() != 80 || !std::all_of(record.begin(), record.end(), [](unsigned char c) { return c >= 32 && c <= 126; }))
        fail("non-ASCII or invalid FITS header record");
    const auto name = trim(record.substr(0, 8));
    if (!name.empty() && (record.substr(0, 8) != name + std::string(8 - name.size(), ' ') ||
        !std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        }))) fail("invalid FITS keyword name: " + name);
    size_t start = 10;
    if (name.empty() || name == "COMMENT" || name == "HISTORY" || name == "END") return;
    if (name == "HIERARCH") {
        auto equals = record.find('=', 9);
        if (equals == std::string::npos || trim(record.substr(9, equals - 9)).empty()) fail("invalid HIERARCH header record");
        start = equals + 1;
    } else if (name == "CONTINUE") {
        if (record.substr(8, 2) != "  ") fail("invalid CONTINUE header record");
    } else if (record.substr(8, 2) != "= ") fail("invalid FITS value indicator: " + name);
    auto field = trim(record.substr(start));
    if (!field.empty() && field.front() == '\'') {
        bool closed = false;
        for (size_t i = 1; i < field.size(); ++i) {
            if (field[i] != '\'') continue;
            if (i + 1 < field.size() && field[i + 1] == '\'') { ++i; continue; }
            auto tail = trim(field.substr(i + 1));
            if (!tail.empty() && tail.front() != '/') fail("text after FITS string: " + name);
            closed = true; break;
        }
        if (!closed) fail("unterminated FITS string: " + name);
    } else {
        const auto token = trim(field.substr(0, field.find('/')));
        if (name == "CONTINUE" || scalarType(token) == ValueType::String) fail("invalid unquoted FITS value: " + name);
    }
}
std::vector<std::string> readRecords(fitsfile* ptr) {
    int status = 0, count = 0, free = 0;
    fits_get_hdrspace(ptr, &count, &free, &status);
    checkFits(status, "read FITS header size");
    std::vector<std::string> records;
    std::set<std::string> seen;
    for (int i = 1; i <= count; ++i) {
        char record[FLEN_CARD] = {};
        fits_read_record(ptr, i, record, &status);
        checkFits(status, "read FITS header record");
        std::string padded(record); padded.resize(80, ' ');
        validateRecord(padded);
        char key[FLEN_KEYWORD] = {}; int length = 0;
        fits_get_keyname(record, key, &length, &status);
        checkFits(status, "read header keyword name");
        std::string name(key);
        if (!name.empty() && name != "COMMENT" && name != "HISTORY" && name != "CONTINUE" &&
            !seen.insert(name).second) fail("duplicate FITS keyword: " + name);
        records.push_back(std::move(padded));
    }
    return records;
}
}

void writeFits(const fs::path& path, const Source& source, bool bottomUp, const std::vector<std::string>& records) {
    FitsFile file;
    int status = 0;
    fits_create_diskfile(&file.ptr, fs::absolute(path).c_str(), &status);
    checkFits(status, "create temporary FITS file");
    fits_create_img(file.ptr, BYTE_IMG, 0, nullptr, &status);
    fits_set_compression_type(file.ptr, RICE_1, &status);
    long axes[2] = {static_cast<long>(source.width), static_cast<long>(source.height)};
    fits_create_img(file.ptr, USHORT_IMG, 2, axes, &status);
    checkFits(status, "create compressed image extension");
    for (const auto& record : records) {
        validateRecord(record);
        // CFITSIO may supply a default extension name. The source label is
        // descriptive metadata, not a structural keyword to discard.
        if (record.substr(0, 8) == "EXTNAME ") {
            fits_delete_key(file.ptr, "EXTNAME", &status);
            if (status == KEY_NO_EXIST) { status = 0; fits_clear_errmsg(); }
        }
        fits_write_record(file.ptr, record.c_str(), &status);
        checkFits(status, "write FITS metadata");
    }
    const size_t width = static_cast<size_t>(source.width), height = static_cast<size_t>(source.height);
    if (source.pixels.size() != width * height) fail("internal decoded image size mismatch");
    for (size_t y = 0; y < height; ++y) {
        checkpoint();
        const size_t sourceY = bottomUp ? height - y - 1 : y;
        fits_write_img(file.ptr, TUSHORT, static_cast<LONGLONG>(y * width + 1), static_cast<LONGLONG>(width),
                       const_cast<uint16_t*>(source.pixels.data() + sourceY * width), &status);
        checkFits(status, "write compressed image row");
    }
    fits_write_chksum(file.ptr, &status);
    int type = 0;
    fits_movabs_hdu(file.ptr, 1, &type, &status);
    fits_write_chksum(file.ptr, &status);
    checkFits(status, "write FITS checksums");
    file.close();
}

void verifyFits(const fs::path& path, const Source& source, bool bottomUp, const std::vector<std::string>& records) {
    FitsFile file;
    int status = 0;
    fits_open_diskfile(&file.ptr, fs::absolute(path).c_str(), READONLY, &status);
    checkFits(status, "open FITS for verification");
    int count = 0, type = 0;
    fits_get_num_hdus(file.ptr, &count, &status);
    checkFits(status, "read HDU count");
    if (count != 2) fail("expected exactly two FITS HDUs");
    std::vector<std::string> imageRecords;
    for (int hdu = 1; hdu <= count; ++hdu) {
        checkpoint();
        fits_movabs_hdu(file.ptr, hdu, &type, &status);
        checkFits(status, "select FITS HDU");
        auto actual = readRecords(file.ptr);
        if (hdu == 2) imageRecords = std::move(actual);
        else {
            int axes = -1;
            fits_get_img_dim(file.ptr, &axes, &status);
            checkFits(status, "read primary HDU dimensions");
            if (type != IMAGE_HDU || axes != 0) fail("primary HDU is not empty");
        }
        int data = 0, header = 0;
        fits_verify_chksum(file.ptr, &data, &header, &status);
        checkFits(status, "verify HDU checksum");
        if (data != 1 || header != 1) fail("FITS checksum missing or invalid in HDU " + std::to_string(hdu));
    }
    if (!fits_is_compressed_image(file.ptr, &status)) fail("output is not a compressed FITS image");
    int bitpix = 0, naxis = 0, equivalent = 0;
    long axes[2] = {};
    fits_get_img_param(file.ptr, 2, &bitpix, &naxis, axes, &status);
    fits_get_img_equivtype(file.ptr, &equivalent, &status);
    char codec[FLEN_VALUE] = {};
    fits_read_key(file.ptr, TSTRING, "ZCMPTYPE", codec, nullptr, &status);
    checkFits(status, "read compressed image parameters");
    if (naxis != 2 || axes[0] != static_cast<long>(source.width) || axes[1] != static_cast<long>(source.height))
        fail("output dimensions differ from XISF");
    if (bitpix != SHORT_IMG || equivalent != USHORT_IMG || std::string(codec) != "RICE_1")
        fail("output is not Rice-compressed unsigned 16-bit FITS");
    double zero = 0, scale = 1;
    fits_read_key(file.ptr, TDOUBLE, "BZERO", &zero, nullptr, &status);
    checkFits(status, "read unsigned pixel offset");
    fits_read_key(file.ptr, TDOUBLE, "BSCALE", &scale, nullptr, &status);
    if (status == KEY_NO_EXIST) { status = 0; fits_clear_errmsg(); }
    checkFits(status, "read pixel scale");
    if (zero != 32768 || scale != 1) fail("unexpected FITS pixel scaling");

    // Locate each canonical metadata record in order. Exact token comparison
    // catches type changes and precision loss even if CFITSIO coerces values.
    size_t pos = 0;
    for (const auto& expected : records) {
        while (pos < imageRecords.size() && imageRecords[pos] != expected) ++pos;
        if (pos == imageRecords.size()) fail("FITS metadata differs from source: " + trim(expected));
        ++pos;
    }
    for (const auto& record : imageRecords) {
        char key[FLEN_KEYWORD] = {}; int length = 0;
        char raw[FLEN_CARD] = {}; std::memcpy(raw, record.data(), 80);
        fits_get_keyname(raw, key, &length, &status);
        checkFits(status, "read header keyword name");
        std::string name(key);
        if (name.empty() || name == "COMMENT" || name == "HISTORY" || name == "CONTINUE") continue;
        if (name == "BLANK" || name == "ZBLANK" || name == "ZSCALE" || name == "ZZERO" ||
            (structuralKeyword(name) && (name.rfind("TSCAL", 0) == 0 || name.rfind("TZERO", 0) == 0 || name.rfind("TNULL", 0) == 0)))
            fail("unexpected null or table scaling keyword: " + name);
    }
    const size_t width = static_cast<size_t>(source.width), height = static_cast<size_t>(source.height);
    if (source.pixels.size() != width * height) fail("source pixels unavailable for verification");
    std::vector<uint16_t> row(width);
    std::vector<char> nulls(width);
    for (size_t y = 0; y < height; ++y) {
        checkpoint();
        int anyNull = 0;
        fits_read_imgnull(file.ptr, TUSHORT, static_cast<LONGLONG>(y * width + 1), static_cast<LONGLONG>(width),
                          row.data(), nulls.data(), &anyNull, &status);
        checkFits(status, "read output pixels and null mask");
        if (anyNull || std::any_of(nulls.begin(), nulls.end(), [](char c) { return c != 0; })) fail("unexpected null pixel in output");
        size_t sourceY = bottomUp ? height - y - 1 : y;
        for (size_t x = 0; x < width; ++x)
            if (row[x] != source.pixels[sourceY * width + x])
                fail("pixel mismatch at output x=" + std::to_string(x) + ", y=" + std::to_string(y));
    }
    file.close();
}

void syncPath(const fs::path& path, bool directory) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | (directory ? O_DIRECTORY : 0));
    if (fd < 0) ioError("open for synchronization", path);
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    int error = errno;
    int closed = ::close(fd);
    if (result < 0) { errno = error; ioError("synchronize", path); }
    if (closed < 0) ioError("close synchronized file", path);
}

void publish(const fs::path& temporary, const fs::path& destination, bool overwrite) {
    const auto output = fs::absolute(destination);
    syncPath(temporary, false);
    checkpoint();
    if (overwrite) fs::rename(temporary, output);
    else {
        bool renamed = false;
#if defined(__linux__) && defined(SYS_renameat2)
        if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, output.c_str(), RENAME_NOREPLACE) == 0) renamed = true;
        else if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) ioError("publish without overwrite", output);
#endif
        if (!renamed) {
            std::error_code error;
            fs::create_hard_link(temporary, output, error);
            if (error) throw std::system_error(error, "safe no-replace publication unavailable or destination exists: " + output.string());
            // The output is published; TemporaryOutput retries removing a leftover link.
            fs::remove(temporary, error);
        }
    }
    // Once renamed, the target contains the complete file. A subsequent sync
    // error is reported; never delete or replace that valid target on failure.
    syncPath(output.parent_path(), true);
    syncPath(temporary.parent_path(), true);
}

TemporaryOutput::TemporaryOutput(const fs::path& output) {
    const auto parent = fs::absolute(output).parent_path();
    try {
        std::vector<fs::path> missing;
        for (auto p = parent; !fs::exists(p); p = p.parent_path()) missing.push_back(p);
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            if (fs::create_directory(*it)) createdDirectories.push_back(*it);
            syncPath(it->parent_path(), true);
        }
        std::string pattern = (parent / ".xisf2fits-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) ioError("create private temporary directory", parent);
        directory = pattern;
        file = directory / "image.tmp";
    } catch (...) {
        std::error_code ignored;
        for (auto it = createdDirectories.rbegin(); it != createdDirectories.rend(); ++it) fs::remove(*it, ignored);
        throw;
    }
}
TemporaryOutput::~TemporaryOutput() {
    std::error_code ignored;
    fs::remove(file, ignored);
    fs::remove(directory, ignored);
    for (auto it = createdDirectories.rbegin(); it != createdDirectories.rend(); ++it) fs::remove(*it, ignored);
}
}
