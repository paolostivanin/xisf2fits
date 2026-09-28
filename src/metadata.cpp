#include "converter.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <regex>
#include <set>

namespace x2f {
namespace {
bool shortName(const std::string& name) {
    return !name.empty() && name.size() <= 8 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
std::string prefix(const std::string& name) {
    if (shortName(name)) return name + std::string(8 - name.size(), ' ') + "= ";
    if (name.empty() || name.size() > 64 || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return c >= 32 && c <= 126 && c != '=';
        })) fail("unrepresentable FITS keyword name: " + name);
    return "HIERARCH " + name + " = ";
}
bool knownString(const std::string& name) {
    static const std::set<std::string> names = {
        "OBJECT", "FILTER", "DATE", "DATE-OBS", "DATE-END", "DATE-BEG", "DATE-AVG", "DATE-LOC",
        "IMAGETYP", "INSTRUME", "TELESCOP", "FOCNAME", "SWCREATE", "SWMODIFY", "OBSERVER", "ORIGIN",
        "OBJCTRA", "OBJCTDEC", "READOUTM", "FWHEEL", "PIERSIDE", "BAYERPAT", "CAMERAID", "ROTNAME",
        "CREATOR", "OBSERVAT", "SITENAME", "EXTNAME", "RADESYS", "RADECSYS", "TIMESYS", "BUNIT"
    };
    return names.count(name) || name.rfind("CTYPE", 0) == 0 || name.rfind("CUNIT", 0) == 0;
}
std::string decodeLiteral(const std::string& raw) {
    std::string value;
    for (size_t i = 1; i + 1 < raw.size(); ++i) {
        if (raw[i] == '\'') {
            if (i + 2 >= raw.size() || raw[i + 1] != '\'') fail("malformed quoted FITS value: " + raw);
            ++i;
        }
        value += raw[i];
    }
    return value;
}
void normalize(Card& card, Metadata& metadata) {
    bool changed = false;
    card.value = ascii(card.value, changed);
    card.comment = ascii(card.comment, changed);
    if (changed) metadata.warnings.push_back("ASCII-normalized keyword: " + card.name);
}
bool numeric(ValueType t) { return t == ValueType::Integer || t == ValueType::Real; }
ValueType numberType(const std::string& value) {
    // Scan instead of using std::regex: some implementations recurse for each
    // digit and can exhaust the stack on a large XML keyword value.
    size_t i = 0, digits = 0;
    bool real = false;
    if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
    auto consumeDigits = [&] {
        size_t n = 0;
        while (i < value.size() && value[i] >= '0' && value[i] <= '9') { ++i; ++n; }
        return n;
    };
    digits += consumeDigits();
    if (i < value.size() && value[i] == '.') { real = true; ++i; digits += consumeDigits(); }
    if (!digits) return ValueType::String;
    if (i < value.size() && (value[i] == 'E' || value[i] == 'e' || value[i] == 'D' || value[i] == 'd')) {
        real = true; ++i;
        if (i < value.size() && (value[i] == '+' || value[i] == '-')) ++i;
        if (!consumeDigits()) return ValueType::String;
    }
    if (i != value.size()) return ValueType::String;
    return real ? ValueType::Real : ValueType::Integer;
}
long double number(std::string token) {
    for (char& c : token) if (c == 'd' || c == 'D') c = 'E';
    return std::strtold(token.c_str(), nullptr);
}
}

ValueType scalarType(const std::string& value) {
    if (value.empty()) return ValueType::Undefined;
    if (value == "T" || value == "F") return ValueType::Logical;
    const auto type = numberType(value);
    if (type != ValueType::String) return type;
    if (value.front() == '(' && value.back() == ')') {
        auto comma = value.find(',');
        if (comma != std::string::npos && numeric(numberType(trim(value.substr(1, comma - 1)))) &&
            numeric(numberType(trim(value.substr(comma + 1, value.size() - comma - 2))))) return ValueType::Complex;
    }
    return ValueType::String;
}

