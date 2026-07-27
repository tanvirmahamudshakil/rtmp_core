#pragma once

// Shared standalone driver for this project's fuzz harnesses.
//
// Why this exists. Every harness here compiles against libFuzzer when
// -DRTMP_SERVER_ENABLE_FUZZING=ON, which is the right tool: coverage-guided,
// with ASan wired in. But libFuzzer ships in LLVM's compiler-rt, and Apple
// Clang on macOS does not install libclang_rt.fuzzer_osx.a — the link fails
// with "library ... libclang_rt.fuzzer_osx.a not found". This project's
// development host is macOS, so on that host the harnesses would build as
// corpus replayers and never actually be *run* against anything.
//
// "Wire up a fuzzer and never execute it" is exactly the fake-completion this
// project's engineering rules forbid, so this driver provides a real,
// executable fuzzing mode with no engine dependency: a seeded, deterministic
// mutation fuzzer. It is strictly weaker than libFuzzer (no coverage
// feedback, so it explores by mutation pressure alone), but it does genuinely
// execute millions of mutated inputs through the parsers under ASan/UBSan and
// will find memory errors, and it is fully reproducible from its seed.
//
// On Linux CI, prefer the libFuzzer build. Use this driver where libFuzzer is
// unavailable, and for replaying a saved crash input as a regression test.
//
// Usage:
//   <harness> <file> [<file> ...]      replay corpus files (no mutation)
//   <harness> --runs N [--seed S] [--max-len L] [--verbose]
//
// A harness opts in by defining RTMP_SERVER_FUZZ_SEED_CORPUS to a function
// returning std::vector<std::vector<std::uint8_t>> before including this
// header; the driver mutates those seeds instead of starting from noise.
// Structure-aware seeds matter a great deal here: a purely random byte string
// is rejected by the first type-marker check in almost every case, so without
// seeds the fuzzer would exercise only the error path.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace rtmp_server_fuzz {

using Input = std::vector<std::uint8_t>;

// Values that disproportionately often sit at a boundary in length/count
// fields: zero, all-ones at each width, and the signed/unsigned edges.
inline const std::uint8_t kInterestingBytes[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x20, 0x0A};

class Mutator {
public:
    explicit Mutator(std::uint64_t seed) : rng_(seed) {}

    std::size_t pick(std::size_t n) { return n == 0 ? 0 : std::uniform_int_distribution<std::size_t>(0, n - 1)(rng_); }

    std::uint8_t byte() { return static_cast<std::uint8_t>(std::uniform_int_distribution<int>(0, 255)(rng_)); }

    // Applies one to four mutations to `data`, bounded by `max_len`.
    void mutate(Input& data, std::size_t max_len) {
        const int rounds = 1 + static_cast<int>(pick(4));
        for (int r = 0; r < rounds; ++r) {
            switch (pick(7)) {
                case 0: // flip a single bit
                    if (!data.empty()) data[pick(data.size())] ^= static_cast<std::uint8_t>(1u << pick(8));
                    break;
                case 1: // overwrite a byte with a random value
                    if (!data.empty()) data[pick(data.size())] = byte();
                    break;
                case 2: // overwrite a byte with a boundary value
                    if (!data.empty()) {
                        data[pick(data.size())] = kInterestingBytes[pick(sizeof(kInterestingBytes))];
                    }
                    break;
                case 3: // insert a run of bytes (grows length fields' operands)
                    if (data.size() < max_len) {
                        const std::size_t at = pick(data.size() + 1);
                        const std::size_t n = 1 + pick(16);
                        Input run(n);
                        for (auto& b : run) b = byte();
                        data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), run.begin(), run.end());
                        if (data.size() > max_len) data.resize(max_len);
                    }
                    break;
                case 4: // erase a run of bytes (truncation / partial-message paths)
                    if (data.size() > 1) {
                        const std::size_t at = pick(data.size());
                        const std::size_t n = 1 + pick(data.size() - at);
                        data.erase(data.begin() + static_cast<std::ptrdiff_t>(at),
                                   data.begin() + static_cast<std::ptrdiff_t>(at + n));
                    }
                    break;
                case 5: // duplicate a slice (drives repetition/nesting depth)
                    if (!data.empty() && data.size() < max_len) {
                        const std::size_t at = pick(data.size());
                        const std::size_t n = 1 + pick(data.size() - at);
                        const Input slice(data.begin() + static_cast<std::ptrdiff_t>(at),
                                          data.begin() + static_cast<std::ptrdiff_t>(at + n));
                        data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), slice.begin(), slice.end());
                        if (data.size() > max_len) data.resize(max_len);
                    }
                    break;
                default: // splice a 32-bit boundary value over a 4-byte field
                    if (data.size() >= 4) {
                        const std::size_t at = pick(data.size() - 3);
                        for (std::size_t i = 0; i < 4; ++i) {
                            data[at + i] = kInterestingBytes[pick(sizeof(kInterestingBytes))];
                        }
                    }
                    break;
            }
        }
    }

