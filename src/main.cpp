#include "converter.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>

namespace {
using namespace x2f;
struct Options {
    fs::path input, output;
    bool verify = false, dryRun = false, overwrite = false, bottomUp = false;
};
void usage() {
    std::cout << R"(xisf2fits - convert UInt16 camera XISF images to Rice-compressed FITS

Usage:
  xisf2fits [OPTIONS] INPUT.xisf OUTPUT.fits.fz
  xisf2fits [OPTIONS] INPUT_DIRECTORY OUTPUT_DIRECTORY

  --verify          Verify pixels, null masks, metadata, and both HDU checksums.
                    In batches, verify existing outputs before skipping them.
  --dry-run         Validate headers and destinations without writing outputs,
                    decoding pixels, or checking source block hashes.
  --overwrite       Replace existing outputs only after successful conversion.
  --row-order ORDER top-down (default) or bottom-up; reject WCS row flips.
  --version         Print program version.
  -h, --help        Show this help.
  --                End option parsing (allows filenames beginning with '-').

Declared source checksums are always checked during conversion, even without
--verify. Missing source checksums and metadata omissions are reported.
Scalar XISF properties are retained as HIERARCH XISF.IMAGE.* / XISF.FILE.*
keywords on the compressed image HDU. Unsupported image formats are skipped
in batches. Invalid data fails. Exit status: 0 success, 1 failure, 128+signal
on interruption. Sources are never modified.
)";
}
Options parse(int argc, char** argv) {
    Options options;
    std::vector<std::string> positional;
    bool end = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (end) positional.push_back(arg);
        else if (arg == "--") end = true;
        else if (arg == "--help" || arg == "-h") { usage(); std::exit(0); }
        else if (arg == "--version") { std::cout << "xisf2fits " << XISF2FITS_VERSION << '\n'; std::exit(0); }
        else if (arg == "--verify") options.verify = true;
        else if (arg == "--dry-run") options.dryRun = true;
        else if (arg == "--overwrite") options.overwrite = true;
        else if (arg == "--row-order") {
            if (++i >= argc) fail("--row-order requires top-down or bottom-up");
            const std::string order = argv[i];
            if (order != "top-down" && order != "bottom-up") fail("unknown row order: " + order);
            options.bottomUp = order == "bottom-up";
        } else if (!arg.empty() && arg.front() == '-') fail("unknown option: " + arg);
        else positional.push_back(arg);
    }
    if (positional.size() != 2) fail("expected INPUT and OUTPUT (use --help)");
    options.input = fs::absolute(positional[0]).lexically_normal();
    options.output = fs::absolute(positional[1]).lexically_normal();
    if (!fs::exists(options.input)) fail("input does not exist: " + options.input.string());
    return options;
}
bool isXisf(const fs::path& path) { return upper(path.extension().string()) == ".XISF"; }
bool outputExists(const fs::path& path) {
    // A dangling symlink also occupies a destination and must not bypass
    // no-overwrite handling.
    std::error_code error;
    auto status = fs::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) throw std::system_error(error, path.string());
    return fs::exists(status);
}
enum class Outcome { Converted, Skipped, Planned };
Outcome convert(const fs::path& input, const fs::path& output, const Options& options, bool batch) {
    checkpoint();
    bool exists = outputExists(output);
    if (exists && fs::exists(output) && fs::equivalent(input, output)) fail("output path refers to the input XISF");
    if (exists && !fs::is_regular_file(output)) fail("output exists but is not a regular file: " + output.string());
    if (exists && !options.overwrite && !batch) fail("output already exists (use --overwrite): " + output.string());
    if (exists && !options.overwrite && !options.verify) {
        std::cout << "SKIPPED " << input << ": output exists (not verified)\n";
        return Outcome::Skipped;
    }
    auto source = readSource(input, options.dryRun);
    auto metadata = prepareMetadata(source, options.bottomUp);
    auto records = headerRecords(metadata);
    for (const auto& warning : source.warnings) std::cerr << "WARNING " << input << ": " << warning << '\n';
    for (const auto& warning : metadata.warnings) std::cerr << "WARNING " << input << ": " << warning << '\n';
    if (options.dryRun) {
        std::cout << (exists && !options.overwrite ? "WOULD VERIFY " : "WOULD CONVERT ") << input << " -> " << output
                  << " [source hashes not checked]\n";
        return Outcome::Planned;
    }
    if (exists && !options.overwrite) {
        try { verifyFits(output, source, options.bottomUp, records); }
        catch (const Cancelled&) { throw; }
        catch (const std::exception& error) { fail(std::string(error.what()) + "; regenerate using --overwrite --verify"); }
        std::cout << "SKIPPED " << input << ": existing output verified\n";
        return Outcome::Skipped;
    }
    TemporaryOutput temporary(output);
    writeFits(temporary.file, source, options.bottomUp, records);
    if (options.verify) verifyFits(temporary.file, source, options.bottomUp, records);
    publish(temporary.file, output, options.overwrite);
    std::cout << "CONVERTED " << input << " -> " << output;
    if (source.imageChecksumVerified) std::cout << " [source checksum verified]";
    if (options.verify) std::cout << " [output verified]";
    std::cout << '\n';
    return Outcome::Converted;
}

