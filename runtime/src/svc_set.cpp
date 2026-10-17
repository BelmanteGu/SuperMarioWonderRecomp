// set (nn::settings): the console language and the LanguageCode helpers.
//
// The game asks set for the console language and, for every nn::settings::Language it knows, converts it to a
// LanguageCode (MakeLanguageCode) to compare against nn::oe::GetDesiredLanguage (svc_am.cpp); it also reads the
// available-language list. The codes are the Switch's own (language.h). Without this the answers are zero and the
// game's language handling falls back to Japanese.
#include <cstdint>

#include "ipc.h"
#include "language.h"

namespace nx {

namespace {

// nn::settings::Language order: ja, en-US, fr, de, it, es, zh-CN, ko, nl, pt, ru, zh-TW, en-GB, fr-CA, es-419,
// zh-Hans, zh-Hant, pt-BR.
constexpr int32_t kLanguageCount = 18;

// GetAvailableLanguageCodes(2): the count in the response, the codes in the client's out array.
uint32_t push_available(IpcCtx& c) {
    Buffer out = c.out_buf(0);
    if (out.addr && out.size >= size_t(kLanguageCount) * 8)
        for (int32_t k = 0; k < kLanguageCount; k++)
            st<uint64_t>(out.addr + uint64_t(k) * 8, pack_language_code(language_code_for_enum(uint32_t(k))));
    c.push<int32_t>(kLanguageCount);
    return 0u;
}

std::shared_ptr<Service> make(std::string name) {
    auto s = std::make_shared<Service>(std::move(name));
    s->cmds[0] = [](IpcCtx& c) { c.push<uint64_t>(desired_language()); return 0u; };  // GetLanguageCode
    s->cmds[1] = [](IpcCtx& c) { return push_available(c); };                          // GetAvailableLanguageCodes
    s->cmds[2] = [](IpcCtx& c) {                                                       // MakeLanguageCode(Language)
        c.push<uint64_t>(pack_language_code(language_code_for_enum(c.arg<uint32_t>(0))));
        return 0u;
    };
    s->cmds[3] = [](IpcCtx& c) { c.push<int32_t>(kLanguageCount); return 0u; };  // GetAvailableLanguageCodeCount
    s->cmds[4] = [](IpcCtx& c) { c.push<int32_t>(1); return 0u; };               // GetRegionCode (1 = Americas)
    s->cmds[5] = [](IpcCtx& c) { return push_available(c); };                    // GetAvailableLanguageCodes2
    s->cmds[6] = [](IpcCtx& c) { c.push<int32_t>(kLanguageCount); return 0u; };  // GetAvailableLanguageCodeCount2
    return s;
}

}  // namespace

ServiceRegistrar r_set("set", [] { return make("set"); });
ServiceRegistrar r_set_sys("set:sys", [] { return make("set:sys"); });
ServiceRegistrar r_set_u("set:u", [] { return make("set:u"); });

}  // namespace nx