bool structuralKeyword(const std::string& name) {
    if (name.size() > 64) return false;
    static const std::set<std::string> names = {
        "SIMPLE", "XTENSION", "BITPIX", "NAXIS", "PCOUNT", "GCOUNT", "EXTEND", "END", "BLOCKED",
        "BSCALE", "BZERO", "BLANK", "CHECKSUM", "DATASUM", "TFIELDS", "THEAP", "ROWORDER", "LONGSTRN",
        "ZIMAGE", "ZTABLE", "ZCMPTYPE", "ZBITPIX", "ZNAXIS", "ZTILELEN", "ZHECKSUM", "ZDATASUM",
        "ZSIMPLE", "ZEXTEND", "ZBLOCKED", "ZTENSION", "ZPCOUNT", "ZGCOUNT", "ZQUANTIZ", "ZDITHER0",
        "ZBLANK", "ZSCALE", "ZZERO", "ZHEAPPTR", "ZTHEAP", "ZFORM", "ZCTYP", "ZMASKCMP"
    };
    static const std::regex indexed(R"((NAXIS|ZNAXIS|ZTILE|ZNAME|ZVAL|TTYPE|TFORM|TDIM|TSCAL|TZERO|TNULL|TUNIT|TBCOL|ZFORM|ZCTYP)[0-9]+)");
    return names.count(name) || std::regex_match(name, indexed);
}
bool wcsKeyword(const std::string& name) {
    if (name.size() > 64) return false;
    static const std::regex indexed(R"((WCSAXES|WCSNAME|RADESYS|RADECSYS|EQUINOX|LONPOLE|LATPOLE)[A-Z]?|(CTYPE|CRPIX|CRVAL|CDELT|CROTA|CUNIT)[0-9]+[A-Z]?|(CD|PC|PV|PS)[0-9]+_[0-9]+[A-Z]?|(A|B|AP|BP)_(ORDER|DMAX|[0-9]+_[0-9]+)|(CPDIS|CQDIS|D2IMDIS|DET2IM)[0-9]+|DP[0-9]+.*)");
    // EQUINOX and RADESYS alone are pointing hints in NINA, not a solution.
    return name != "EQUINOX" && name != "RADESYS" && name != "RADECSYS" && std::regex_match(name, indexed);
}
Card parseKeyword(const Keyword& keyword) {
    Card card{upper(trim(keyword.name)), {}, keyword.comment, ValueType::Undefined};
    if (card.name.rfind("HIERARCH ", 0) == 0) card.name = trim(card.name.substr(9));
    const auto raw = trim(keyword.value);
    if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
        card.type = ValueType::String; card.value = decodeLiteral(raw);
    } else if (raw.empty()) card.type = ValueType::Undefined;
    else {
        card.type = knownString(card.name) ? ValueType::String : scalarType(raw);
        card.value = card.type == ValueType::String ? keyword.value : upper(raw);
    }
    return card;
}

