#include "converter.hpp"

#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace x2f;
namespace {
void require(bool condition, const std::string& message) { if (!condition) fail("test failed: " + message); }
template<class F> void rejects(F&& action, const std::string& message) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    require(threw, message);
}
std::string contents(const fs::path& p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }
void put(const fs::path& p, const std::string& s) { std::ofstream f(p); f << s; }
}

int main() {
    try {
        for (const auto& text : {"IC 1805", "TecnoSky AG70", "+61 27 00", "2023-09-09T00:17:01.721", "1/2", "5foo"})
            require(scalarType(text) == ValueType::String, text);
        require(scalarType("T") == ValueType::Logical, "logical type");
        require(scalarType("-12") == ValueType::Integer, "integer type");
        require(scalarType("1.23D-4") == ValueType::Real, "D exponent");
        require(scalarType("(1.0, -2)") == ValueType::Complex, "complex type");
        require(parseKeyword({"OBJECT", "1234", ""}).type == ValueType::String, "numeric object name");
        require(parseKeyword({"FILTER", "T", ""}).type == ValueType::String, "logical filter name");
        for (const auto& name : {"BLANK", "TFIELDS", "THEAP", "TZERO1", "ZBLANK", "ZNAXIS2", "ZQUANTIZ"})
            require(structuralKeyword(name), name);
        for (const auto& name : {"EXTNAME", "NAXISFOO", "TTYPEX", "TSCALX", "ZVALUE"})
            require(!structuralKeyword(name), name);
        require(wcsKeyword("CRPIX2A") && wcsKeyword("PC3_2") && wcsKeyword("A_ORDER"), "WCS recognition");
        require(!wcsKeyword("RA") && !wcsKeyword("EQUINOX"), "pointing hints do not prohibit flips");

        Source source; source.width = 2; source.height = 2;
        source.keywords = {{"FOCUSPOS", "2946", ""}, {"COMMENT", "", "important"}};
        source.properties = {{"IMAGE", "Instrument:Focuser:Position", "Float32", "0", ""}};
        auto metadata = prepareMetadata(source, false);
        require(!metadata.warnings.empty(), "conflicting property reported");
        metadata.cards.push_back({"LONG", std::string(150, '\'') + "&", "comment", ValueType::String});
        auto records = headerRecords(metadata);
        for (const auto& r : records) require(r.size() == 80, "all records have exactly 80 bytes");

        std::string pattern = (fs::temp_directory_path() / "xisf2fits-unit-XXXXXX").string();
        require(::mkdtemp(pattern.data()), "temporary test directory");
        const fs::path root = pattern;
        struct Cleanup { fs::path root; ~Cleanup() { std::error_code e; fs::remove_all(root, e); } } cleanup{root};
        const auto output = root / "out.fits.fz";
        put(output, "existing");
        {
            TemporaryOutput temp(output); put(temp.file, "replacement");
            rejects([&] { publish(temp.file, output, false); }, "no-replace publication rejects existing target");
            require(contents(output) == "existing", "race cannot overwrite target");
            publish(temp.file, output, true);
            require(contents(output) == "replacement", "explicit overwrite replaces target");
        }
        {
            TemporaryOutput temp(root / "new.fits.fz"); put(temp.file, "complete");
            publish(temp.file, root / "new.fits.fz", false);
            require(contents(root / "new.fits.fz") == "complete", "no-replace publication succeeds");
        }
        {
            TemporaryOutput temp(root / "nested" / "deeper" / "out.fits.fz");
            put(temp.file, "partial");
            interrupted = SIGINT;
            rejects([&] { publish(temp.file, output, true); }, "cancellation before publication");
            interrupted = 0;
            require(contents(output) == "replacement", "cancelled publish preserves previous file");
        }
        require(!fs::exists(root / "nested"), "failed conversion removes created empty directories");
        for (const auto& entry : fs::directory_iterator(root))
            require(entry.path().filename().string().rfind(".xisf2fits-", 0) != 0, "temporary directory cleaned up");

        source.pixels = {0, 32768, 1, 65535};
        const auto valid = root / "verify.fits.fz";
        writeFits(valid, source, false, records);
        verifyFits(valid, source, false, records);
        source.pixels[0] = 1;
        rejects([&] { verifyFits(valid, source, false, records); }, "pixel verification rejects altered source");
        source.pixels[0] = 0;
        // Recompute checksums after a malicious BLANK insertion: checksum-only
        // verification must not conceal null interpretation of real pixels.
        fitsfile* f = nullptr; int status = 0, type = 0;
        fits_open_diskfile(&f, valid.c_str(), READWRITE, &status);
        fits_movabs_hdu(f, 2, &type, &status);
        long blank = 0;
        fits_write_key(f, TLONG, "BLANK", &blank, nullptr, &status);
        fits_write_chksum(f, &status);
        fits_close_file(f, &status);
        checkFits(status, "create null interpretation regression");
        rejects([&] { verifyFits(valid, source, false, records); }, "reject null interpretation with valid checksums");
        std::cout << "unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
