#include "converter.hpp"

#include <libxml/parser.h>
#include <openssl/evp.h>
#include <lz4.h>
#include <zlib.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>

namespace x2f {
namespace {
using Bytes = std::vector<unsigned char>;
// Limit for the XML header and, separately, for decoded String properties.
constexpr uint64_t metadataBudget = 64 * 1024 * 1024;
std::string attr(xmlNode* node, const char* name) {
    auto* p = xmlGetProp(node, BAD_CAST name);
    std::string value = p ? reinterpret_cast<const char*>(p) : "";
    xmlFree(p);
    return value;
}
std::string text(xmlNode* node) {
    std::string result;
    for (auto* child = node->children; child; child = child->next)
        if (child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE)
            result += reinterpret_cast<const char*>(child->content);
    return result;
}
bool named(xmlNode* node, const char* name) {
    return node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name) &&
           node->ns && xmlStrEqual(node->ns->href, BAD_CAST "http://www.pixinsight.com/xisf");
}
// N.I.N.A. 3.0 writes the XISF default display resolution, 72 pixels per inch;
// it says nothing about the optics, so only other values are reported.
bool defaultResolution(xmlNode* node) {
    const auto unit = attr(node, "unit");
    return named(node, "Resolution") && attr(node, "horizontal") == "72" &&
           attr(node, "vertical") == "72" && (unit.empty() || unit == "inch");
}
std::vector<xmlNode*> children(xmlNode* node, const char* name) {
    std::vector<xmlNode*> result;
    for (auto* child = node->children; child; child = child->next)
        if (named(child, name)) result.push_back(child);
    return result;
}
std::vector<std::string> split(const std::string& s, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        auto end = s.find(separator, start);
        parts.push_back(s.substr(start, end - start));
        if (end == std::string::npos) return parts;
        start = end + 1;
    }
}
void readExact(std::ifstream& stream, unsigned char* data, size_t size) {
    while (size) {
        checkpoint();
        auto chunk = std::min(size, size_t{1024 * 1024});
        stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(chunk));
        if (stream.gcount() != static_cast<std::streamsize>(chunk)) fail("short read in XISF source");
        size -= chunk;
        data += chunk;
    }
}
void seek(std::ifstream& stream, uint64_t offset) {
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    if (!stream) fail("cannot seek XISF source");
}
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
Bytes decodeText(std::string encoded, const std::string& encoding) {
    encoded.erase(std::remove_if(encoded.begin(), encoded.end(), [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }), encoded.end());
    Bytes result;
    if (encoding == "base16" || encoding == "hex") {
        if (encoded.size() % 2) fail("invalid base16 block");
        for (size_t i = 0; i < encoded.size(); i += 2) {
            int a = hexDigit(encoded[i]), b = hexDigit(encoded[i + 1]);
            if (a < 0 || b < 0) fail("invalid base16 block");
            result.push_back(static_cast<unsigned char>(a * 16 + b));
        }
    } else if (encoding == "base64") {
        if (encoded.empty() || encoded.size() % 4) fail("invalid base64 block");
        size_t padding = encoded.back() == '=' ? (encoded[encoded.size() - 2] == '=' ? 2 : 1) : 0;
        for (size_t i = 0; i < encoded.size() - padding; ++i) {
            const char c = encoded[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/')) fail("invalid base64 block");
        }
        result.resize(encoded.size() / 4 * 3);
        int n = EVP_DecodeBlock(result.data(), reinterpret_cast<const unsigned char*>(encoded.data()),
                                static_cast<int>(encoded.size()));
        if (n < 0) fail("invalid base64 block");
        result.resize(static_cast<size_t>(n) - padding);
    } else fail("unsupported block encoding: " + encoding);
    return result;
}

struct Block {
    uint64_t offset = 0, size = 0, decodedSize = 0;
    Bytes inlineData;
    bool attachment = false;
    std::string codec, checksum, label;
    size_t shuffle = 1;
    std::vector<std::pair<size_t, size_t>> subblocks;
};

Block inspectBlock(xmlNode* node, uint64_t fileSize, uint64_t headerEnd) {
    Block b;
    b.label = reinterpret_cast<const char*>(node->name);
    if (!attr(node, "id").empty()) b.label += " " + attr(node, "id");
    auto location = split(attr(node, "location"), ':');
    xmlNode* dataNode = node;
    if (location[0] == "attachment" && location.size() == 3) {
        b.attachment = true;
        b.offset = unsignedNumber(location[1], "attachment offset");
        b.size = unsignedNumber(location[2], "attachment size");
        if (b.offset < headerEnd || b.offset > fileSize || b.size > fileSize - b.offset)
            fail("attachment outside source file: " + b.label);
    } else if (location[0] == "inline" && location.size() == 2) {
        b.inlineData = decodeText(text(node), location[1]);
        b.size = b.inlineData.size();
    } else if (location.size() == 1 && location[0] == "embedded") {
        auto data = children(node, "Data");
        if (data.size() != 1) fail("embedded block requires exactly one Data element");
        dataNode = data.front();
        b.inlineData = decodeText(text(dataNode), attr(dataNode, "encoding"));
        b.size = b.inlineData.size();
    } else fail("unsupported data location: " + attr(node, "location"));
    auto parameter = [&](const char* key) {
        return xmlHasProp(dataNode, BAD_CAST key) ? attr(dataNode, key) : attr(node, key);
    };
    const auto byteOrder = parameter("byteOrder");
    if (!byteOrder.empty() && byteOrder != "little") fail("unsupported byte order: " + byteOrder);
    b.checksum = parameter("checksum");
    b.decodedSize = b.size;
    const auto compression = parameter("compression");
    if (!compression.empty()) {
        auto c = split(compression, ':');
        if (c.size() < 2 || c.size() > 3) fail("invalid compression descriptor");
        b.codec = c[0];
        bool shuffled = b.codec.size() > 3 && b.codec.substr(b.codec.size() - 3) == "+sh";
        if (shuffled) b.codec.resize(b.codec.size() - 3);
        if (b.codec != "zlib" && b.codec != "lz4" && b.codec != "lz4hc" && b.codec != "zstd")
            fail("unsupported compression codec: " + b.codec);
        b.decodedSize = unsignedNumber(c[1], "uncompressed size");
        if (shuffled != (c.size() == 3)) fail("invalid byte shuffle descriptor");
        if (shuffled) {
            const auto itemSize = unsignedNumber(c[2], "shuffle item size");
            if (!itemSize || itemSize > b.decodedSize || itemSize > 16) fail("invalid shuffle item size");
            b.shuffle = static_cast<size_t>(itemSize);
        }
    }
    if (!b.size || !b.decodedSize || b.size > std::numeric_limits<size_t>::max() ||
        b.decodedSize > std::numeric_limits<size_t>::max()) fail("invalid block size: " + b.label);
    if (!parameter("subblocks").empty()) {
        if (b.codec.empty()) fail("subblocks require compression");
        uint64_t compressed = 0, decoded = 0;
        for (const auto& part : split(parameter("subblocks"), ':')) {
            const auto sizes = split(part, ',');
            if (sizes.size() != 2) fail("invalid subblock descriptor");
            const auto c = unsignedNumber(sizes[0], "subblock compressed size");
            const auto d = unsignedNumber(sizes[1], "subblock decoded size");
            if (!c || !d || c > b.size - compressed || d > b.decodedSize - decoded)
                fail("subblock sizes exceed block size");
            compressed += c; decoded += d;
            b.subblocks.emplace_back(static_cast<size_t>(c), static_cast<size_t>(d));
        }
        if (compressed != b.size || decoded != b.decodedSize) fail("subblock sizes do not match block size");
    } else b.subblocks.emplace_back(static_cast<size_t>(b.size), static_cast<size_t>(b.decodedSize));
    if (b.codec == "lz4" || b.codec == "lz4hc")
        for (auto [c, d] : b.subblocks)
            if (c > INT_MAX || d > INT_MAX) fail("LZ4 subblock exceeds codec size limit");
    return b;
}

// XISF 1.0 algorithm names, mapped to OpenSSL digest names.
const std::map<std::string, std::string> checksumNames = {
    {"SHA1", "SHA1"}, {"SHA-1", "SHA1"}, {"SHA256", "SHA256"}, {"SHA-256", "SHA256"},
    {"SHA512", "SHA512"}, {"SHA-512", "SHA512"}, {"SHA3-256", "SHA3-256"}, {"SHA3-512", "SHA3-512"}
};

const EVP_MD* checksumAlgorithm(const Block& b, std::string& expected) {
    auto fields = split(b.checksum, ':');
    if (fields.size() != 2) fail("invalid source checksum: " + b.label);
    const auto name = checksumNames.find(upper(fields[0]));
    if (name == checksumNames.end()) fail("unsupported source checksum algorithm: " + fields[0]);
    auto* md = EVP_get_digestbyname(name->second.c_str());
    if (!md) fail("unavailable source checksum algorithm: " + name->second);
    expected = upper(fields[1]);
    if (expected.size() != static_cast<size_t>(EVP_MD_size(md)) * 2 ||
        !std::all_of(expected.begin(), expected.end(), [](char c) { return hexDigit(c) >= 0; }))
        fail("invalid source checksum digest: " + b.label);
    return md;
}

void checkChecksum(const Block& b, std::ifstream& stream, const Bytes* stored, bool dryRun) {
    if (b.checksum.empty()) return;
    std::string expected;
    auto* md = checksumAlgorithm(b, expected);
    if (dryRun) return;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) fail("cannot initialize source checksum");
    auto update = [&](const unsigned char* data, size_t size) {
        if (EVP_DigestUpdate(ctx.get(), data, size) != 1) fail("source checksum update failed");
    };
    if (stored) update(stored->data(), stored->size());
    else if (!b.attachment) update(b.inlineData.data(), b.inlineData.size());
    else {
        std::array<unsigned char, 1024 * 1024> buffer{};
        seek(stream, b.offset);
        for (uint64_t remaining = b.size; remaining;) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            readExact(stream, buffer.data(), n); update(buffer.data(), n); remaining -= n;
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int n = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &n) != 1) fail("source checksum finalization failed");
    std::string actual;
    const char* hex = "0123456789ABCDEF";
    for (unsigned int i = 0; i < n; ++i) { actual += hex[digest[i] >> 4]; actual += hex[digest[i] & 15]; }
    if (actual != expected) fail("source checksum mismatch: " + b.label);
}