Metadata prepareMetadata(const Source& source, bool bottomUp) {
    Metadata metadata;
    std::map<std::string, size_t> indexes;
    auto add = [&](Card card) {
        normalize(card, metadata);
        if (card.type != ValueType::Commentary) {
            prefix(card.name);
            auto existing = indexes.find(card.name);
            if (existing != indexes.end()) {
                const auto& old = metadata.cards[existing->second];
                if (old.type != card.type || old.value != card.value || old.comment != card.comment)
                    fail("conflicting duplicate keyword: " + card.name);
                metadata.warnings.push_back("collapsed identical duplicate keyword: " + card.name);
                return;
            }
            indexes[card.name] = metadata.cards.size();
        }
        metadata.cards.push_back(std::move(card));
    };
    for (size_t i = 0; i < source.keywords.size(); ++i) {
        const auto& keyword = source.keywords[i];
        auto name = upper(trim(keyword.name));
        if (name.rfind("HIERARCH ", 0) == 0) name = trim(name.substr(9));
        if (bottomUp && wcsKeyword(name)) fail("cannot flip rows of an image with FITS WCS keywords");
        if (structuralKeyword(name)) {
            metadata.warnings.push_back("regenerated structural keyword: " + name);
            continue;
        }
        if (name == "CONTINUE") fail("standalone source CONTINUE card; provide the complete XISF keyword value");
        if (name == "COMMENT" || name == "HISTORY" || name.empty()) {
            add({name, keyword.value.empty() ? keyword.comment : keyword.value, {}, ValueType::Commentary});
            if (!keyword.value.empty() && !keyword.comment.empty() && keyword.value != keyword.comment)
                add({name, keyword.comment, {}, ValueType::Commentary});
        } else {
            auto card = parseKeyword(keyword);
            while (card.type == ValueType::String && !card.value.empty() && card.value.back() == '&' &&
                   i + 1 < source.keywords.size() && upper(trim(source.keywords[i + 1].name)) == "CONTINUE") {
                auto next = parseKeyword(source.keywords[++i]);
                if (next.type != ValueType::String) fail("non-string source CONTINUE card");
                card.value.pop_back();
                card.value += next.value;
                if (!next.comment.empty() && next.comment != card.comment)
                    card.comment += (card.comment.empty() ? "" : " ") + next.comment;
            }
            // Astropy treats every NAXIS-prefixed name as an indexed axis,
            // even when its suffix is not numeric. Retain such custom values
            // in a namespace rather than generating an unreadable header.
            if (card.name.rfind("NAXIS", 0) == 0 || card.name.rfind("ZNAXIS", 0) == 0) {
                metadata.warnings.push_back("namespaced custom keyword: " + card.name + " -> XISF.FITS." + card.name);
                card.name = "XISF.FITS." + card.name;
            }
            add(std::move(card));
        }
    }

    // Property values retain their XML decimal representation, not a rounded
    // libXISF Variant or a reformatted double. Ordinary capture keys stay intact.
    const std::map<std::string, std::pair<std::string, long double>> aliases = {
        {"Observer:Name", {"OBSERVER", 1}}, {"Instrument:Camera:Name", {"INSTRUME", 1}},
        {"Instrument:Telescope:Name", {"TELESCOP", 1}}, {"Instrument:Telescope:Aperture", {"APTDIA", 1000}},
        {"Instrument:Telescope:FocalLength", {"FOCALLEN", 1000}}, {"Instrument:Filter:Name", {"FILTER", 1}},
        {"Instrument:ExposureTime", {"EXPTIME", 1}}, {"Instrument:Focuser:Position", {"FOCUSPOS", 1}},
        {"Instrument:Sensor:Temperature", {"CCD-TEMP", 1}}, {"Instrument:Sensor:XPixelSize", {"XPIXSZ", 1}},
        {"Instrument:Sensor:YPixelSize", {"YPIXSZ", 1}}, {"Instrument:Camera:XBinning", {"XBINNING", 1}},
        {"Instrument:Camera:YBinning", {"YBINNING", 1}}, {"Observation:Object:Name", {"OBJECT", 1}},
        {"Observation:Time:Start", {"DATE-OBS", 1}}, {"Observation:Location:Latitude", {"SITELAT", 1}},
        {"Observation:Location:Longitude", {"SITELONG", 1}}, {"Observation:Location:Elevation", {"SITEELEV", 1}},
        {"Observation:Center:RA", {"RA", 1}}, {"Observation:Center:Dec", {"DEC", 1}},
        {"Observation:Equinox", {"EQUINOX", 1}}
    };
    std::set<std::string> propertyNames;
    for (const auto& property : source.properties) {
        auto id = upper(property.id);
        std::replace(id.begin(), id.end(), ':', '.');
        Card card{"XISF." + property.scope + "." + id, property.value, property.comment, ValueType::String};
        if (!propertyNames.insert(card.name).second || indexes.count(card.name)) fail("property keyword collision: " + card.name);
        if (property.type == "Boolean") {
            auto value = upper(trim(property.value));
            if (value == "TRUE" || value == "1") card.value = "T";
            else if (value == "FALSE" || value == "0") card.value = "F";
            else fail("invalid Boolean property: " + property.id);
            card.type = ValueType::Logical;
        } else if (property.type == "String" || property.type == "TimePoint") card.type = ValueType::String;
        else if (property.type == "Int8" || property.type == "Int16" || property.type == "Int32" || property.type == "Int64" ||
                 property.type == "UInt8" || property.type == "UInt16" || property.type == "UInt32" || property.type == "UInt64" ||
                 property.type == "Float32" || property.type == "Float64") {
            card.value = upper(trim(card.value));
            card.type = scalarType(card.value);
            if (!numeric(card.type) || !std::isfinite(number(card.value)) ||
                (property.type.find("Int") != std::string::npos && card.type != ValueType::Integer))
                fail("invalid numeric property: " + property.id);
            if (property.type.find("Int") != std::string::npos) {
                std::string value = card.value;
                if (value.front() == '+') value.erase(0, 1);
                bool isUnsigned = property.type.front() == 'U';
                auto bits = unsignedNumber(property.type.substr(isUnsigned ? 4 : 3), "integer property width");
                if (isUnsigned) {
                    auto n = unsignedNumber(value, "unsigned property " + property.id);
                    if (n > (std::numeric_limits<uint64_t>::max() >> (64 - bits))) fail("integer property out of range: " + property.id);
                } else {
                    int64_t n = 0;
                    auto parsed = std::from_chars(value.data(), value.data() + value.size(), n);
                    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                        (bits < 64 && (n < -(int64_t{1} << (bits - 1)) || n >= (int64_t{1} << (bits - 1)))))
                        fail("integer property out of range: " + property.id);
                }
            } else {
                if (std::fabs(number(card.value)) > (property.type == "Float32" ?
                    static_cast<long double>(std::numeric_limits<float>::max()) : static_cast<long double>(std::numeric_limits<double>::max())))
                    fail("floating property out of range: " + property.id);
                // XML type is authoritative for properties: "0" in a Float32
                // property must not silently become a FITS integer keyword.
                if (card.type == ValueType::Integer) card.value += ".0";
                card.type = ValueType::Real;
            }
        } else {
            metadata.warnings.push_back("omitted unsupported property " + property.id + " (" + property.type + ")");
            continue;
        }
        if (property.id.empty()) fail("empty XISF property id");
        try {
            const auto p = prefix(card.name);
            if (p.size() + (card.type == ValueType::String ? 3 : card.value.size()) > 80)
                fail("property does not fit a FITS card");
        } catch (const std::runtime_error&) {
            metadata.warnings.push_back("omitted unrepresentable property: " + property.id);
            continue;
        }
        auto alias = aliases.find(property.id);
        if (property.scope == "IMAGE" && alias != aliases.end() && indexes.count(alias->second.first)) {
            const auto& old = metadata.cards[indexes.at(alias->second.first)];
            bool equal = old.value == card.value;
            if (numeric(old.type) && numeric(card.type)) {
                const auto a = number(old.value), b = number(card.value) * alias->second.second;
                equal = std::fabs(a - b) <= 1e-12L * std::max({1.0L, std::fabs(a), std::fabs(b)});
            }
            if (!equal) metadata.warnings.push_back("property conflicts with " + old.name + ": " + property.id + "; retained both");
        }
        add(std::move(card));
    }

    auto find = [&](const std::string& name) -> const Card* {
        auto it = indexes.find(name);
        return it == indexes.end() ? nullptr : &metadata.cards[it->second];
    };
    const auto* patternCard = find("BAYERPAT");
    std::string pattern = source.cfa;
    if (patternCard) {
        // Trailing blanks are insignificant in FITS strings; CFITSIO pads to 8.
        pattern = upper(patternCard->value);
        pattern.erase(pattern.find_last_not_of(' ') + 1);
    }
    auto parity = [](const Card* card) {
        if (card->type != ValueType::Integer) fail("invalid Bayer offset");
        // Only parity matters; avoid narrowing arbitrary integer strings.
        return (card->value.back() - '0') % 2;
    };
    auto offset = [&](const char* key, const char* alternate) {
        const auto* a = find(key); const auto* b = find(alternate);
        if (a && b && parity(a) != parity(b)) fail("conflicting Bayer offsets");
        const auto* c = a ? a : b;
        return c ? parity(c) : 0;
    };
    const int x = offset("XBAYROFF", "XBAYEROFF"), y = offset("YBAYROFF", "YBAYEROFF");
    if (!pattern.empty()) {
        static const std::set<std::string> patterns = {"RGGB", "BGGR", "GRBG", "GBRG"};
        if (!patterns.count(pattern) || (!source.cfa.empty() && !patterns.count(source.cfa))) fail("unsupported Bayer pattern");
        // An XML CFA already describes the first stored pixel; FITS offsets
        // apply only to BAYERPAT. Without it, a nonzero offset is ambiguous.
        if (!patternCard && (x || y)) fail("Bayer offsets are ambiguous with an XML CFA and no BAYERPAT");
        std::string effective = pattern;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col) effective[row * 2 + col] = pattern[((row + y) % 2) * 2 + (col + x) % 2];
        if (patternCard && !source.cfa.empty() && effective != source.cfa) fail("XML CFA conflicts with FITS Bayer metadata");
        if (bottomUp && source.height % 2 == 0) effective = effective.substr(2) + effective.substr(0, 2);
        metadata.cards.erase(std::remove_if(metadata.cards.begin(), metadata.cards.end(), [](const Card& c) {
            return c.name == "BAYERPAT" || c.name == "XBAYROFF" || c.name == "YBAYROFF" || c.name == "XBAYEROFF" || c.name == "YBAYEROFF";
        }), metadata.cards.end());
        metadata.cards.push_back({"BAYERPAT", effective, "Bayer pattern at first output pixel", ValueType::String});
        metadata.cards.push_back({"XBAYROFF", "0", "Bayer X offset in output", ValueType::Integer});
        metadata.cards.push_back({"YBAYROFF", "0", "Bayer Y offset in output", ValueType::Integer});
    } else if (find("XBAYROFF") || find("YBAYROFF") || find("XBAYEROFF") || find("YBAYEROFF")) fail("Bayer offsets without a pattern");
    metadata.cards.push_back({"ROWORDER", bottomUp ? "BOTTOM-UP" : "TOP-DOWN", "Pixel row order in output image", ValueType::String});
    return metadata;
}

