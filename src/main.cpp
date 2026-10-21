#include "converter.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

namespace {
using namespace x2f;
struct Options {
    fs::path input, output;
    bool verify = false, dryRun = false, overwrite = false, bottomUp = false, verbose = false;
    size_t jobs = 1;
};
std::atomic<size_t> truncatedFiles{0};
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
  -j, --jobs N      Convert N files at once in directory mode (default 1;
                    0 uses every hardware thread).
  --verbose         List the keywords whose header comments were shortened.
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
        else if (arg == "--verbose") options.verbose = true;
        else if (arg == "--row-order") {
            if (++i >= argc) fail("--row-order requires top-down or bottom-up");
            const std::string order = argv[i];
            if (order != "top-down" && order != "bottom-up") fail("unknown row order: " + order);
            options.bottomUp = order == "bottom-up";
        } else if (arg == "--jobs" || arg == "-j") {
            if (++i >= argc) fail("--jobs requires a number");
            options.jobs = static_cast<size_t>(unsignedNumber(argv[i], "job count"));
            if (!options.jobs) options.jobs = std::max(1u, std::thread::hardware_concurrency());
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
Outcome convert(const fs::path& input, const fs::path& output, const Options& options, bool batch,
                std::ostream& out, std::ostream& err) {
    checkpoint();
    bool exists = outputExists(output);
    if (exists && fs::exists(output) && fs::equivalent(input, output)) fail("output path refers to the input XISF");
    if (exists && !fs::is_regular_file(output)) fail("output exists but is not a regular file: " + output.string());
    if (exists && !options.overwrite && !batch) fail("output already exists (use --overwrite): " + output.string());
    if (exists && !options.overwrite && !options.verify) {
        out << "SKIPPED " << input << ": output exists (not verified)\n";
        return Outcome::Skipped;
    }
    auto source = readSource(input, options.dryRun);
    auto metadata = prepareMetadata(source, options.bottomUp);
    auto records = headerRecords(metadata);
    for (const auto& warning : source.warnings) err << "WARNING " << input << ": " << warning << '\n';
    for (const auto& warning : metadata.warnings) err << "WARNING " << input << ": " << warning << '\n';
    const bool truncated = !metadata.truncatedComments.empty();
    if (truncated && options.verbose) {
        err << "NOTE " << input << ": truncated header comments:";
        for (size_t i = 0; i < metadata.truncatedComments.size(); ++i)
            err << (i ? ", " : " ") << metadata.truncatedComments[i];
        err << '\n';
    }
    if (options.dryRun) {
        out << (exists && !options.overwrite ? "WOULD VERIFY " : "WOULD CONVERT ") << input << " -> " << output
            << " [source hashes not checked]\n";
        truncatedFiles += truncated;
        return Outcome::Planned;
    }
    if (exists && !options.overwrite) {
        try { verifyFits(output, source, options.bottomUp, records); }
        catch (const Cancelled&) { throw; }
        catch (const std::exception& error) { fail(std::string(error.what()) + "; regenerate using --overwrite --verify"); }
        out << "SKIPPED " << input << ": existing output verified\n";
        truncatedFiles += truncated;
        return Outcome::Skipped;
    }
    TemporaryOutput temporary(output);
    writeFits(temporary.file, source, options.bottomUp, records);
    if (options.verify) verifyFits(temporary.file, source, options.bottomUp, records);
    publish(temporary.file, output, options.overwrite);
    out << "CONVERTED " << input << " -> " << output;
    if (source.imageChecksumVerified) out << " [source checksum verified]";
    if (options.verify) out << " [output verified]";
    out << '\n';
    truncatedFiles += truncated;
    return Outcome::Converted;
}

void collect(const fs::path& root, std::vector<fs::path>& inputs, std::atomic<size_t>& failed) {
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
// Comments are cut to fit 80-character cards on every NINA frame; values never
// are. Report this once per run so that actionable warnings remain visible.
void reportTruncation(bool verbose) {
    if (!truncatedFiles) return;
    std::cerr << "NOTE: header comments shortened to fit 80-character cards in " << truncatedFiles
              << (truncatedFiles == 1 ? " file" : " files") << " (values intact"
              << (verbose ? ")" : "; --verbose lists keywords)") << '\n';
}
void handleSignal(int signal) { interrupted = signal; }
}

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    try {
        initializeLibraries();
        const auto options = parse(argc, argv);
        if (fs::is_regular_file(options.input)) {
            if (!isXisf(options.input)) fail("input file must have a .xisf extension");
            auto output = options.output;
            if (fs::is_directory(output)) output /= options.input.stem().string() + ".fits.fz";
            const auto name = upper(output.filename().string());
            if (name.size() < 8 || name.substr(name.size() - 8) != ".FITS.FZ") fail("output filename must end in .fits.fz");
            convert(options.input, output, options, false, std::cout, std::cerr);
            checkpoint();
            reportTruncation(options.verbose);
            return 0;
        }
        if (!fs::is_directory(options.input)) fail("input must be a regular file or directory");
        if (outputExists(options.output) && !fs::is_directory(options.output)) fail("directory input needs directory output");
        std::atomic<size_t> converted{0}, planned{0}, skipped{0}, failed{0};
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
        std::vector<std::pair<fs::path, fs::path>> tasks;
        for (const auto& [output, sources] : destinations) {
            if (sources.size() > 1 || folded[upper(output.string())] > 1) {
                for (const auto& source : sources) { std::cerr << "FAILED " << source << ": destination collision: " << output << '\n'; ++failed; }
                continue;
            }
            tasks.emplace_back(sources.front(), output);
        }
        std::atomic<size_t> next{0};
        std::mutex printing;
        auto work = [&] {
            for (size_t i; (i = next++) < tasks.size();) {
                const auto& [input, output] = tasks[i];
                std::ostringstream out, err;
                try {
                    switch (convert(input, output, options, true, out, err)) {
                    case Outcome::Converted: ++converted; break;
                    case Outcome::Skipped: ++skipped; break;
                    case Outcome::Planned: ++planned; break;
                    }
                } catch (const Cancelled&) { return; }
                catch (const Unsupported& error) { out << "SKIPPED " << input << ": " << error.what() << '\n'; ++skipped; }
                catch (const std::exception& error) { err << "FAILED " << input << ": " << error.what() << '\n'; ++failed; }
                // Print each file's diagnostics and result together, as it completes.
                std::lock_guard<std::mutex> lock(printing);
                std::cerr << err.str();
                std::cout << out.str() << std::flush;
            }
        };
        const size_t workers = std::min(options.jobs, tasks.size());
        std::vector<std::thread> helpers;
        for (size_t i = 1; i < workers; ++i) {
            try { helpers.emplace_back(work); }
            catch (const std::system_error& error) {
                std::lock_guard<std::mutex> lock(printing);
                std::cerr << "WARNING: running " << i << " of " << workers << " jobs: " << error.what() << '\n';
                break;
            }
        }
        work();
        for (auto& helper : helpers) helper.join();
        checkpoint();
        reportTruncation(options.verbose);
        std::cout << "Summary: " << (options.dryRun ? planned : converted).load() << (options.dryRun ? " planned" : " converted")
                  << ", " << skipped.load() << " skipped, " << failed.load() << " failed\n";
        return failed ? 1 : 0;
    } catch (const Cancelled& error) {
        std::cerr << "xisf2fits: " << error.what() << '\n';
        return 128 + interrupted;
    } catch (const std::exception& error) {
        std::cerr << "xisf2fits: " << error.what() << '\n';
        return 1;
    }
}