private:
    std::mt19937_64 rng_;
};

inline int replay_files(int argc, char** argv) {
    int replayed = 0;
    for (int i = 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", argv[i]);
            return 2;
        }
        const Input data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        LLVMFuzzerTestOneInput(data.data(), data.size());
        ++replayed;
    }
    std::printf("replayed %d corpus file(s) without crashing\n", replayed);
    return 0;
}

inline int run(int argc, char** argv, const std::vector<Input>& seeds, const char* name) {
    std::uint64_t runs = 0;
    std::uint64_t seed = 0x5eed'1234'abcd'0001ULL;
    std::size_t max_len = 4096;
    bool verbose = false;
    bool have_runs = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::uint64_t& out) {
            if (i + 1 < argc) out = std::strtoull(argv[++i], nullptr, 10);
        };
        if (arg == "--runs") {
            next(runs);
            have_runs = true;
        } else if (arg == "--seed") {
            next(seed);
        } else if (arg == "--max-len") {
            std::uint64_t v = max_len;
            next(v);
            max_len = static_cast<std::size_t>(v);
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 2;
        } else {
            return replay_files(argc, argv); // any positional argument means corpus replay
        }
    }

    if (!have_runs) {
        std::printf(
            "usage: %s <corpus-file>...            replay saved inputs\n"
            "       %s --runs N [--seed S] [--max-len L] [--verbose]\n",
            name, name);
        return 2;
    }

    // Always execute the seed corpus verbatim first: those inputs are valid
    // by construction, so a failure there is a plain functional bug rather
    // than something the mutator happened upon.
    for (const auto& s : seeds) LLVMFuzzerTestOneInput(s.data(), s.size());

    Mutator mutator(seed);
    const auto started = std::chrono::steady_clock::now();
    Input scratch;
    std::uint64_t total_bytes = 0;

    for (std::uint64_t i = 0; i < runs; ++i) {
        // Restart from a fresh seed periodically so the corpus does not
        // degenerate into one long random walk away from valid structure.
        if (scratch.empty() || (i % 64) == 0) {
            scratch = seeds.empty() ? Input{} : seeds[mutator.pick(seeds.size())];
        }
        mutator.mutate(scratch, max_len);
        total_bytes += scratch.size();
        LLVMFuzzerTestOneInput(scratch.data(), scratch.size());
        if (verbose && (i % 100000) == 0 && i > 0) std::printf("  ... %llu runs\n", static_cast<unsigned long long>(i));
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%s: %llu executions, %llu seed inputs, %.2f MB mutated, %.2fs (%.0f exec/s), no crashes\n", name,
                static_cast<unsigned long long>(runs), static_cast<unsigned long long>(seeds.size()),
                static_cast<double>(total_bytes) / (1024.0 * 1024.0), elapsed,
                elapsed > 0 ? static_cast<double>(runs) / elapsed : 0.0);
    return 0;
}

} // namespace rtmp_server_fuzz

// Defines main() for the standalone (non-libFuzzer) build. `seed_fn` is a
// callable returning std::vector<rtmp_server_fuzz::Input>.
#ifndef RTMP_SERVER_FUZZING_ENGINE_LIBFUZZER
#define RTMP_SERVER_FUZZ_MAIN(name, seed_fn)                              \
    int main(int argc, char** argv) {                                     \
        return rtmp_server_fuzz::run(argc, argv, (seed_fn)(), (name));    \
    }
#else
#define RTMP_SERVER_FUZZ_MAIN(name, seed_fn)
#endif