void collect(const fs::path& root, std::vector<fs::path>& inputs, size_t& failed) {
    std::vector<fs::path> directories{root};
    while (!directories.empty()) {
        checkpoint();
        auto directory = directories.back(); directories.pop_back();
        std::error_code error;
        fs::directory_iterator it(directory, error), end;
        if (error) { std::cerr << "FAILED " << directory << ": " << error.message() << '\n'; ++failed; continue; }
        while (it != end) {
            const auto path = it->path();
            const auto status = it->symlink_status(error);
            if (!error && fs::is_directory(status)) directories.push_back(path);
            else if (!error && it->is_regular_file(error) && isXisf(path)) inputs.push_back(path);
            if (error) { std::cerr << "FAILED " << path << ": " << error.message() << '\n'; ++failed; error.clear(); }
            it.increment(error);
            if (error) { std::cerr << "FAILED " << directory << ": " << error.message() << '\n'; ++failed; break; }
        }
    }
}
void handleSignal(int signal) { interrupted = signal; }
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    try {
        const auto options = parse(argc, argv);
        if (fs::is_regular_file(options.input)) {
            if (!isXisf(options.input)) fail("input file must have a .xisf extension");
            auto output = options.output;
            if (fs::is_directory(output)) output /= options.input.stem().string() + ".fits.fz";
            const auto name = upper(output.filename().string());
            if (name.size() < 8 || name.substr(name.size() - 8) != ".FITS.FZ") fail("output filename must end in .fits.fz");
            convert(options.input, output, options, false);
            checkpoint();
            return 0;
        }
        if (!fs::is_directory(options.input)) fail("input must be a regular file or directory");
        if (outputExists(options.output) && !fs::is_directory(options.output)) fail("directory input needs directory output");
        size_t converted = 0, planned = 0, skipped = 0, failed = 0;
        std::vector<fs::path> inputs;
        collect(options.input, inputs, failed);
        std::sort(inputs.begin(), inputs.end());
        std::map<fs::path, std::vector<fs::path>> destinations;
        for (const auto& input : inputs) {
            auto relative = input.lexically_relative(options.input);
            if (relative.empty() || *relative.begin() == "..") fail("cannot derive safe relative path: " + input.string());
            relative.replace_extension(".fits.fz");
            destinations[options.output / relative].push_back(input);
        }
        // Destinations differing only in case collide on case-insensitive
        // filesystems; refuse them everywhere rather than probe the target.
        std::map<std::string, size_t> folded;
        for (const auto& entry : destinations) ++folded[upper(entry.first.string())];
        for (const auto& [output, sources] : destinations) {
            checkpoint();
            if (sources.size() > 1 || folded[upper(output.string())] > 1) {
                for (const auto& source : sources) { std::cerr << "FAILED " << source << ": destination collision: " << output << '\n'; ++failed; }
                continue;
            }
            const auto& input = sources.front();
            try {
                switch (convert(input, output, options, true)) {
                case Outcome::Converted: ++converted; break;
                case Outcome::Skipped: ++skipped; break;
                case Outcome::Planned: ++planned; break;
                }
            } catch (const Cancelled&) { throw; }
            catch (const Unsupported& error) { std::cout << "SKIPPED " << input << ": " << error.what() << '\n'; ++skipped; }
            catch (const std::exception& error) { std::cerr << "FAILED " << input << ": " << error.what() << '\n'; ++failed; }
        }
        std::cout << "Summary: " << (options.dryRun ? planned : converted) << (options.dryRun ? " planned" : " converted")
                  << ", " << skipped << " skipped, " << failed << " failed\n";
        return failed ? 1 : 0;
    } catch (const Cancelled& error) {
        std::cerr << "xisf2fits: " << error.what() << '\n';
        return 128 + interrupted;
    } catch (const std::exception& error) {
        std::cerr << "xisf2fits: " << error.what() << '\n';
        return 1;
    }
}
