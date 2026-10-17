// The console language, shared by the am (appletOE) and set (settings) services.
//
// The player picks it in <data root>/language.txt or with NX_LANGUAGE; both take the Switch's own code (es, es-419,
// en-US, ...) or a readable name (spanish, castellano, latino, ...). NN::settings::LanguageCode packs the code as a
// short ASCII string, little endian, and the console only accepts the codes below (Ryujinx's
// Ryujinx.Horizon/Sdk/Settings/LanguageCode.cs): note "es", "ja", "fr", ... are two letters, not "es-ES".
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "platform.h"

namespace nx {

inline uint64_t pack_language_code(const char* code) {
    char b[8] = {0};
    for (int i = 0; i < 8 && code[i]; i++) b[i] = code[i];
    uint64_t v = 0;
    std::memcpy(&v, b, sizeof(v));
    return v;
}

inline std::string language_lower_trimmed(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    size_t a = s.find_first_not_of(" \t\r\n\"'");
    size_t b = s.find_last_not_of(" \t\r\n\"'");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The Switch's nn::settings::Language order and its LanguageCode strings.
inline const char* language_code_for_enum(uint32_t language) {
    static const char* kCodes[] = {
        "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl",
        "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR",
    };
    constexpr uint32_t kCount = uint32_t(sizeof(kCodes) / sizeof(kCodes[0]));
    return kCodes[language < kCount ? language : 1];
}

// First token of a file ("es\n" -> "es"), lowercase; "" if the file is missing.
inline std::string language_read_file(const std::string& path) {
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return "";
    char buf[64] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string s = language_lower_trimmed(std::string(buf, n));
    size_t sp = s.find_first_of(" \t\r\n;");
    return sp == std::string::npos ? s : s.substr(0, sp);
}

// The language the player configured: NX_LANGUAGE (config.txt on Android), else <data root>/language.txt, else en-US.
inline uint64_t desired_language() {
    std::string value = language_lower_trimmed(plat::setting("language"));
    if (value.empty()) value = language_read_file(plat::data_root() + "/language.txt");
    struct Entry { const char* code; const char* names; };
    static const Entry kTable[] = {
        {"en-US", "en en-us english americanenglish ingles"},
        {"en-GB", "en-gb britishenglish british"},
        {"ja", "ja ja-jp japanese japones"},  // "ja-JP" is accepted as an alias by the player, not by the console
        {"fr", "fr fr-fr french frances"},
        {"fr-CA", "fr-ca canadianfrench"},
        {"de", "de de-de german aleman"},
        {"it", "it it-it italian italiano"},
        {"es", "es es-es spanish castellano espanol espana"},
        {"es-419", "es-419 latam latino latinamerican spanishlatam latinoamerica"},
        {"nl", "nl nl-nl dutch neerlandes"},
        {"pt", "pt pt-pt portuguese portugues"},
        {"pt-BR", "pt-br brazilian portuguese-brazil brasil"},
        {"ru", "ru ru-ru russian ruso"},
        {"ko", "ko ko-kr korean coreano"},
        // The game knows the Chinese texts by zh-Hans / zh-Hant only: zh-CN / zh-TW alone send it to Japanese.
        {"zh-Hans", "zh-hans zh-cn chinesesimplified chinosimplificado"},
        {"zh-Hant", "zh-hant zh-tw chinesetraditional chinotradicional"},
    };
    for (const Entry& e : kTable) {
        std::string names = e.names;
        for (size_t p = 0, q; p <= names.size(); p = q + 1) {
            q = names.find(' ', p);
            size_t len = (q == std::string::npos) ? names.size() - p : q - p;
            if (names.compare(p, len, value) == 0 && len == value.size()) return pack_language_code(e.code);
            if (q == std::string::npos) break;
        }
    }
    // A raw code such as "es-ES" (an alias) or "en-us": canonicalize the case and, if it is a known alias, use the
    // console's own spelling.
    static const Entry kAlias[] = {
        {"ja", "ja-jp"}, {"fr", "fr-fr"}, {"de", "de-de"}, {"it", "it-it"}, {"es", "es-es"}, {"nl", "nl-nl"},
        {"pt", "pt-pt"}, {"ru", "ru-ru"}, {"ko", "ko-kr"},
    };
    for (const Entry& e : kAlias)
        if (value == e.names) return pack_language_code(e.code);
    if (value.size() == 5 && value[2] == '-') {
        std::string c = value;
        c[0] = char(std::toupper(static_cast<unsigned char>(c[0])));
        c[1] = char(std::toupper(static_cast<unsigned char>(c[1])));
        c[3] = char(std::toupper(static_cast<unsigned char>(c[3])));
        c[4] = char(std::toupper(static_cast<unsigned char>(c[4])));
        return pack_language_code(c.c_str());
    }
    if (value.size() == 2) return pack_language_code(value.c_str());
    return pack_language_code("en-US");
}

}  // namespace nx