Bytes readBlock(const Block& b, std::ifstream& stream) {
    Bytes stored;
    if (b.attachment) { stored.resize(static_cast<size_t>(b.size)); seek(stream, b.offset); readExact(stream, stored.data(), stored.size()); }
    else stored = b.inlineData;
    checkChecksum(b, stream, &stored, false);
    if (b.codec.empty()) return stored;
    Bytes decoded(static_cast<size_t>(b.decodedSize));
    size_t in = 0, out = 0;
    for (auto [c, d] : b.subblocks) {
        checkpoint();
        if (b.codec == "zlib") {
            uLongf n = static_cast<uLongf>(d);
            uLong consumed = static_cast<uLong>(c);
            if (n != d || consumed != c || uncompress2(decoded.data() + out, &n, stored.data() + in, &consumed) != Z_OK || n != d || consumed != c)
                fail("zlib decompression size or data error");
        } else if (b.codec == "lz4" || b.codec == "lz4hc") {
            int n = LZ4_decompress_safe(reinterpret_cast<const char*>(stored.data() + in),
                                      reinterpret_cast<char*>(decoded.data() + out), static_cast<int>(c), static_cast<int>(d));
            if (n < 0 || static_cast<size_t>(n) != d) fail("LZ4 decompression size or data error");
        } else {
            size_t n = ZSTD_decompress(decoded.data() + out, d, stored.data() + in, c);
            if (ZSTD_isError(n) || n != d) fail("ZSTD decompression size or data error");
        }
        in += c; out += d;
    }
    if (b.shuffle > 1) {
        Bytes unshuffled(decoded.size());
        size_t count = decoded.size() / b.shuffle;
        for (size_t byte = 0; byte < b.shuffle; ++byte) {
            checkpoint();
            for (size_t i = 0; i < count; ++i) unshuffled[i * b.shuffle + byte] = decoded[byte * count + i];
        }
        std::copy(decoded.begin() + static_cast<ptrdiff_t>(count * b.shuffle), decoded.end(),
                  unshuffled.begin() + static_cast<ptrdiff_t>(count * b.shuffle));
        return unshuffled;
    }
    return decoded;
}
}

