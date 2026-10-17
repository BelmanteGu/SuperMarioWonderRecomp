// On-device importer: the player's own keys + NSPs / XCIs (gamecard images) -> the data folder the runtime loads
//   <out>/analysis/<module>.bin   flat text|ro|data images of the ExeFS NSOs (tools/nso2bin.py's format)
//   <out>/analysis/main.npdm
//   <out>/extract/romfs/...       the RomFS, with the update's patch (BKTR) applied over the base (sparse or not)
//   <out>/.import/                progress journal (resume), the CRC index of every output file
// Everything streams (no temporary copy of a section), every RomFS block is checked against the NCA's IVFC SHA-256
// tree and every NSO segment against its hash, so a wrong key or a damaged dump stops with an error instead of
// writing garbage. Key values never reach a log line or an error message (only key names).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nxi {

struct InputFile {
    std::string name;  // file name (no folder): *.nsp, *.xci (or split parts .xc0 .xc1 / .xci.00 .xci.01), prod.keys, title.keys
    int fd = -1;       // readable with pread (a regular file or a SAF document descriptor); the importer does not close it
};

struct Progress {
    std::string phase;  // "scan", "exefs", "romfs", "verify", "done"
    uint64_t done = 0, total = 0;
    std::string item;   // file being written
};

struct Options {
    std::string out_dir;
    uint64_t title_id = 0;  // the application (base) title; 0: the only one found
    bool verify_only = false;  // re-read the outputs and compare with the CRC index
    bool keep_exefs = false;   // also write the raw NSOs to <out>/exefs/ (the input of python -m recomp)
    std::atomic<bool>* cancel = nullptr;
    std::function<void(const Progress&)> on_progress;  // throttled by the caller if needed
    std::function<void(const std::string&)> on_log;
};

// 0 on success; else the error message is in `error` (no key material in it).
int run_import(const std::vector<InputFile>& inputs, const Options& opt, std::string& error);

}  // namespace nxi
