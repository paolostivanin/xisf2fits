#include "converter.hpp"

#include <algorithm>
#include <cstring>
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
bool warned(const Metadata& metadata, const std::string& text) {
    for (const auto& warning : metadata.warnings) if (warning.find(text) != std::string::npos) return true;
    return false;
}
bool hasCard(const Metadata& metadata, const std::string& name) {
    for (const auto& card : metadata.cards) if (card.name == name) return true;
    return false;
}
Image frame(std::vector<Keyword> keywords, std::vector<Property> properties = {}, std::string cfa = {}) {
    Image image; image.width = 2; image.height = 2;
    image.keywords = std::move(keywords); image.properties = std::move(properties); image.cfa = std::move(cfa);
    return image;
}
std::vector<std::string> recordsFor(const Image& image, bool named) {
    auto metadata = prepareMetadata(image, false, named);
    return headerRecords(metadata);
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

        Source source; source.images.resize(1);
        auto& image = source.images[0]; image.width = 2; image.height = 2;
        image.keywords = {{"FOCUSPOS", "2946", ""}, {"COMMENT", "", "important"}};
        image.properties = {{"IMAGE", "Instrument:Focuser:Position", "Float32", "0", ""}};
        auto metadata = prepareMetadata(image, false);
        require(!metadata.warnings.empty(), "conflicting property reported");
        metadata.cards.push_back({"LONG", std::string(150, '\'') + "&", "comment", ValueType::String});
        auto records = headerRecords(metadata);
        for (const auto& r : records) require(r.size() == 80, "all records have exactly 80 bytes");

        // Typographic quotes and dashes are exact; only lossy replacements are flagged.
        bool lossy = false;
        require(ascii("Bob\xE2\x80\x99s \xE2\x80\x9Cscope\xE2\x80\x9D \xE2\x80\x93 70", lossy) == "Bob's \"scope\" - 70" && !lossy,
                "curly quotes and dashes normalize losslessly");
        for (const auto& text : {"caf\xC3\xA9", "a\tb", "a\nb"}) {
            lossy = false; ascii(text, lossy);
            require(lossy, std::string("lossy replacement flagged: ") + text);
        }

        // Known benign differences between XISF properties and capture keywords.
        const Keyword ninaCreator{"SWCREATE", "'N.I.N.A. 3.1.2.9001 (x64)'", ""};
        const Property focuser{"IMAGE", "Instrument:Focuser:Position", "Float32", "0", ""};
        require(!warned(prepareMetadata(frame({ninaCreator, {"FOCUSPOS", "3200", ""}}, {focuser}), false), "conflicts"),
                "NINA zero focuser property is not a conflict");
        require(warned(prepareMetadata(frame({{"SWCREATE", "'SomeApp 1.0'", ""}, {"FOCUSPOS", "3200", ""}}, {focuser}), false), "conflicts with FOCUSPOS"),
                "zero focuser property from other software is a conflict");
        require(warned(prepareMetadata(frame({ninaCreator, {"FOCUSPOS", "3200", ""}},
                {{"IMAGE", "Instrument:Focuser:Position", "Float32", "3100", ""}}), false), "conflicts with FOCUSPOS"),
                "nonzero NINA focuser property still conflicts");
        const Keyword dateObs{"DATE-OBS", "'2023-11-17T18:15:09.635'", ""};
        require(!warned(prepareMetadata(frame({dateObs}, {{"IMAGE", "Observation:Time:Start", "TimePoint", "2023-11-17T18:15:09.6350Z", ""}}), false), "conflicts"),
                "TimePoint zone designator and fractional zeros are insignificant");
        require(warned(prepareMetadata(frame({dateObs}, {{"IMAGE", "Observation:Time:Start", "TimePoint", "2023-11-17T18:15:09.636Z", ""}}), false), "conflicts with DATE-OBS"),
                "different TimePoint conflicts");
        // PixInsight masters: Float32 values widened to Float64 properties.
        const Property aperture{"IMAGE", "Instrument:Telescope:Aperture", "Float64", "0.07000000029802322", ""};
        const Property focal{"IMAGE", "Instrument:Telescope:FocalLength", "Float64", "0.3499999940395355", ""};
        require(!warned(prepareMetadata(frame({{"APTDIA", "70", ""}, {"FOCALLEN", "349.99999", ""}}, {aperture, focal}), false), "conflicts"),
                "Float32 rounding is not a conflict");
        require(warned(prepareMetadata(frame({{"FOCALLEN", "346.5", ""}}, {focal}), false), "conflicts with FOCALLEN"),
                "a different focal length conflicts");

        // Lossy ASCII replacements are listed once per file, not once per card.
        auto lossyMetadata = prepareMetadata(frame({{"FOCNAME", "'caf\xC3\xA9'", ""}, {"OBJECT", "'M\xC3\xA9'", ""},
                                                    {"TELESCOP", "'Bob\xE2\x80\x99s'", ""}}), false);
        int lossyWarnings = 0;
        for (const auto& w : lossyMetadata.warnings) lossyWarnings += w.find("ASCII-normalized") != std::string::npos;
        require(lossyWarnings == 1 && warned(lossyMetadata, "(lossy): FOCNAME, OBJECT") && !warned(lossyMetadata, "TELESCOP"),
                "one lossy normalization warning naming only lossy keywords");

        // Bayer offsets: tokens, not parity, decide whether they are zero.
        for (const auto& zero : {"0", "+000", "-0"}) {
            auto m = prepareMetadata(frame({{"XBAYROFF", zero, ""}, {"YBAYEROFF", "0", ""}}), false);
            require(warned(m, "omitted zero Bayer offsets without a pattern: XBAYROFF, YBAYEROFF") &&
                    !hasCard(m, "XBAYROFF") && !hasCard(m, "YBAYEROFF") && !hasCard(m, "BAYERPAT"),
                    std::string("zero offsets omitted: ") + zero);
        }
        const std::string xmlCfa = "RGGB";
        for (const auto& offset : {"1", "2", "-2", "123456789012345678901234567890"}) {
            rejects([&] { prepareMetadata(frame({{"YBAYROFF", offset, ""}}), false); }, std::string("nonzero offset without pattern: ") + offset);
            rejects([&] { prepareMetadata(frame({{"YBAYROFF", offset, ""}}, {}, xmlCfa), false); }, std::string("nonzero offset with XML CFA only: ") + offset);
        }
        rejects([&] { prepareMetadata(frame({{"XBAYROFF", "0", ""}, {"XBAYEROFF", "2", ""}}, {}, xmlCfa), false); }, "alias cannot hide a nonzero offset");
        rejects([&] { prepareMetadata(frame({{"XBAYROFF", "0.0", ""}}), false); }, "non-integer offset");
        auto cfaMetadata = prepareMetadata(frame({{"XBAYROFF", "-0", ""}}, {}, xmlCfa), false);
        require(hasCard(cfaMetadata, "BAYERPAT") && !warned(cfaMetadata, "omitted"), "zero offset with XML CFA keeps the pattern");
        for (const auto& empty : {"''", "'   '", ""}) {
            rejects([&] { prepareMetadata(frame({{"BAYERPAT", empty, ""}}, {}, xmlCfa), false); }, std::string("empty BAYERPAT with XML CFA: ") + empty);
            rejects([&] { prepareMetadata(frame({{"BAYERPAT", empty, ""}}), false); }, std::string("empty BAYERPAT: ") + empty);
        }
        auto shifted = prepareMetadata(frame({{"BAYERPAT", "'RGGB'", ""}, {"XBAYROFF", "-2", ""}, {"YBAYROFF", "1", ""}}), false);
        for (const auto& card : shifted.cards) if (card.name == "BAYERPAT") require(card.value == "GBRG", "parity shifts a valid BAYERPAT");

        // Truncated comments are recorded once per keyword, in header order.
        auto truncation = prepareMetadata(frame({{"OBJECT", "'" + std::string(60, 'x') + "'", std::string(40, 'a')},
                                                 {"SHORT", "1", "fits"},
                                                 {"FULL", "'" + std::string(68, 'f') + "'", "dropped"},
                                                 {"LONGSTR", "'" + std::string(150, 's') + "'", std::string(60, 'c')}}), false);
        headerRecords(truncation);
        require((truncation.truncatedComments == std::vector<std::string>{"OBJECT", "FULL", "LONGSTR"}), "truncated comments listed in header order");
        require(!warned(truncation, "truncated"), "truncation is a note, not a warning");
        auto intact = prepareMetadata(frame({{"SHORT", "1", "fits"}}), false);
        headerRecords(intact);
        require(intact.truncatedComments.empty(), "no truncation recorded when every comment fits");

        // Extensions of multi-image files are named after their image ids.
        auto master = frame({{"SHORT", "1", ""}}); master.id = "rejection_low";
        require(!hasCard(prepareMetadata(master, false), "EXTNAME"), "single images get no EXTNAME");
        auto extname = prepareMetadata(master, false, true);
        require(hasCard(extname, "EXTNAME") && extname.cards[1].value == "rejection_low", "image id becomes EXTNAME");
        master.keywords.push_back({"extname", "'SCI'", ""});
        for (const auto& card : prepareMetadata(master, false, true).cards)
            require(card.name != "EXTNAME" || card.value == "SCI", "an existing EXTNAME is kept");

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

        image.pixels = {0, 32768, 1, 65535};
        const auto valid = root / "verify.fits.fz";
        writeFits(valid, source, false, {records});
        verifyFits(valid, source, false, {records});
        image.pixels[0] = 1;
        rejects([&] { verifyFits(valid, source, false, {records}); }, "pixel verification rejects altered source");
        image.pixels[0] = 0;
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
        rejects([&] { verifyFits(valid, source, false, {records}); }, "reject null interpretation with valid checksums");

        // Float32 extensions are lossless: signed zero, denormals, NaN payloads
        // and infinities survive bit for bit, and a null check cannot hide them.
        const uint32_t bits[6] = {0x80000000u, 0x00000001u, 0x7fc12345u, 0x7f800000u, 0xff800000u, 0x3ee28f5cu};
        Source floats; floats.images.resize(2);
        for (size_t i = 0; i < 2; ++i) {
            auto& f = floats.images[i];
            f.id = i ? "rejection_low" : "integration"; f.width = 3; f.height = 2; f.float32 = true;
            f.samples.resize(6);
            std::memcpy(f.samples.data(), bits, sizeof bits);
            if (i) std::reverse(f.samples.begin(), f.samples.end());
        }
        const std::vector<std::vector<std::string>> floatRecords{recordsFor(floats.images[0], true), recordsFor(floats.images[1], true)};
        const auto lossless = root / "float.fits.fz";
        for (bool bottomUp : {false, true}) {
            fs::remove(lossless);
            writeFits(lossless, floats, bottomUp, floatRecords);
            verifyFits(lossless, floats, bottomUp, floatRecords);
        }
        rejects([&] { verifyFits(lossless, floats, false, floatRecords); }, "row order is verified on float images");
        auto zero = floats;
        uint32_t positive = 0; std::memcpy(&zero.images[1].samples[5], &positive, sizeof positive);
        rejects([&] { verifyFits(lossless, zero, true, floatRecords); }, "-0.0 and +0.0 differ in the second extension");
        Source fewer; fewer.images = {floats.images[0]};
        rejects([&] { verifyFits(lossless, fewer, true, {floatRecords[0]}); }, "extension count is verified");
        rejects([&] { verifyFits(valid, floats, false, floatRecords); }, "UInt16 output does not verify as Float32");
        std::cout << "unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
