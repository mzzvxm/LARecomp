#ifndef REXGLUE_HAS_XEO3_TARGET
#include "string_table.h"
#include "logging.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/string_dumper.h>

namespace {

constexpr uint32_t kStringTableGlobal = 0x8286D7FC;

uint8_t* GetMembase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

uint32_t ReadBE32(uint8_t* base, uint32_t ea) {
    uint8_t* p = base + ea;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  | uint32_t(p[3]);
}

uint16_t ReadBE16(uint8_t* base, uint32_t ea) {
    uint8_t* p = base + ea;
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

struct ScanStats {
    uint32_t dumped = 0;
    uint32_t replaced = 0;
    uint32_t skipped = 0;
};

ScanStats ScanHashMap(uint8_t* base, uint32_t hashmap_ea,
                      rex::system::StringDumper& dumper,
                      bool do_dump, bool do_replace) {
    ScanStats stats{};
    uint32_t buckets_ptr = ReadBE32(base, hashmap_ea);
    uint16_t num_buckets = ReadBE16(base, hashmap_ea + 4);

    if (!num_buckets || !buckets_ptr) return stats;

    for (uint16_t i = 0; i < num_buckets; i++) {
        uint32_t node = ReadBE32(base, buckets_ptr + 4 * i);
        while (node) {
            uint32_t game_hash = ReadBE32(base, node);
            uint32_t value_ptr = ReadBE32(base, node + 4);

            if (value_ptr) {
                const char* text = reinterpret_cast<const char*>(base + value_ptr);
                size_t len = strnlen(text, 131);

                if (len > 0) {
                    std::string_view sv(text, len);
                    char ctx_buf[32];
                    std::snprintf(ctx_buf, sizeof(ctx_buf), "mcla:%08X", game_hash);
                    std::string context(ctx_buf);

                    if (do_dump) {
                        dumper.DumpString(context, sv, value_ptr);
                        stats.dumped++;
                    }

                    if (do_replace) {
                        std::string replacement = dumper.FindReplacement(context, sv);
                        if (!replacement.empty()) {
                            size_t rlen = replacement.size();
                            if (rlen > 131) rlen = 131;
                            std::memcpy(base + value_ptr, replacement.data(), rlen);
                            base[value_ptr + rlen] = 0;
                            stats.replaced++;
                        }
                    }
                } else {
                    stats.skipped++;
                }
            }

            node = ReadBE32(base, node + 8);
        }
    }
    return stats;
}

void ScanMCLAStringTable(bool trigger_rescan = false) {
    uint8_t* base = GetMembase();
    if (!base) return;

    uint32_t table = ReadBE32(base, kStringTableGlobal);
    if (!table) return;

    auto* ks = rex::system::kernel_state();
    if (!ks) return;
    auto& dumper = ks->string_dumper();

    bool do_dump = REXCVAR_GET(string_dump_enabled);
    bool do_replace = REXCVAR_GET(string_replace_enabled);

    if (!do_dump && !do_replace) return;

    if (do_replace && trigger_rescan) {
        dumper.Rescan();
    }

    if (trigger_rescan) {
        MC_INFO("[string-table] scanning MCLA table at 0x{:08X} (dump={}, replace={})...",
                table, do_dump, do_replace);
    }

    ScanStats display = ScanHashMap(base, table + 16, dumper, do_dump, do_replace);
    ScanStats source  = ScanHashMap(base, table + 44, dumper, do_dump, false);

    if (do_dump) {
        dumper.FlushDump();
        // Log condicional para não floodar o terminal a cada tick da thread
        if (display.dumped > 0 || source.dumped > 0) {
            MC_INFO("[string-table] dumped {} display + {} source strings",
                    display.dumped, source.dumped);
        }
    }
    if (do_replace && display.replaced > 0) {
        // Log condicional para evitar flood
        MC_INFO("[string-table] replaced {} display strings", display.replaced);
    }
}

}  // anonymous namespace

void InitStringTableTools() {
    MC_INFO("[string-table] MCLA bridge registered. Background dumper active.");

    // Thread separada que faz o dump de forma contínua enquanto ativo
    std::thread([]() {
        bool was_replace_enabled = false;
        
        while (true) {
            bool do_dump = REXCVAR_GET(string_dump_enabled);
            bool do_replace = REXCVAR_GET(string_replace_enabled);

            // Trigger para garantir que o Rescan() só ocorra 1 vez quando ativado, 
            // e não a cada 500ms estourando o I/O do disco.
            bool trigger_rescan = false;
            if (do_replace && !was_replace_enabled) {
                trigger_rescan = true;
            }
            was_replace_enabled = do_replace;

            if (do_dump || do_replace) {
                ScanMCLAStringTable(trigger_rescan);
            }
            
            // Pausa de 500ms entre scans para não travar o uso da CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

#endif // REXGLUE_HAS_XEO3_TARGET