std::vector<std::string> headerRecords(Metadata& metadata) {
    std::vector<std::string> records;
    bool longStrings = false;
    auto emit = [&](std::string record) {
        if (record.size() > 80) fail("internal FITS card overflow");
        record.resize(80, ' '); records.push_back(std::move(record));
    };
    auto comment = [&](std::string record, const Card& card) {
        if (!card.comment.empty()) {
            const size_t available = record.size() + 3 <= 80 ? 80 - record.size() - 3 : 0;
            if (card.comment.size() > available) metadata.warnings.push_back("truncated header comment: " + card.name);
            if (available) record += " / " + card.comment.substr(0, available);
        }
        emit(std::move(record));
    };
    for (const auto& card : metadata.cards) {
        checkpoint();
        if (card.type == ValueType::Commentary) {
            size_t pos = 0;
            do {
                emit(card.name + std::string(8 - card.name.size(), ' ') + card.value.substr(pos, 72));
                pos += std::min(size_t{72}, card.value.size() - pos);
            } while (pos < card.value.size());
            continue;
        }
        auto p = prefix(card.name);
        if (card.type != ValueType::String) {
            std::string value = card.value;
            if (shortName(card.name) && value.size() < 20) value.insert(0, 20 - value.size(), ' ');
            if (p.size() + value.size() > 80) fail("FITS numeric value too long: " + card.name);
            comment(p + value, card);
            continue;
        }
        // Split logical characters, never between the two apostrophes of an
        // escaped quote. An actual trailing '&' needs an empty final CONTINUE.
        size_t pos = 0;
        bool done = false;
        do {
            if (p.size() + 3 > 80) fail("keyword name leaves no room for a string: " + card.name);
            std::string encoded;
            const auto start = pos;
            const size_t capacity = 80 - p.size() - 2;
            while (pos < card.value.size()) {
                size_t cost = card.value[pos] == '\'' ? 2 : 1;
                if (encoded.size() + cost > capacity - 1) break;
                encoded += card.value[pos];
                if (card.value[pos] == '\'') encoded += '\'';
                ++pos;
            }
            done = pos == card.value.size() && (encoded.empty() || encoded.back() != '&');
            // Use the final byte when no continuation marker is needed.
            if (!done && pos + 1 == card.value.size() && card.value[pos] != '&' &&
                encoded.size() + (card.value[pos] == '\'' ? 2 : 1) <= capacity) {
                encoded += card.value[pos];
                if (card.value[pos] == '\'') encoded += '\'';
                ++pos; done = true;
            }
            if (done) comment(p + "'" + encoded + "'", card);
            else {
                if (pos == start) fail("keyword name leaves no room for string continuation: " + card.name);
                emit(p + "'" + encoded + "&'"); longStrings = true;
                p = "CONTINUE  ";
            }
        } while (!done);
    }
    if (longStrings) {
        std::string declaration = "LONGSTRN= 'OGIP 1.0'";
        declaration.resize(80, ' ');
        records.insert(records.begin(), declaration);
    }
    return records;
}
}