void initializeLibraries() {
    xmlInitParser();
    if (fits_init_cfitsio()) fail("cannot initialize CFITSIO");
    // Digest nothing with each algorithm, so that OpenSSL loads its providers
    // and method caches now rather than on concurrent first use.
    for (const auto& entry : checksumNames) {
        unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int n = 0;
        auto* md = EVP_get_digestbyname(entry.second.c_str());
        if (md && EVP_Digest("", 0, digest, &n, md, nullptr) != 1) fail("cannot initialize digest " + entry.second);
    }
}

Source readSource(const fs::path& path, bool dryRun) {
    Source source;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) fail("cannot open XISF source: " + path.string());
    const auto length = stream.tellg();
    if (length < 16) fail("truncated XISF signature");
    const auto fileSize = static_cast<uint64_t>(length);
    seek(stream, 0);
    std::array<unsigned char, 16> signature{};
    readExact(stream, signature.data(), signature.size());
    if (std::memcmp(signature.data(), "XISF0100", 8) != 0) fail("invalid XISF signature");
    uint32_t headerLength = 0;
    for (size_t i = 0; i < 4; ++i) {
        headerLength |= uint32_t(signature[8 + i]) << (8 * i);
        if (signature[12 + i]) fail("nonzero XISF reserved signature bytes");
    }
    if (!headerLength || headerLength > metadataBudget || headerLength > fileSize - 16)
        fail("invalid or oversized XISF XML header length (limit 64 MiB)");
    Bytes xml(headerLength);
    readExact(stream, xml.data(), xml.size());
    // libxml2 otherwise caps text nodes at 10,000,000 bytes, rejecting inline
    // blocks within the header budget. The size cap and DTD rejection bound it.
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlReadMemory(reinterpret_cast<const char*>(xml.data()), static_cast<int>(xml.size()), nullptr, nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_HUGE), xmlFreeDoc);
    if (!doc || doc->intSubset || doc->extSubset) fail("invalid XISF XML or forbidden DTD");
    auto* root = xmlDocGetRootElement(doc.get());
    if (!root || !named(root, "xisf") || attr(root, "version") != "1.0") fail("unsupported XISF root or version");
    auto nodes = children(root, "Image");
    if (nodes.empty()) fail("XISF contains no image");
    // Compression settings persist across the extensions of a FITS file, so
    // every image must have the same sample format.
    std::string list;
    bool master = false, supported = true;
    for (auto* node : nodes) {
        auto geometry = split(attr(node, "geometry"), ':');
        if (geometry.size() != 3) fail("invalid image geometry");
        Image image;
        image.id = attr(node, "id");
        image.width = unsignedNumber(geometry[0], "image width");
        image.height = unsignedNumber(geometry[1], "image height");
        auto channels = unsignedNumber(geometry[2], "channel count");
        if (!image.width || !image.height || !channels) fail("zero image dimension");
        const auto format = attr(node, "sampleFormat"), space = attr(node, "colorSpace");
        image.float32 = format == "Float32";
        supported &= channels == 1 && space == "Gray" && (image.float32 || format == "UInt16") &&
                     (source.images.empty() || source.images.front().float32 == image.float32);
        // Masters carry rejection, weight, crop or slope maps beside the result.
        master |= image.id.rfind("integration", 0) == 0;
        list += (list.empty() ? "" : ", ") + (image.id.empty() ? "unnamed" : image.id) +
                (space == "Gray" || space.empty() ? "" : " " + space) + " " + format;
        source.images.push_back(std::move(image));
    }
    if (!supported && nodes.size() == 1) throw Unsupported("expected single-channel UInt16 or Float32 Gray image");
    if (!supported)
        throw Unsupported((master ? "PixInsight integration master (" : "multi-image XISF (") + list +
                          "); every image must be single-channel UInt16 or Float32 Gray, of one format");
    // Diagnostics about one image of several name it.
    auto label = [&](size_t i) {
        if (nodes.size() == 1) return std::string();
        return "image " + (source.images[i].id.empty() ? "#" + std::to_string(i + 1) : source.images[i].id) + ": ";
    };
    std::map<xmlNode*, size_t> imageIndex;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& image = source.images[i];
        const uint64_t bytes = image.float32 ? 4 : 2;
        if (image.width > static_cast<uint64_t>(LONG_MAX) || image.height > static_cast<uint64_t>(LONG_MAX) ||
            image.height > std::numeric_limits<size_t>::max() / bytes / image.width ||
            image.height > static_cast<uint64_t>(LLONG_MAX) / bytes / image.width)
            fail("image dimensions overflow pixel count");
        const auto storage = attr(nodes[i], "pixelStorage");
        if (!storage.empty() && storage != "Planar" && storage != "Normal") fail("unsupported pixel storage");
        imageIndex[nodes[i]] = i;
    }

    std::map<xmlNode*, Block> blocks;
    std::function<void(xmlNode*)> inspect = [&](xmlNode* node) {
        if (node->type != XML_ELEMENT_NODE) return;
        checkpoint();
        if (xmlHasProp(node, BAD_CAST "location")) {
            auto b = inspectBlock(node, fileSize, uint64_t{16} + headerLength);
            checkChecksum(b, stream, nullptr, true); // Validate descriptors even in dry-run.
            blocks.emplace(node, std::move(b));
        }
        for (auto* child = node->children; child; child = child->next) inspect(child);
    };
    inspect(root);
    bool unchecked = false;
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!blocks.count(nodes[i])) fail("image has no data block");
        const auto& block = blocks.at(nodes[i]);
        const auto& image = source.images[i];
        const uint64_t bytes = image.float32 ? 4 : 2;
        if (block.decodedSize != image.width * image.height * bytes) fail("image block size differs from geometry");
        if (block.shuffle != 1 && block.shuffle != bytes)
            fail(image.float32 ? "invalid Float32 shuffle item size" : "invalid UInt16 shuffle item size");
        unchecked |= block.checksum.empty();
    }
    if (unchecked) source.warnings.push_back("source image checksum absent; source integrity cannot be verified");
    // Check advertised sizes before allocating: a tiny compressed block can
    // claim an arbitrary decoded size.
    uint64_t stringBytes = 0;
    for (const auto& [node, block] : blocks) {
        if (imageIndex.count(node) || !named(node, "Property") || attr(node, "type") != "String") continue;
        if (block.decodedSize > metadataBudget - stringBytes) fail("decoded XISF string properties exceed 64 MiB");
        stringBytes += block.decodedSize;
    }

    std::map<xmlNode*, std::string> strings;
    for (const auto& [node, block] : blocks) {
        if (dryRun) continue;
        auto index = imageIndex.find(node);
        if (index != imageIndex.end()) {
            auto& image = source.images[index->second];
            auto bytes = readBlock(block, stream);
            if (image.float32) {
                image.samples.resize(bytes.size() / 4);
                for (size_t i = 0; i < image.samples.size(); ++i) {
                    const uint32_t bits = uint32_t(bytes[4 * i]) | (uint32_t(bytes[4 * i + 1]) << 8) |
                                          (uint32_t(bytes[4 * i + 2]) << 16) | (uint32_t(bytes[4 * i + 3]) << 24);
                    std::memcpy(&image.samples[i], &bits, sizeof bits);
                }
            } else {
                image.pixels.resize(bytes.size() / 2);
                for (size_t i = 0; i < image.pixels.size(); ++i)
                    image.pixels[i] = uint16_t(bytes[2 * i]) | (uint16_t(bytes[2 * i + 1]) << 8);
            }
            image.checksumVerified = !block.checksum.empty();
        } else if (named(node, "Property") && attr(node, "type") == "String") {
            auto bytes = readBlock(block, stream);
            strings[node] = std::string(bytes.begin(), bytes.end());
        } else checkChecksum(block, stream, nullptr, false);
    }
    size_t astrometric = 0;
    auto properties = [&](xmlNode* parent, const std::string& scope, Image& image, const std::string& prefix) {
        for (auto* node : children(parent, "Property")) {
            Property p{scope, attr(node, "id"), attr(node, "type"), attr(node, "value"), attr(node, "comment")};
            // Copying a native PixInsight solution would not make a usable FITS
            // WCS, and a partial one would mislead: omit all of it.
            if (p.id.rfind("PCL:AstrometricSolution", 0) == 0 || p.id.rfind("AstrometricSolution", 0) == 0) {
                ++astrometric;
                continue;
            }
            if (p.type == "String") {
                if (blocks.count(node)) {
                    if (dryRun) { source.warnings.push_back(prefix + "property deferred until conversion: " + p.id); continue; }
                    p.value = strings.at(node);
                } else p.value = text(node);
            }
            image.properties.push_back(std::move(p));
        }
    };
    for (size_t i = 0; i < nodes.size(); ++i) properties(nodes[i], "IMAGE", source.images[i], label(i));
    for (auto* node : children(root, "Metadata")) {
        properties(node, "FILE", source.images.front(), "");
        for (auto* child = node->children; child; child = child->next)
            if (child->type == XML_ELEMENT_NODE && !named(child, "Property"))
                source.warnings.push_back("omitted XISF metadata element: " + std::string(reinterpret_cast<const char*>(child->name)));
    }
    if (astrometric)
        source.warnings.push_back("omitted PixInsight astrometric solution (" + std::to_string(astrometric) +
                                  (astrometric == 1 ? " property" : " properties") + "); no FITS WCS is written");
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto* node = nodes[i];
        auto& image = source.images[i];
        for (auto* keyword : children(node, "FITSKeyword"))
            image.keywords.push_back({attr(keyword, "name"), attr(keyword, "value"), attr(keyword, "comment")});
        auto cfas = children(node, "ColorFilterArray");
        if (cfas.size() > 1) fail(label(i) + "multiple color filter arrays");
        if (!cfas.empty()) {
            if (attr(cfas[0], "width") != "2" || attr(cfas[0], "height") != "2") fail(label(i) + "unsupported CFA dimensions; expected 2x2 Bayer");
            image.cfa = upper(attr(cfas[0], "pattern"));
            if (image.cfa.empty()) fail(label(i) + "empty CFA pattern");
        }
        for (auto* child = node->children; child; child = child->next)
            if (child->type == XML_ELEMENT_NODE && !named(child, "Property") && !named(child, "FITSKeyword") &&
                !named(child, "ColorFilterArray") && !named(child, "Data") && !defaultResolution(child))
                source.warnings.push_back(label(i) + "omitted XISF image element: " + std::string(reinterpret_cast<const char*>(child->name)));
    }
    for (auto* node = root->children; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && !named(node, "Image") && !named(node, "Metadata"))
            source.warnings.push_back("omitted XISF container element: " + std::string(reinterpret_cast<const char*>(node->name)));
    if (fs::file_size(path) != fileSize) fail("source file size changed during conversion");
    checkpoint();
    return source;
}
}
