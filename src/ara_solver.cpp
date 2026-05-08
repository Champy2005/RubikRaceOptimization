#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <deque>
#include <fstream>
#include <atomic>
#include <iostream>
#include <mutex>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;

namespace {

constexpr int kInf = 1'000'000'000;
constexpr int kNoTarget = numeric_limits<int>::min();
constexpr uint16_t kBlankCell = numeric_limits<uint16_t>::max();

using PackedBoard = vector<uint16_t>;

struct Timer {
    chrono::steady_clock::time_point start = chrono::steady_clock::now();

    long long elapsed_ms() const {
        return chrono::duration_cast<chrono::milliseconds>(
                   chrono::steady_clock::now() - start)
            .count();
    }
};

struct Hash128 {
    uint64_t lo = 0;
    uint64_t hi = 0;

    bool operator==(const Hash128& other) const {
        return lo == other.lo && hi == other.hi;
    }
};

struct Hash128Hasher {
    size_t operator()(const Hash128& h) const {
        uint64_t x = h.lo ^ (h.hi + 0x9e3779b97f4a7c15ULL + (h.lo << 6) + (h.lo >> 2));
        if constexpr (sizeof(size_t) >= 8) {
            return static_cast<size_t>(x);
        } else {
            return static_cast<size_t>((x >> 32) ^ x);
        }
    }
};

uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t value_key(int value) {
    return static_cast<uint64_t>(static_cast<int64_t>(value)) ^ 0xd1b54a32d192ed03ULL;
}

Hash128 cell_hash(int pos, int value) {
    const uint64_t p = static_cast<uint64_t>(pos) + 0x632be59bd9b4e019ULL;
    const uint64_t v = value_key(value);
    return {
        splitmix64(p ^ (v * 0x9e3779b97f4a7c15ULL)),
        splitmix64((p * 0xbf58476d1ce4e5b9ULL) ^ (v + 0x94d049bb133111ebULL)),
    };
}

Hash128 xor_hash(Hash128 a, Hash128 b) {
    return {a.lo ^ b.lo, a.hi ^ b.hi};
}

uint16_t pack_cell(int value) {
    return value == -1 ? kBlankCell : static_cast<uint16_t>(value);
}

int unpack_cell(uint16_t value) {
    return value == kBlankCell ? -1 : static_cast<int>(value);
}

struct Options {
    string input_path;
    string output_path = "solution.out";
    string meta_path = "meta.json";
    string checkpoint_path;
    string resume_path;
    string validate_solution_path;
    string improvement_mode = "constructor-first";
    int time_limit_ms = 10'000;
    int snapshot_stride = 64;
    int patch_top_k = 100;
    int patch_attempt_ms = 40;
    int patch_threads = 0;
    int patch_batch_size = 0;
    int patch_batch_timeslice_ms = 30;
    int patch_force_5x5_every = 4;
    int patch_max_commits = 0;
    string patch_commit_policy = "best-batch";
    bool patch_stats = false;
    bool no_macro_resynthesis = false;
    string macro_strategy = "sweep";
    int macro_stall_ms = 15'000;
    int macro_time_ratio = 40;
    bool anytime_constructive = true;
    string anytime_engine = "monotone";
    int anytime_ratio = 60;
    uint64_t anytime_seed = 1;
    int anytime_configs = 0;
    int anytime_threads = 0;
    int anytime_attempt_ms = 0;
    int elite_size = 8;
    int rollout_depth = 2;
    int rollout_top_k = 4;
    int repair_jobs = 32;
    string constructor_engine = "auto";
    int constructor_threads = 0;
    int constructor_configs = 0;
    uint64_t constructor_seed = 1;
    int constructor_ratio = 0;
    int constructor_config_time_ms = 0;
    int constructor_repair_jobs = 0;
    int cleanup_ms = -1;
    bool no_cleanup = false;
    bool large_constructive = true;
    string large_engine = "off";
    int large_ratio = 70;
    int large_band_size = 4;
    int large_beam_width = 24;
    int large_band_candidates = 8;
    int large_suffix_jobs = 64;
    int large_constructor_ratio = 80;
    int large_patch_ratio = 10;
    int large_constructor_configs = 0;
    int large_constructor_min_ms = 30'000;
    int large_config_time_ms = 0;
    int large_repair_from_elites = 64;
    int large_patch_cleanup_ms = 5'000;
    bool no_large_patch_cleanup = false;
    uint64_t large_seed = 0;
    string constructive_mode = "paired";
    bool no_constructive = false;
    bool self_test = false;
};

bool starts_with(const string& s, const string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

string next_arg(int& i, int argc, char** argv, const string& flag) {
    if (i + 1 >= argc) {
        throw runtime_error("missing value for " + flag);
    }
    return argv[++i];
}

Options parse_options(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        auto value_after_equals = [&](const string& flag) -> string {
            return arg.substr(flag.size() + 1);
        };

        if (arg == "--self-test") {
            opt.self_test = true;
        } else if (arg == "--no-constructive") {
            opt.no_constructive = true;
        } else if (arg == "--improvement") {
            opt.improvement_mode = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--improvement=")) {
            opt.improvement_mode = value_after_equals("--improvement");
        } else if (arg == "--time-ms") {
            opt.time_limit_ms = stoi(next_arg(i, argc, argv, arg));
        } else if (starts_with(arg, "--time-ms=")) {
            opt.time_limit_ms = stoi(value_after_equals("--time-ms"));
        } else if (arg == "--snapshot-stride") {
            opt.snapshot_stride = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--snapshot-stride=")) {
            opt.snapshot_stride = max(1, stoi(value_after_equals("--snapshot-stride")));
        } else if (arg == "--patch-top-k") {
            opt.patch_top_k = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--patch-top-k=")) {
            opt.patch_top_k = max(1, stoi(value_after_equals("--patch-top-k")));
        } else if (arg == "--patch-attempt-ms") {
            opt.patch_attempt_ms = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--patch-attempt-ms=")) {
            opt.patch_attempt_ms = max(1, stoi(value_after_equals("--patch-attempt-ms")));
        } else if (arg == "--patch-threads") {
            opt.patch_threads = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--patch-threads=")) {
            opt.patch_threads = max(0, stoi(value_after_equals("--patch-threads")));
        } else if (arg == "--patch-batch-size") {
            opt.patch_batch_size = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--patch-batch-size=")) {
            opt.patch_batch_size = max(1, stoi(value_after_equals("--patch-batch-size")));
        } else if (arg == "--patch-batch-timeslice-ms") {
            opt.patch_batch_timeslice_ms = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--patch-batch-timeslice-ms=")) {
            opt.patch_batch_timeslice_ms = max(1, stoi(value_after_equals("--patch-batch-timeslice-ms")));
        } else if (arg == "--patch-force-5x5-every") {
            opt.patch_force_5x5_every = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--patch-force-5x5-every=")) {
            opt.patch_force_5x5_every = max(1, stoi(value_after_equals("--patch-force-5x5-every")));
        } else if (arg == "--patch-commit-policy") {
            opt.patch_commit_policy = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--patch-commit-policy=")) {
            opt.patch_commit_policy = value_after_equals("--patch-commit-policy");
        } else if (arg == "--patch-stats") {
            opt.patch_stats = true;
        } else if (arg == "--no-macro-resynthesis") {
            opt.no_macro_resynthesis = true;
        } else if (arg == "--macro-strategy") {
            opt.macro_strategy = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--macro-strategy=")) {
            opt.macro_strategy = value_after_equals("--macro-strategy");
        } else if (arg == "--macro-stall-ms") {
            opt.macro_stall_ms = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--macro-stall-ms=")) {
            opt.macro_stall_ms = max(1, stoi(value_after_equals("--macro-stall-ms")));
        } else if (arg == "--macro-time-ratio") {
            opt.macro_time_ratio = min(90, max(1, stoi(next_arg(i, argc, argv, arg))));
        } else if (starts_with(arg, "--macro-time-ratio=")) {
            opt.macro_time_ratio = min(90, max(1, stoi(value_after_equals("--macro-time-ratio"))));
        } else if (arg == "--anytime-constructive") {
            string value = next_arg(i, argc, argv, arg);
            if (value != "on" && value != "off") {
                throw runtime_error("--anytime-constructive must be on or off");
            }
            opt.anytime_constructive = value == "on";
        } else if (starts_with(arg, "--anytime-constructive=")) {
            string value = value_after_equals("--anytime-constructive");
            if (value != "on" && value != "off") {
                throw runtime_error("--anytime-constructive must be on or off");
            }
            opt.anytime_constructive = value == "on";
        } else if (arg == "--anytime-engine") {
            opt.anytime_engine = next_arg(i, argc, argv, arg);
            opt.constructor_engine = opt.anytime_engine;
        } else if (starts_with(arg, "--anytime-engine=")) {
            opt.anytime_engine = value_after_equals("--anytime-engine");
            opt.constructor_engine = opt.anytime_engine;
        } else if (arg == "--anytime-ratio") {
            opt.anytime_ratio = min(90, max(1, stoi(next_arg(i, argc, argv, arg))));
            opt.constructor_ratio = opt.anytime_ratio;
        } else if (starts_with(arg, "--anytime-ratio=")) {
            opt.anytime_ratio = min(90, max(1, stoi(value_after_equals("--anytime-ratio"))));
            opt.constructor_ratio = opt.anytime_ratio;
        } else if (arg == "--anytime-seed") {
            opt.anytime_seed = static_cast<uint64_t>(stoull(next_arg(i, argc, argv, arg)));
            opt.constructor_seed = opt.anytime_seed;
        } else if (starts_with(arg, "--anytime-seed=")) {
            opt.anytime_seed = static_cast<uint64_t>(stoull(value_after_equals("--anytime-seed")));
            opt.constructor_seed = opt.anytime_seed;
        } else if (arg == "--anytime-configs") {
            opt.anytime_configs = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.constructor_configs = opt.anytime_configs;
        } else if (starts_with(arg, "--anytime-configs=")) {
            opt.anytime_configs = max(0, stoi(value_after_equals("--anytime-configs")));
            opt.constructor_configs = opt.anytime_configs;
        } else if (arg == "--anytime-threads") {
            opt.anytime_threads = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.constructor_threads = opt.anytime_threads;
        } else if (starts_with(arg, "--anytime-threads=")) {
            opt.anytime_threads = max(0, stoi(value_after_equals("--anytime-threads")));
            opt.constructor_threads = opt.anytime_threads;
        } else if (arg == "--anytime-attempt-ms") {
            opt.anytime_attempt_ms = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.constructor_config_time_ms = opt.anytime_attempt_ms;
        } else if (starts_with(arg, "--anytime-attempt-ms=")) {
            opt.anytime_attempt_ms = max(0, stoi(value_after_equals("--anytime-attempt-ms")));
            opt.constructor_config_time_ms = opt.anytime_attempt_ms;
        } else if (arg == "--elite-size") {
            opt.elite_size = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--elite-size=")) {
            opt.elite_size = max(1, stoi(value_after_equals("--elite-size")));
        } else if (arg == "--rollout-depth") {
            opt.rollout_depth = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--rollout-depth=")) {
            opt.rollout_depth = max(0, stoi(value_after_equals("--rollout-depth")));
        } else if (arg == "--rollout-top-k") {
            opt.rollout_top_k = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--rollout-top-k=")) {
            opt.rollout_top_k = max(1, stoi(value_after_equals("--rollout-top-k")));
        } else if (arg == "--repair-jobs") {
            opt.repair_jobs = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.constructor_repair_jobs = opt.repair_jobs;
        } else if (starts_with(arg, "--repair-jobs=")) {
            opt.repair_jobs = max(0, stoi(value_after_equals("--repair-jobs")));
            opt.constructor_repair_jobs = opt.repair_jobs;
        } else if (arg == "--constructor-engine") {
            opt.constructor_engine = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--constructor-engine=")) {
            opt.constructor_engine = value_after_equals("--constructor-engine");
        } else if (arg == "--constructor-threads") {
            opt.constructor_threads = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--constructor-threads=")) {
            opt.constructor_threads = max(0, stoi(value_after_equals("--constructor-threads")));
        } else if (arg == "--constructor-configs") {
            opt.constructor_configs = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--constructor-configs=")) {
            opt.constructor_configs = max(0, stoi(value_after_equals("--constructor-configs")));
        } else if (arg == "--constructor-seed") {
            opt.constructor_seed = static_cast<uint64_t>(stoull(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--constructor-seed=")) {
            opt.constructor_seed = static_cast<uint64_t>(stoull(value_after_equals("--constructor-seed")));
        } else if (arg == "--constructor-ratio") {
            opt.constructor_ratio = min(95, max(1, stoi(next_arg(i, argc, argv, arg))));
        } else if (starts_with(arg, "--constructor-ratio=")) {
            opt.constructor_ratio = min(95, max(1, stoi(value_after_equals("--constructor-ratio"))));
        } else if (arg == "--constructor-config-time-ms") {
            opt.constructor_config_time_ms = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--constructor-config-time-ms=")) {
            opt.constructor_config_time_ms = max(0, stoi(value_after_equals("--constructor-config-time-ms")));
        } else if (arg == "--constructor-repair-jobs") {
            opt.constructor_repair_jobs = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--constructor-repair-jobs=")) {
            opt.constructor_repair_jobs = max(0, stoi(value_after_equals("--constructor-repair-jobs")));
        } else if (arg == "--cleanup-ms") {
            opt.cleanup_ms = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--cleanup-ms=")) {
            opt.cleanup_ms = max(0, stoi(value_after_equals("--cleanup-ms")));
        } else if (arg == "--no-cleanup") {
            opt.no_cleanup = true;
        } else if (arg == "--large-constructive") {
            string value = next_arg(i, argc, argv, arg);
            if (value != "on" && value != "off") {
                throw runtime_error("--large-constructive must be on or off");
            }
            opt.large_constructive = value == "on";
        } else if (starts_with(arg, "--large-constructive=")) {
            string value = value_after_equals("--large-constructive");
            if (value != "on" && value != "off") {
                throw runtime_error("--large-constructive must be on or off");
            }
            opt.large_constructive = value == "on";
        } else if (arg == "--large-engine") {
            opt.large_engine = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--large-engine=")) {
            opt.large_engine = value_after_equals("--large-engine");
        } else if (arg == "--large-ratio") {
            opt.large_ratio = min(90, max(1, stoi(next_arg(i, argc, argv, arg))));
        } else if (starts_with(arg, "--large-ratio=")) {
            opt.large_ratio = min(90, max(1, stoi(value_after_equals("--large-ratio"))));
        } else if (arg == "--large-band-size") {
            opt.large_band_size = min(8, max(2, stoi(next_arg(i, argc, argv, arg))));
        } else if (starts_with(arg, "--large-band-size=")) {
            opt.large_band_size = min(8, max(2, stoi(value_after_equals("--large-band-size"))));
        } else if (arg == "--large-beam-width") {
            opt.large_beam_width = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--large-beam-width=")) {
            opt.large_beam_width = max(1, stoi(value_after_equals("--large-beam-width")));
        } else if (arg == "--large-band-candidates") {
            opt.large_band_candidates = max(1, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--large-band-candidates=")) {
            opt.large_band_candidates = max(1, stoi(value_after_equals("--large-band-candidates")));
        } else if (arg == "--large-suffix-jobs") {
            opt.large_suffix_jobs = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--large-suffix-jobs=")) {
            opt.large_suffix_jobs = max(0, stoi(value_after_equals("--large-suffix-jobs")));
        } else if (arg == "--large-constructor-ratio") {
            opt.large_constructor_ratio = min(95, max(1, stoi(next_arg(i, argc, argv, arg))));
            opt.constructor_ratio = opt.large_constructor_ratio;
        } else if (starts_with(arg, "--large-constructor-ratio=")) {
            opt.large_constructor_ratio = min(95, max(1, stoi(value_after_equals("--large-constructor-ratio"))));
            opt.constructor_ratio = opt.large_constructor_ratio;
        } else if (arg == "--large-patch-ratio") {
            opt.large_patch_ratio = min(50, max(0, stoi(next_arg(i, argc, argv, arg))));
        } else if (starts_with(arg, "--large-patch-ratio=")) {
            opt.large_patch_ratio = min(50, max(0, stoi(value_after_equals("--large-patch-ratio"))));
        } else if (arg == "--large-constructor-configs") {
            opt.large_constructor_configs = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.constructor_configs = opt.large_constructor_configs;
        } else if (starts_with(arg, "--large-constructor-configs=")) {
            opt.large_constructor_configs = max(0, stoi(value_after_equals("--large-constructor-configs")));
            opt.constructor_configs = opt.large_constructor_configs;
        } else if (arg == "--large-constructor-min-ms") {
            opt.large_constructor_min_ms = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--large-constructor-min-ms=")) {
            opt.large_constructor_min_ms = max(0, stoi(value_after_equals("--large-constructor-min-ms")));
        } else if (arg == "--large-config-time-ms") {
            opt.large_config_time_ms = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.constructor_config_time_ms = opt.large_config_time_ms;
        } else if (starts_with(arg, "--large-config-time-ms=")) {
            opt.large_config_time_ms = max(0, stoi(value_after_equals("--large-config-time-ms")));
            opt.constructor_config_time_ms = opt.large_config_time_ms;
        } else if (arg == "--large-repair-from-elites") {
            opt.large_repair_from_elites = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.constructor_repair_jobs = opt.large_repair_from_elites;
        } else if (starts_with(arg, "--large-repair-from-elites=")) {
            opt.large_repair_from_elites = max(0, stoi(value_after_equals("--large-repair-from-elites")));
            opt.constructor_repair_jobs = opt.large_repair_from_elites;
        } else if (arg == "--large-patch-cleanup-ms") {
            opt.large_patch_cleanup_ms = max(0, stoi(next_arg(i, argc, argv, arg)));
            opt.cleanup_ms = opt.large_patch_cleanup_ms;
        } else if (starts_with(arg, "--large-patch-cleanup-ms=")) {
            opt.large_patch_cleanup_ms = max(0, stoi(value_after_equals("--large-patch-cleanup-ms")));
            opt.cleanup_ms = opt.large_patch_cleanup_ms;
        } else if (arg == "--no-large-patch-cleanup") {
            opt.no_large_patch_cleanup = true;
            opt.no_cleanup = true;
        } else if (arg == "--large-seed") {
            opt.large_seed = static_cast<uint64_t>(stoull(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--large-seed=")) {
            opt.large_seed = static_cast<uint64_t>(stoull(value_after_equals("--large-seed")));
        } else if (arg == "--patch-max-commits") {
            opt.patch_max_commits = max(0, stoi(next_arg(i, argc, argv, arg)));
        } else if (starts_with(arg, "--patch-max-commits=")) {
            opt.patch_max_commits = max(0, stoi(value_after_equals("--patch-max-commits")));
        } else if (arg == "--constructive-mode") {
            opt.constructive_mode = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--constructive-mode=")) {
            opt.constructive_mode = value_after_equals("--constructive-mode");
        } else if (arg == "--out") {
            opt.output_path = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--out=")) {
            opt.output_path = value_after_equals("--out");
        } else if (arg == "--meta") {
            opt.meta_path = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--meta=")) {
            opt.meta_path = value_after_equals("--meta");
        } else if (arg == "--checkpoint") {
            opt.checkpoint_path = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--checkpoint=")) {
            opt.checkpoint_path = value_after_equals("--checkpoint");
        } else if (arg == "--resume") {
            opt.resume_path = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--resume=")) {
            opt.resume_path = value_after_equals("--resume");
        } else if (arg == "--validate-solution") {
            opt.validate_solution_path = next_arg(i, argc, argv, arg);
        } else if (starts_with(arg, "--validate-solution=")) {
            opt.validate_solution_path = value_after_equals("--validate-solution");
        } else if (!arg.empty() && arg[0] == '-') {
            throw runtime_error("unknown option: " + arg);
        } else if (opt.input_path.empty()) {
            opt.input_path = arg;
        } else {
            throw runtime_error("unexpected positional argument: " + arg);
        }
    }

    if (opt.improvement_mode == "patch") {
        opt.improvement_mode = "constructor-first";
    }
    if (opt.improvement_mode != "constructor-first" && opt.improvement_mode != "none") {
        throw runtime_error("--improvement must be constructor-first, patch, or none");
    }
    if (opt.constructive_mode != "greedy" && opt.constructive_mode != "paired" &&
        opt.constructive_mode != "macro" && opt.constructive_mode != "strip") {
        throw runtime_error("--constructive-mode must be greedy, paired, macro, or strip");
    }
    if (opt.patch_commit_policy != "best-batch" && opt.patch_commit_policy != "first" &&
        opt.patch_commit_policy != "deterministic") {
        throw runtime_error("--patch-commit-policy must be best-batch, first, or deterministic");
    }
    if (opt.macro_strategy != "auto" && opt.macro_strategy != "transport" &&
        opt.macro_strategy != "strip" && opt.macro_strategy != "sweep" &&
        opt.macro_strategy != "all") {
        throw runtime_error("--macro-strategy must be auto, transport, strip, sweep, or all");
    }
    if (opt.anytime_engine != "v1" && opt.anytime_engine != "monotone" &&
        opt.anytime_engine != "off") {
        throw runtime_error("--anytime-engine must be v1, monotone, or off");
    }
    if (opt.constructor_engine != "auto" && opt.constructor_engine != "v1" &&
        opt.constructor_engine != "monotone" && opt.constructor_engine != "off") {
        throw runtime_error("--constructor-engine must be auto, v1, monotone, or off");
    }
    if (opt.large_engine != "v1" && opt.large_engine != "band-v2" && opt.large_engine != "off") {
        throw runtime_error("--large-engine must be v1, band-v2, or off");
    }
    return opt;
}

struct Problem {
    int n = 0;
    vector<int> initial;
    vector<int> target_at;
    vector<int> inner_positions;
    int initial_blank = -1;
    Hash128 initial_hash;

    int index(int r, int c) const {
        return r * n + c;
    }

    pair<int, int> rc(int pos) const {
        return {pos / n, pos % n};
    }

    int manhattan(int a, int b) const {
        auto [ar, ac] = rc(a);
        auto [br, bc] = rc(b);
        return abs(ar - br) + abs(ac - bc);
    }

    bool in_bounds(int r, int c) const {
        return r >= 0 && r < n && c >= 0 && c < n;
    }

    bool is_inner(int pos) const {
        auto [r, c] = rc(pos);
        return r > 0 && r + 1 < n && c > 0 && c + 1 < n;
    }

    bool is_goal(const vector<int>& board) const {
        for (int pos : inner_positions) {
            if (board[pos] != target_at[pos]) {
                return false;
            }
        }
        return true;
    }

    Hash128 compute_hash(const vector<int>& board) const {
        Hash128 h;
        for (int i = 0; i < static_cast<int>(board.size()); ++i) {
            h = xor_hash(h, cell_hash(i, board[i]));
        }
        return h;
    }

    static Problem read(istream& in) {
        Problem p;
        if (!(in >> p.n)) {
            throw runtime_error("failed to read N");
        }
        if (p.n < 3) {
            throw runtime_error("N must be at least 3");
        }

        const int cells = p.n * p.n;
        p.initial.resize(cells);
        for (int i = 0; i < cells; ++i) {
            if (!(in >> p.initial[i])) {
                throw runtime_error("failed to read initial board");
            }
            if (p.initial[i] == -1) {
                p.initial_blank = i;
            }
        }
        if (p.initial_blank < 0) {
            throw runtime_error("initial board has no -1 blank");
        }

        p.target_at.assign(cells, kNoTarget);
        for (int r = 1; r + 1 < p.n; ++r) {
            for (int c = 1; c + 1 < p.n; ++c) {
                int value = 0;
                if (!(in >> value)) {
                    throw runtime_error("failed to read center target");
                }
                const int pos = p.index(r, c);
                p.target_at[pos] = value;
                p.inner_positions.push_back(pos);
            }
        }
        p.initial_hash = p.compute_hash(p.initial);
        return p;
    }
};

void apply_constructor_first_defaults(const Problem& p, Options& opt) {
    const bool constructor_board = p.n >= 6;
    if (opt.constructor_engine == "auto") {
        opt.constructor_engine = constructor_board && opt.anytime_constructive ? "monotone" : "off";
    }

    opt.anytime_engine = opt.constructor_engine;
    opt.anytime_constructive = opt.constructor_engine != "off";
    if (opt.constructor_threads > 0) {
        opt.anytime_threads = opt.constructor_threads;
    }
    if (opt.constructor_configs > 0) {
        opt.anytime_configs = opt.constructor_configs;
        if (p.n >= 30) {
            opt.large_constructor_configs = opt.constructor_configs;
        }
    }
    opt.anytime_seed = opt.constructor_seed;
    if (opt.large_seed == 0) {
        opt.large_seed = opt.constructor_seed;
    }

    const int default_constructor_ratio = p.n >= 30 ? 85 : 80;
    if (opt.constructor_ratio <= 0) {
        opt.constructor_ratio = default_constructor_ratio;
    }
    opt.anytime_ratio = min(95, max(1, opt.constructor_ratio));
    if (p.n >= 30) {
        opt.large_constructor_ratio = min(95, max(1, opt.constructor_ratio));
    }

    if (opt.constructor_config_time_ms >= 0) {
        opt.anytime_attempt_ms = opt.constructor_config_time_ms;
        if (p.n >= 30) {
            opt.large_config_time_ms = opt.constructor_config_time_ms;
        }
    }
    if (opt.constructor_repair_jobs > 0) {
        opt.repair_jobs = opt.constructor_repair_jobs;
        if (p.n >= 30) {
            opt.large_repair_from_elites = opt.constructor_repair_jobs;
        }
    } else if (constructor_board) {
        opt.constructor_repair_jobs = p.n >= 30 ? 128 : 64;
        opt.repair_jobs = opt.constructor_repair_jobs;
        if (p.n >= 30) {
            opt.large_repair_from_elites = opt.constructor_repair_jobs;
        }
    }

    if (opt.cleanup_ms < 0) {
        opt.cleanup_ms = constructor_board ? (p.n >= 30 ? 5'000 : 10'000) : -1;
    }
    if (opt.no_cleanup) {
        opt.no_large_patch_cleanup = true;
        opt.cleanup_ms = 0;
        opt.large_patch_cleanup_ms = 0;
    } else if (p.n >= 30 && opt.cleanup_ms >= 0) {
        opt.large_patch_cleanup_ms = opt.cleanup_ms;
    }

}

bool atomic_write(const string& path, const string& content) {
    if (path.empty()) {
        return true;
    }
    const string tmp = path + ".tmp";
    {
        ofstream out(tmp, ios::binary);
        if (!out) {
            return false;
        }
        out << content;
    }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool valid_command_for_blank(const Problem& p, int blank, char cmd, int& next_blank) {
    int r = blank / p.n;
    int c = blank % p.n;
    int nr = r;
    int nc = c;
    if (cmd == 'U') {
        nr = r + 1;
    } else if (cmd == 'D') {
        nr = r - 1;
    } else if (cmd == 'L') {
        nc = c + 1;
    } else if (cmd == 'R') {
        nc = c - 1;
    } else {
        return false;
    }
    if (!p.in_bounds(nr, nc)) {
        return false;
    }
    next_blank = p.index(nr, nc);
    return true;
}

Hash128 hash_after_swap(Hash128 hash, int blank, int tile_pos, int tile_value) {
    hash = xor_hash(hash, cell_hash(blank, -1));
    hash = xor_hash(hash, cell_hash(tile_pos, tile_value));
    hash = xor_hash(hash, cell_hash(blank, tile_value));
    hash = xor_hash(hash, cell_hash(tile_pos, -1));
    return hash;
}

bool apply_move(const Problem& p, vector<int>& board, int& blank, char cmd, Hash128* hash = nullptr) {
    int next_blank = -1;
    if (!valid_command_for_blank(p, blank, cmd, next_blank)) {
        return false;
    }
    const int tile_value = board[next_blank];
    if (tile_value == -1) {
        return false;
    }
    if (hash != nullptr) {
        *hash = hash_after_swap(*hash, blank, next_blank, tile_value);
    }
    swap(board[blank], board[next_blank]);
    blank = next_blank;
    return true;
}

bool apply_move(const Problem& p, PackedBoard& board, int& blank, char cmd, Hash128* hash = nullptr) {
    int next_blank = -1;
    if (!valid_command_for_blank(p, blank, cmd, next_blank)) {
        return false;
    }
    const int tile_value = unpack_cell(board[next_blank]);
    if (tile_value == -1) {
        return false;
    }
    if (hash != nullptr) {
        *hash = hash_after_swap(*hash, blank, next_blank, tile_value);
    }
    swap(board[blank], board[next_blank]);
    blank = next_blank;
    return true;
}

char command_for_blank_step(const Problem& p, int from, int to) {
    const int dr = to / p.n - from / p.n;
    const int dc = to % p.n - from % p.n;
    if (dr == 1 && dc == 0) return 'U';
    if (dr == -1 && dc == 0) return 'D';
    if (dr == 0 && dc == 1) return 'L';
    if (dr == 0 && dc == -1) return 'R';
    throw runtime_error("non-adjacent blank step");
}

char command_for_tile_step(const Problem& p, int from, int to) {
    const int dr = to / p.n - from / p.n;
    const int dc = to % p.n - from % p.n;
    if (dr == -1 && dc == 0) return 'U';
    if (dr == 1 && dc == 0) return 'D';
    if (dr == 0 && dc == -1) return 'L';
    if (dr == 0 && dc == 1) return 'R';
    throw runtime_error("non-adjacent tile step");
}

char inverse_command(char cmd) {
    if (cmd == 'U') return 'D';
    if (cmd == 'D') return 'U';
    if (cmd == 'L') return 'R';
    if (cmd == 'R') return 'L';
    return 0;
}

bool validate_solution(const Problem& p, const string& path, vector<int>* final_board = nullptr) {
    vector<int> board = p.initial;
    int blank = p.initial_blank;
    for (char cmd : path) {
        if (cmd == 'S') {
            break;
        }
        if (!apply_move(p, board, blank, cmd)) {
            return false;
        }
    }
    if (final_board != nullptr) {
        *final_board = board;
    }
    return p.is_goal(board);
}

class ConstructiveSolver {
public:
    ConstructiveSolver(const Problem& p, const string& mode, const Timer* timer = nullptr, int deadline_ms = -1)
        : p_(p),
          board_(pack_board(p.initial)),
          blank_(p.initial_blank),
          paired_mode_(mode == "paired" || mode == "macro"),
          macro_mode_(mode == "macro" || starts_with(mode, "strip") || starts_with(mode, "order")),
          strip_mode_(starts_with(mode, "strip")),
          order_mode_(starts_with(mode, "order")),
          strip_variant_(mode),
          timer_(timer),
          deadline_ms_(deadline_ms),
          locked_(p.n * p.n, 0),
          parent_(p.n * p.n, -1),
          seen_(p.n * p.n, 0) {}

    bool solve(string& path) {
        if (p_.is_goal(unpack_board(board_))) {
            path.clear();
            return true;
        }
        if (strip_mode_) {
            return solve_strip(path);
        }
        if (order_mode_) {
            return solve_dynamic_order(path);
        }

        for (int r = 1; r + 1 < p_.n; ++r) {
            if (time_exhausted()) {
                return false;
            }
            int row_end_limit = paired_mode_ && r + 2 < p_.n ? p_.n - 3 : p_.n - 1;
            for (int c = 1; c < row_end_limit; ++c) {
                if (time_exhausted()) {
                    return false;
                }
                const int target = p_.index(r, c);
                const int want = p_.target_at[target];

                if (want == -1) {
                    if (!move_blank_to(target, -1)) {
                        return false;
                    }
                    locked_[target] = 1;
                    continue;
                }

                if (unpack_cell(board_[target]) == want) {
                    locked_[target] = 1;
                    continue;
                }

                int source = find_source(want, target);
                if (source < 0) {
                    return false;
                }
                if (!transport_tile(source, target)) {
                    return false;
                }
                if (unpack_cell(board_[target]) != want) {
                    return false;
                }
                locked_[target] = 1;
            }
            if (paired_mode_ && r + 2 < p_.n && !solve_row_suffix(r)) {
                return false;
            }
        }

        if (paired_mode_ && !finish_remaining_center()) {
            return false;
        }

        if (!p_.is_goal(unpack_board(board_))) {
            return false;
        }
        path = moves_;
        return true;
    }

private:
    bool time_exhausted() const {
        return timer_ != nullptr && deadline_ms_ >= 0 && timer_->elapsed_ms() >= deadline_ms_;
    }

    static PackedBoard pack_board(const vector<int>& board) {
        PackedBoard packed;
        packed.reserve(board.size());
        for (int value : board) {
            packed.push_back(pack_cell(value));
        }
        return packed;
    }

    vector<int> bfs_path(int start, int goal, int extra_block) {
        if (start == goal) {
            return {start};
        }
        ++stamp_;
        if (stamp_ == numeric_limits<int>::max()) {
            fill(seen_.begin(), seen_.end(), 0);
            stamp_ = 1;
        }

        deque<int> q;
        q.push_back(start);
        seen_[start] = stamp_;
        parent_[start] = -1;

        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        int checks = 0;
        while (!q.empty()) {
            if ((++checks & 255) == 0 && time_exhausted()) {
                return {};
            }
            int cur = q.front();
            q.pop_front();
            auto [r, c] = p_.rc(cur);
            for (int k = 0; k < 4; ++k) {
                int nr = r + dr[k];
                int nc = c + dc[k];
                if (!p_.in_bounds(nr, nc)) {
                    continue;
                }
                int nxt = p_.index(nr, nc);
                if (seen_[nxt] == stamp_) {
                    continue;
                }
                if (nxt != goal && (locked_[nxt] || nxt == extra_block)) {
                    continue;
                }
                seen_[nxt] = stamp_;
                parent_[nxt] = cur;
                if (nxt == goal) {
                    vector<int> path;
                    for (int x = goal; x != -1; x = parent_[x]) {
                        path.push_back(x);
                    }
                    reverse(path.begin(), path.end());
                    return path;
                }
                q.push_back(nxt);
            }
        }
        return {};
    }

    bool move_blank_to(int dest, int controlled_tile) {
        if (blank_ == dest) {
            return true;
        }
        vector<int> path = bfs_path(blank_, dest, controlled_tile);
        if (path.empty()) {
            return false;
        }
        for (size_t i = 1; i < path.size(); ++i) {
            char cmd = command_for_blank_step(p_, blank_, path[i]);
            bool ok = apply_move(p_, board_, blank_, cmd);
            if (!ok) {
                return false;
            }
            moves_.push_back(cmd);
        }
        return true;
    }

    int find_source(int value, int target) const {
        int best = -1;
        int best_dist = kInf;
        for (int i = 0; i < static_cast<int>(board_.size()); ++i) {
            if (unpack_cell(board_[i]) != value || locked_[i]) {
                continue;
            }
            int d = p_.manhattan(i, target);
            if (d < best_dist) {
                best_dist = d;
                best = i;
            }
        }
        return best;
    }

    bool transport_tile(int source, int target) {
        if (macro_mode_) {
            string saved_moves = moves_;
            PackedBoard saved_board = board_;
            int saved_blank = blank_;
            if (transport_tile_macro(source, target)) {
                return true;
            }
            moves_ = std::move(saved_moves);
            board_ = std::move(saved_board);
            blank_ = saved_blank;
        }
        int tile = source;
        vector<int> tile_path = bfs_path(tile, target, -1);
        if (tile_path.empty()) {
            return false;
        }

        for (size_t i = 1; i < tile_path.size(); ++i) {
            int next_tile_pos = tile_path[i];
            if (!move_blank_to(next_tile_pos, tile)) {
                return false;
            }
            char cmd = command_for_tile_step(p_, tile, next_tile_pos);
            bool ok = apply_move(p_, board_, blank_, cmd);
            if (!ok) {
                return false;
            }
            moves_.push_back(cmd);
            tile = next_tile_pos;
        }
        return true;
    }

    bool transport_tile_macro(int source, int target) {
        int tile = source;
        unordered_set<uint64_t> seen_states;
        seen_states.reserve(256);
        const int max_steps = max(16, p_.n * p_.n * 2);
        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        for (int guard = 0; guard < max_steps && tile != target; ++guard) {
            if (time_exhausted()) {
                return false;
            }
            uint64_t state_key = (static_cast<uint64_t>(tile) << 32) ^ static_cast<uint64_t>(blank_);
            seen_states.insert(state_key);
            auto [tr, tc] = p_.rc(tile);
            struct Choice {
                int score = kInf;
                int next_tile = -1;
                vector<int> blank_path;
            };
            Choice best;
            for (int k = 0; k < 4; ++k) {
                int nr = tr + dr[k];
                int nc = tc + dc[k];
                if (!p_.in_bounds(nr, nc)) {
                    continue;
                }
                int next_tile = p_.index(nr, nc);
                if (next_tile != target && locked_[next_tile]) {
                    continue;
                }
                vector<int> blank_path = bfs_path(blank_, next_tile, tile);
                if (blank_path.empty()) {
                    continue;
                }
                int next_dist = p_.manhattan(next_tile, target);
                int cur_dist = p_.manhattan(tile, target);
                uint64_t next_state_key = (static_cast<uint64_t>(next_tile) << 32) ^ static_cast<uint64_t>(tile);
                int score = static_cast<int>(blank_path.size()) - 1 + next_dist * 4;
                if (next_dist > cur_dist) {
                    score += 12;
                }
                if (seen_states.find(next_state_key) != seen_states.end()) {
                    score += 50;
                }
                if (score < best.score) {
                    best = {score, next_tile, std::move(blank_path)};
                }
            }
            if (best.next_tile < 0) {
                return false;
            }
            for (size_t i = 1; i < best.blank_path.size(); ++i) {
                char cmd = command_for_blank_step(p_, blank_, best.blank_path[i]);
                if (!apply_move(p_, board_, blank_, cmd)) {
                    return false;
                }
                moves_.push_back(cmd);
            }
            char cmd = command_for_tile_step(p_, tile, best.next_tile);
            if (!apply_move(p_, board_, blank_, cmd)) {
                return false;
            }
            moves_.push_back(cmd);
            tile = best.next_tile;
        }
        return tile == target;
    }

    bool solve_row_suffix(int r) {
        vector<int> pending;
        for (int c = p_.n - 3; c < p_.n - 1; ++c) {
            pending.push_back(p_.index(r, c));
        }
        return solve_pending_targets(pending);
    }

    bool finish_remaining_center() {
        vector<int> pending;
        for (int pos : p_.inner_positions) {
            if (!locked_[pos]) {
                pending.push_back(pos);
            }
        }
        if (pending.empty()) {
            return true;
        }
        return solve_pending_targets(pending);
    }

    bool solve_pending_targets(const vector<int>& pending) {
        int guard = 0;
        while (guard++ < static_cast<int>(pending.size()) * 4) {
            if (time_exhausted()) {
                return false;
            }
            bool progress = false;
            for (int target : pending) {
                if (locked_[target]) {
                    continue;
                }
                int want = p_.target_at[target];
                if (want == -1) {
                    if (move_blank_to(target, -1)) {
                        locked_[target] = 1;
                        progress = true;
                    }
                    continue;
                }
                if (unpack_cell(board_[target]) == want) {
                    locked_[target] = 1;
                    progress = true;
                    continue;
                }
                int source = find_source(want, target);
                if (source >= 0 && transport_tile(source, target) && unpack_cell(board_[target]) == want) {
                    locked_[target] = 1;
                    progress = true;
                }
            }
            if (progress) {
                bool done = true;
                for (int target : pending) {
                    if (!locked_[target]) {
                        done = false;
                        break;
                    }
                }
                if (done) {
                    return true;
                }
                continue;
            }
            break;
        }
        return try_local_finish(pending);
    }

    vector<vector<int>> make_strip_bands() const {
        const int inner = max(0, p_.n - 2);
        int band_size = 3;
        if (strip_variant_.find('2') != string::npos) {
            band_size = 2;
        } else if (strip_variant_.find('4') != string::npos) {
            band_size = 4;
        }
        vector<vector<int>> bands;
        auto add_row_band = [&](int r0, int r1) {
            vector<int> band;
            for (int r = r0; r <= r1; ++r) {
                for (int c = 1; c + 1 < p_.n; ++c) {
                    band.push_back(p_.index(r, c));
                }
            }
            if (!band.empty()) {
                bands.push_back(std::move(band));
            }
        };
        auto add_col_band = [&](int c0, int c1) {
            vector<int> band;
            for (int c = c0; c <= c1; ++c) {
                for (int r = 1; r + 1 < p_.n; ++r) {
                    band.push_back(p_.index(r, c));
                }
            }
            if (!band.empty()) {
                bands.push_back(std::move(band));
            }
        };

        const bool col_mode = strip_variant_.find("col") != string::npos;
        const bool reverse_mode = strip_variant_.find("rev") != string::npos;
        const bool center_mode = strip_variant_.find("center") != string::npos;
        if (center_mode) {
            vector<pair<int, int>> blocks;
            for (int off = 0; off < inner; off += band_size) {
                int a = 1 + off;
                int b = min(p_.n - 2, a + band_size - 1);
                int mid = (a + b) / 2;
                blocks.push_back({abs(mid - p_.n / 2), off});
            }
            sort(blocks.begin(), blocks.end());
            for (auto [dist, off] : blocks) {
                (void)dist;
                int a = 1 + off;
                int b = min(p_.n - 2, a + band_size - 1);
                if (col_mode) {
                    add_col_band(a, b);
                } else {
                    add_row_band(a, b);
                }
            }
            return bands;
        }

        if (!reverse_mode) {
            for (int a = 1; a + 1 < p_.n; a += band_size) {
                int b = min(p_.n - 2, a + band_size - 1);
                if (col_mode) {
                    add_col_band(a, b);
                } else {
                    add_row_band(a, b);
                }
            }
        } else {
            for (int b = p_.n - 2; b >= 1; b -= band_size) {
                int a = max(1, b - band_size + 1);
                if (col_mode) {
                    add_col_band(a, b);
                } else {
                    add_row_band(a, b);
                }
            }
        }
        return bands;
    }

    int find_best_source_for_band(int value, int target) const {
        int best = -1;
        int best_cost = kInf;
        for (int i = 0; i < static_cast<int>(board_.size()); ++i) {
            if (unpack_cell(board_[i]) != value || locked_[i]) {
                continue;
            }
            int cost = p_.manhattan(i, target) * 5 + p_.manhattan(blank_, i);
            auto [r, c] = p_.rc(i);
            if (r == 0 || c == 0 || r + 1 == p_.n || c + 1 == p_.n) {
                cost -= 2;
            }
            if (i == target) {
                cost -= 50;
            }
            if (cost < best_cost) {
                best_cost = cost;
                best = i;
            }
        }
        return best;
    }

    bool solve_band_targets(const vector<int>& band) {
        vector<int> normal;
        normal.reserve(band.size());
        for (int target : band) {
            if (p_.target_at[target] != -1) {
                normal.push_back(target);
            }
        }
        const int max_rounds = max(32, static_cast<int>(normal.size()) * 10 + p_.n * 4);
        int last_wrong = kInf;
        int stagnant_rounds = 0;
        for (int round = 0; round < max_rounds; ++round) {
            if (time_exhausted()) {
                return false;
            }
            vector<int> wrong;
            wrong.reserve(normal.size());
            for (int target : normal) {
                if (unpack_cell(board_[target]) != p_.target_at[target]) {
                    wrong.push_back(target);
                }
            }
            if (wrong.empty()) {
                for (int target : normal) {
                    locked_[target] = 1;
                }
                return true;
            }
            if (static_cast<int>(wrong.size()) >= last_wrong) {
                ++stagnant_rounds;
            } else {
                stagnant_rounds = 0;
            }
            last_wrong = static_cast<int>(wrong.size());
            if (stagnant_rounds > max(8, static_cast<int>(normal.size()) / 2)) {
                return false;
            }

            int best_target = -1;
            int best_source = -1;
            int best_cost = kInf;
            for (int target : wrong) {
                int source = find_best_source_for_band(p_.target_at[target], target);
                if (source < 0) {
                    continue;
                }
                int cost = p_.manhattan(source, target) * 5 + p_.manhattan(blank_, source);
                int border_bonus = p_.is_inner(source) ? 0 : -4;
                cost += border_bonus;
                if (cost < best_cost) {
                    best_cost = cost;
                    best_target = target;
                    best_source = source;
                }
            }
            if (best_target < 0 || best_source < 0) {
                return false;
            }
            if (!transport_tile(best_source, best_target) ||
                unpack_cell(board_[best_target]) != p_.target_at[best_target]) {
                return false;
            }
        }
        return false;
    }

    bool solve_strip(string& path) {
        vector<vector<int>> bands = make_strip_bands();
        for (const vector<int>& band : bands) {
            if (!solve_band_targets(band)) {
                return false;
            }
        }

        vector<int> pending;
        int blank_target = -1;
        for (int pos : p_.inner_positions) {
            if (p_.target_at[pos] == -1) {
                blank_target = pos;
                continue;
            }
            if (!locked_[pos]) {
                pending.push_back(pos);
            }
        }
        if (!pending.empty() && !solve_pending_targets(pending)) {
            return false;
        }
        if (blank_target >= 0) {
            if (!move_blank_to(blank_target, -1)) {
                return false;
            }
            locked_[blank_target] = 1;
        }
        if (!p_.is_goal(unpack_board(board_))) {
            return false;
        }
        path = moves_;
        return true;
    }

    bool solve_dynamic_order(string& path) {
        vector<int> pending;
        int blank_target = -1;
        pending.reserve(p_.inner_positions.size());
        for (int pos : p_.inner_positions) {
            if (p_.target_at[pos] == -1) {
                blank_target = pos;
            } else {
                pending.push_back(pos);
            }
        }

        int guard = 0;
        const int max_rounds = max(64, static_cast<int>(pending.size()) * 3);
        while (guard++ < max_rounds) {
            if (time_exhausted()) {
                return false;
            }
            for (int target : pending) {
                if (!locked_[target] && unpack_cell(board_[target]) == p_.target_at[target]) {
                    locked_[target] = 1;
                }
            }
            int best_target = -1;
            int best_source = -1;
            int best_cost = kInf;
            int remaining = 0;
            for (int target : pending) {
                if (locked_[target]) {
                    continue;
                }
                ++remaining;
                int source = find_best_source_for_band(p_.target_at[target], target);
                if (source < 0) {
                    continue;
                }
                int cost = p_.manhattan(source, target) * 6 + p_.manhattan(blank_, source);
                auto [tr, tc] = p_.rc(target);
                int edge_dist = min({tr - 1, tc - 1, p_.n - 2 - tr, p_.n - 2 - tc});
                if (strip_variant_.find("edge") != string::npos) {
                    cost += edge_dist * 3;
                } else if (strip_variant_.find("center") != string::npos) {
                    cost -= edge_dist * 2;
                }
                if (cost < best_cost) {
                    best_cost = cost;
                    best_target = target;
                    best_source = source;
                }
            }
            if (remaining == 0) {
                break;
            }
            if (best_target < 0 || best_source < 0) {
                return false;
            }
            if (!transport_tile(best_source, best_target) ||
                unpack_cell(board_[best_target]) != p_.target_at[best_target]) {
                return false;
            }
            locked_[best_target] = 1;
        }

        for (int target : pending) {
            if (unpack_cell(board_[target]) != p_.target_at[target]) {
                return false;
            }
            locked_[target] = 1;
        }
        if (blank_target >= 0) {
            if (!move_blank_to(blank_target, -1)) {
                return false;
            }
            locked_[blank_target] = 1;
        }
        if (!p_.is_goal(unpack_board(board_))) {
            return false;
        }
        path = moves_;
        return true;
    }

    bool try_local_finish(const vector<int>& pending) {
        struct State {
            PackedBoard board;
            int blank = -1;
            string path;
        };
        const int max_depth = min(20, static_cast<int>(pending.size()) * 3 + 6);
        deque<State> q;
        unordered_set<Hash128, Hash128Hasher> seen;
        Hash128 start_hash = p_.compute_hash(unpack_board(board_));
        q.push_back({board_, blank_, ""});
        seen.insert(start_hash);
        int checks = 0;
        while (!q.empty()) {
            if ((++checks & 255) == 0 && time_exhausted()) {
                return false;
            }
            State cur = std::move(q.front());
            q.pop_front();
            bool ok = true;
            for (int target : pending) {
                int want = p_.target_at[target];
                if (want == -1) {
                    if (cur.blank != target) {
                        ok = false;
                        break;
                    }
                } else if (unpack_cell(cur.board[target]) != want) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                for (char cmd : cur.path) {
                    bool moved = apply_move(p_, board_, blank_, cmd);
                    if (!moved) {
                        return false;
                    }
                    moves_.push_back(cmd);
                }
                for (int target : pending) {
                    locked_[target] = 1;
                }
                return true;
            }
            if (static_cast<int>(cur.path.size()) >= max_depth) {
                continue;
            }
            for (char cmd : {'U', 'D', 'L', 'R'}) {
                State next = cur;
                if (!apply_move(p_, next.board, next.blank, cmd)) {
                    continue;
                }
                Hash128 hash = p_.compute_hash(unpack_board(next.board));
                if (!seen.insert(hash).second) {
                    continue;
                }
                next.path.push_back(cmd);
                q.push_back(std::move(next));
            }
        }
        return false;
    }

    vector<int> unpack_board(const PackedBoard& board) const {
        vector<int> out;
        out.reserve(board.size());
        for (uint16_t cell : board) {
            out.push_back(unpack_cell(cell));
        }
        return out;
    }

    const Problem& p_;
    PackedBoard board_;
    int blank_;
    bool paired_mode_ = false;
    bool macro_mode_ = false;
    bool strip_mode_ = false;
    bool order_mode_ = false;
    string strip_variant_;
    const Timer* timer_ = nullptr;
    int deadline_ms_ = -1;
    vector<unsigned char> locked_;
    vector<int> parent_;
    vector<int> seen_;
    int stamp_ = 0;
    string moves_;
};

class AnytimeConstructiveSolver {
public:
    struct Stats {
        uint64_t configs_attempted = 0;
        uint64_t valid_solutions = 0;
        uint64_t elite_updates = 0;
        uint64_t repair_jobs_attempted = 0;
        uint64_t repair_commits = 0;
        uint64_t monotone_attempts = 0;
        uint64_t monotone_valid = 0;
        uint64_t repair_valid = 0;
        uint64_t checkpoint_jobs = 0;
        uint64_t cancelled_opposites = 0;
        string best_config_id = "none";
    };

    struct Result {
        string best_path;
        Stats stats;
    };

    struct PartialCheckpoint {
        PackedBoard board;
        int blank = -1;
        vector<unsigned char> locked;
        string moves;
        int fixed_count = 0;
    };

    struct ExternalCheckpoint {
        PackedBoard board;
        int blank = -1;
        vector<unsigned char> locked;
        string moves;
        int fixed_count = 0;
        string label;
    };

    AnytimeConstructiveSolver(const Problem& p,
                              const Options& opt,
                              const Timer& timer,
                              int deadline_ms,
                              size_t incumbent_length)
        : p_(p),
          opt_(opt),
          timer_(timer),
          deadline_ms_(deadline_ms),
          attempt_deadline_ms_(deadline_ms),
          incumbent_length_(incumbent_length),
          parent_(p.n * p.n, -1),
          seen_(p.n * p.n, 0),
          dist_to_target_(p.n * p.n, -1),
          blank_dist_(p.n * p.n, -1),
          blank_parent_(p.n * p.n, -1) {}

    Result run() {
        Result result;
        if (!opt_.anytime_constructive || opt_.anytime_engine == "off") {
            return result;
        }
        vector<Config> configs = make_configs();
        int per_config_ms = opt_.anytime_attempt_ms > 0
                                ? opt_.anytime_attempt_ms
                                : (p_.n >= 30
                                       ? (opt_.large_config_time_ms > 0 ? opt_.large_config_time_ms : max(1, deadline_ms_))
                                       : (p_.n >= 16 ? 600 : 1400));
        unsigned hw = thread::hardware_concurrency();
        int default_threads = max(1, static_cast<int>(hw == 0 ? 2 : hw) - 1);
        int thread_count = opt_.patch_commit_policy == "deterministic"
                               ? 1
                               : max(1, opt_.anytime_threads > 0 ? opt_.anytime_threads : default_threads);
        thread_count = min<int>(thread_count, max<size_t>(1, configs.size()));

        vector<InitialRecord> records;
        mutex records_mu;
        atomic<size_t> next_config{0};
        auto worker_fn = [&]() {
            AnytimeConstructiveSolver worker(p_, opt_, timer_, deadline_ms_, incumbent_length_);
            while (timer_.elapsed_ms() < deadline_ms_) {
                size_t idx = next_config.fetch_add(1);
                if (idx >= configs.size()) {
                    break;
                }
                const Config& cfg = configs[idx];
                worker.attempt_deadline_ms_ =
                    per_config_ms >= deadline_ms_
                        ? deadline_ms_
                        : min(deadline_ms_, static_cast<int>(timer_.elapsed_ms()) + per_config_ms);
                vector<PartialCheckpoint> checkpoints;
                uint64_t cancelled_before = worker.cancelled_opposites_;
                optional<string> path = worker.solve_config(cfg, nullptr, &checkpoints);
                if (!path || !validate_solution(p_, *path + "S")) {
                    continue;
                }
                InitialRecord rec{*path, cfg, std::move(checkpoints),
                                  worker.cancelled_opposites_ - cancelled_before};
                {
                    lock_guard<mutex> lock(records_mu);
                    records.push_back(std::move(rec));
                }
            }
        };
        if (thread_count <= 1) {
            worker_fn();
        } else {
            vector<thread> workers;
            workers.reserve(thread_count);
            for (int i = 0; i < thread_count; ++i) {
                workers.emplace_back(worker_fn);
            }
            for (thread& worker : workers) {
                worker.join();
            }
        }

        result.stats.configs_attempted = min<uint64_t>(configs.size(), next_config.load());
        if (opt_.anytime_engine == "monotone") {
            result.stats.monotone_attempts = result.stats.configs_attempted;
        }
        sort(records.begin(), records.end(), [](const InitialRecord& a, const InitialRecord& b) {
            if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
            if (a.config.id != b.config.id) return a.config.id < b.config.id;
            return a.config.seed < b.config.seed;
        });
        for (InitialRecord& rec : records) {
            ++result.stats.valid_solutions;
            if (opt_.anytime_engine == "monotone") {
                ++result.stats.monotone_valid;
            }
            result.stats.cancelled_opposites += rec.cancelled_opposites;
            result.stats.checkpoint_jobs += rec.checkpoints.size();
            bool elite_added = add_elite(rec.path, rec.config, std::move(rec.checkpoints));
            if (elite_added) {
                ++result.stats.elite_updates;
            }
            if (rec.path.size() < incumbent_length_) {
                incumbent_length_ = rec.path.size();
                result.best_path = rec.path;
                result.stats.best_config_id = rec.config.id;
            }
        }

        int repair_limit = p_.n >= 30 ? opt_.large_repair_from_elites : opt_.repair_jobs;
        for (const EliteSolution& elite : elites_) {
            for (const PartialCheckpoint& cp : elite.checkpoints) {
                if (repair_limit-- <= 0 || timer_.elapsed_ms() >= deadline_ms_) {
                    result.stats = merge_stats(result.stats);
                    return result;
                }
                Config repair_cfg = elite.config;
                repair_cfg.seed = splitmix64(repair_cfg.seed + static_cast<uint64_t>(cp.moves.size()) + 17);
                repair_cfg.order_type = (repair_cfg.order_type + 1 + static_cast<int>(cp.moves.size() % 7)) % 8;
                repair_cfg.source_pref = (repair_cfg.source_pref + 1) % 5;
                repair_cfg.cand_limit = max(repair_cfg.cand_limit, 48);
                repair_cfg.candidate_slack += 2;
                repair_cfg.id = elite.config.id + "/repair";
                ++result.stats.repair_jobs_attempted;
                attempt_deadline_ms_ = min(deadline_ms_, static_cast<int>(timer_.elapsed_ms()) + per_config_ms);
                optional<string> repaired = solve_config(repair_cfg, &cp, nullptr);
                if (!repaired || repaired->size() >= incumbent_length_ ||
                    !validate_solution(p_, *repaired + "S")) {
                    continue;
                }
                ++result.stats.valid_solutions;
                ++result.stats.repair_valid;
                ++result.stats.repair_commits;
                incumbent_length_ = repaired->size();
                result.best_path = *repaired;
                result.stats.best_config_id = repair_cfg.id;
                add_elite(*repaired, repair_cfg, {});
            }
        }
        result.stats = merge_stats(result.stats);
        return result;
    }

    Result run_from_external_checkpoints(const vector<ExternalCheckpoint>& checkpoints, int job_limit) {
        Result result;
        if (checkpoints.empty() || job_limit <= 0 || opt_.anytime_engine == "off") {
            return result;
        }
        vector<Config> configs = make_configs();
        if (configs.empty()) {
            return result;
        }
        struct Job {
            int checkpoint_index = 0;
            int config_index = 0;
        };
        vector<Job> jobs;
        jobs.reserve(static_cast<size_t>(job_limit));
        for (int ki = 0; ki < static_cast<int>(configs.size()) && static_cast<int>(jobs.size()) < job_limit; ++ki) {
            for (int ci = static_cast<int>(checkpoints.size()) - 1;
                 ci >= 0 && static_cast<int>(jobs.size()) < job_limit;
                 --ci) {
                jobs.push_back({ci, ki});
            }
        }
        int per_job_ms = opt_.anytime_attempt_ms > 0
                             ? opt_.anytime_attempt_ms
                             : (p_.n >= 30
                                    ? (opt_.large_config_time_ms > 0 ? opt_.large_config_time_ms : max(1, deadline_ms_))
                                    : 700);
        unsigned hw = thread::hardware_concurrency();
        int default_threads = max(1, static_cast<int>(hw == 0 ? 2 : hw) - 1);
        int thread_count = opt_.patch_commit_policy == "deterministic"
                               ? 1
                               : max(1, opt_.anytime_threads > 0 ? opt_.anytime_threads : default_threads);
        thread_count = min<int>(thread_count, max<size_t>(1, jobs.size()));

        struct Record {
            string path;
            Config config;
            string label;
            uint64_t cancelled = 0;
        };
        vector<Record> records;
        mutex records_mu;
        atomic<size_t> next_job{0};
        auto worker_fn = [&]() {
            AnytimeConstructiveSolver worker(p_, opt_, timer_, deadline_ms_, incumbent_length_);
            while (timer_.elapsed_ms() < deadline_ms_) {
                size_t job_pos = next_job.fetch_add(1);
                if (job_pos >= jobs.size()) {
                    break;
                }
                const Job& job = jobs[job_pos];
                const ExternalCheckpoint& ext = checkpoints[static_cast<size_t>(job.checkpoint_index)];
                Config cfg = configs[static_cast<size_t>(job.config_index)];
                cfg.seed = splitmix64(cfg.seed + static_cast<uint64_t>(ext.moves.size()) + job_pos * 131);
                cfg.order_type = (cfg.order_type + job.checkpoint_index + 1) % 8;
                cfg.source_pref = (cfg.source_pref + job.checkpoint_index + 1) % 5;
                cfg.cand_limit = max(cfg.cand_limit, 48);
                cfg.candidate_slack += 2;
                cfg.id += "/large-suffix/" + ext.label;
                PartialCheckpoint cp{ext.board, ext.blank, ext.locked, ext.moves, ext.fixed_count};
                worker.attempt_deadline_ms_ =
                    per_job_ms >= deadline_ms_
                        ? deadline_ms_
                        : min(deadline_ms_, static_cast<int>(timer_.elapsed_ms()) + per_job_ms);
                uint64_t before = worker.cancelled_opposites_;
                optional<string> path = worker.solve_config(cfg, &cp, nullptr);
                if (!path || path->size() >= incumbent_length_ || !validate_solution(p_, *path + "S")) {
                    continue;
                }
                Record rec{*path, cfg, ext.label, worker.cancelled_opposites_ - before};
                lock_guard<mutex> lock(records_mu);
                records.push_back(std::move(rec));
            }
        };
        if (thread_count <= 1) {
            worker_fn();
        } else {
            vector<thread> workers;
            workers.reserve(thread_count);
            for (int i = 0; i < thread_count; ++i) {
                workers.emplace_back(worker_fn);
            }
            for (thread& worker : workers) {
                worker.join();
            }
        }

        result.stats.repair_jobs_attempted = min<uint64_t>(jobs.size(), next_job.load());
        result.stats.checkpoint_jobs = checkpoints.size();
        sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
            if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
            if (a.config.id != b.config.id) return a.config.id < b.config.id;
            return a.label < b.label;
        });
        for (const Record& rec : records) {
            ++result.stats.valid_solutions;
            ++result.stats.repair_valid;
            result.stats.cancelled_opposites += rec.cancelled;
            if (rec.path.size() < incumbent_length_) {
                incumbent_length_ = rec.path.size();
                result.best_path = rec.path;
                result.stats.best_config_id = rec.config.id;
            }
        }
        result.stats = merge_stats(result.stats);
        return result;
    }

private:
    struct Config {
        int order_type = 0;
        int source_pref = 0;
        int step_policy = 0;
        int lock_style = 0;
        int cand_limit = 24;
        int cand_a = 18;
        int cand_b = 4;
        int cand_c = 3;
        int step_q = 4;
        int step_t = 2;
        int candidate_slack = 3;
        int max_passes = 4;
        uint64_t seed = 1;
        string id;
    };

    struct RunState {
        PackedBoard board;
        int blank = -1;
        vector<unsigned char> locked;
        string moves;
        int fixed_count = 0;
    };

    struct SourceCandidate {
        int pos = -1;
        double score = 0;
    };

    struct EliteSolution {
        string path;
        Config config;
        vector<PartialCheckpoint> checkpoints;
    };

    struct InitialRecord {
        string path;
        Config config;
        vector<PartialCheckpoint> checkpoints;
        uint64_t cancelled_opposites = 0;
    };

    bool time_exhausted() const {
        int active_deadline = min(deadline_ms_, attempt_deadline_ms_);
        return opt_.time_limit_ms >= 0 && timer_.elapsed_ms() >= active_deadline;
    }

    static PackedBoard pack_board(const vector<int>& board) {
        PackedBoard packed;
        packed.reserve(board.size());
        for (int value : board) {
            packed.push_back(pack_cell(value));
        }
        return packed;
    }

    static vector<int> unpack_board(const PackedBoard& board) {
        vector<int> out;
        out.reserve(board.size());
        for (uint16_t cell : board) {
            out.push_back(unpack_cell(cell));
        }
        return out;
    }

    Stats merge_stats(Stats stats) const {
        stats.elite_updates = max<uint64_t>(stats.elite_updates, elite_updates_);
        stats.cancelled_opposites += cancelled_opposites_;
        return stats;
    }

    vector<Config> make_configs() const {
        int auto_count = p_.n >= 30 ? 2048 : (p_.n >= 16 ? 768 : 512);
        int count = opt_.anytime_configs > 0 ? opt_.anytime_configs : auto_count;
        if (p_.n >= 30 && opt_.large_constructor_configs > 0) {
            count = opt_.large_constructor_configs;
        }
        vector<Config> configs;
        configs.reserve(count);
        struct Preset {
            int cand_limit;
            int cand_a;
            int cand_b;
            int cand_c;
            int step_q;
            int step_t;
            int slack;
            int max_passes;
        };
        const vector<Preset> presets = {
            {16, 24, 2, 1, 3, 1, 2, 4},
            {24, 20, 3, 2, 4, 2, 2, 4},
            {32, 16, 5, 2, 5, 2, 3, 6},
            {32, 18, 4, 5, 4, 3, 2, 6},
            {48, 14, 6, 2, 6, 3, 4, 8},
            {48, 12, 8, 2, 7, 4, 4, 8},
            {64, 16, 7, 4, 7, 4, 3, 10},
            {64, 12, 9, 3, 8, 5, 4, 10},
        };
        auto make_cfg = [&](int idx, int order_type, int source_pref, int step_policy, uint64_t seed_base) {
            const Preset& preset = presets[static_cast<size_t>(idx) % presets.size()];
            Config cfg;
            cfg.order_type = order_type;
            cfg.source_pref = source_pref;
            cfg.step_policy = step_policy;
            cfg.lock_style = (idx / 11) % 3;
            cfg.cand_limit = preset.cand_limit;
            cfg.cand_a = preset.cand_a;
            cfg.cand_b = preset.cand_b;
            cfg.cand_c = preset.cand_c;
            cfg.step_q = preset.step_q;
            cfg.step_t = preset.step_t;
            cfg.candidate_slack = preset.slack;
            cfg.max_passes = preset.max_passes;
            cfg.seed = splitmix64(seed_base + static_cast<uint64_t>(idx) * 0x9e3779b97f4a7c15ULL);
            cfg.id = "cfg" + to_string(idx) +
                     "/seed" + to_string(seed_base) +
                     "/o" + to_string(cfg.order_type) +
                     "/s" + to_string(cfg.source_pref) +
                     "/p" + to_string(cfg.step_policy);
            return cfg;
        };
        vector<pair<int, uint64_t>> priority = {
            {0, opt_.anytime_seed},
            {8, opt_.anytime_seed + 1},
            {16, opt_.anytime_seed + 2},
            {24, opt_.anytime_seed + 3},
            {33, opt_.anytime_seed + 2},
            {64, opt_.anytime_seed + 1},
            {91, opt_.anytime_seed},
            {111, opt_.anytime_seed + 4},
        };
        for (auto [idx, seed_base] : priority) {
            if (static_cast<int>(configs.size()) >= count) {
                break;
            }
            configs.push_back(make_cfg(idx, idx % 8, (idx / 2) % 5, (idx / 5) % 4, seed_base));
        }
        for (int i = 0; i < count; ++i) {
            bool already = false;
            for (auto [idx, seed_base] : priority) {
                (void)seed_base;
                if (idx == i) {
                    already = true;
                    break;
                }
            }
            if (already || static_cast<int>(configs.size()) >= count) {
                continue;
            }
            configs.push_back(make_cfg(i, i % 8, (i / 2) % 5, (i / 5) % 4,
                                       opt_.anytime_seed + static_cast<uint64_t>(i / 40)));
        }
        return configs;
    }

    vector<int> target_order(const Config& cfg, mt19937_64& rng) const {
        vector<int> order;
        order.reserve(p_.inner_positions.size());
        for (int pos : p_.inner_positions) {
            if (p_.target_at[pos] != -1) {
                order.push_back(pos);
            }
        }
        auto by_row = [&](int a, int b) {
            auto [ar, ac] = p_.rc(a);
            auto [br, bc] = p_.rc(b);
            return ar == br ? ac < bc : ar < br;
        };
        auto by_col = [&](int a, int b) {
            auto [ar, ac] = p_.rc(a);
            auto [br, bc] = p_.rc(b);
            return ac == bc ? ar < br : ac < bc;
        };
        auto push_row_serpentine = [&](bool reverse_rows) {
            order.clear();
            if (!reverse_rows) {
                for (int r = 1; r <= p_.n - 2; ++r) {
                    if ((r - 1) % 2 == 0) {
                        for (int c = 1; c <= p_.n - 2; ++c) order.push_back(p_.index(r, c));
                    } else {
                        for (int c = p_.n - 2; c >= 1; --c) order.push_back(p_.index(r, c));
                    }
                }
            } else {
                for (int r = p_.n - 2; r >= 1; --r) {
                    if ((p_.n - 2 - r) % 2 == 0) {
                        for (int c = p_.n - 2; c >= 1; --c) order.push_back(p_.index(r, c));
                    } else {
                        for (int c = 1; c <= p_.n - 2; ++c) order.push_back(p_.index(r, c));
                    }
                }
            }
        };
        auto push_col_serpentine = [&](bool reverse_cols) {
            order.clear();
            if (!reverse_cols) {
                for (int c = 1; c <= p_.n - 2; ++c) {
                    if ((c - 1) % 2 == 0) {
                        for (int r = 1; r <= p_.n - 2; ++r) order.push_back(p_.index(r, c));
                    } else {
                        for (int r = p_.n - 2; r >= 1; --r) order.push_back(p_.index(r, c));
                    }
                }
            } else {
                for (int c = p_.n - 2; c >= 1; --c) {
                    if ((p_.n - 2 - c) % 2 == 0) {
                        for (int r = p_.n - 2; r >= 1; --r) order.push_back(p_.index(r, c));
                    } else {
                        for (int r = 1; r <= p_.n - 2; ++r) order.push_back(p_.index(r, c));
                    }
                }
            }
        };
        if (cfg.order_type == 0) {
            push_row_serpentine(false);
        } else if (cfg.order_type == 1) {
            push_row_serpentine(true);
        } else if (cfg.order_type == 2) {
            push_col_serpentine(false);
        } else if (cfg.order_type == 3) {
            push_col_serpentine(true);
        } else if (cfg.order_type == 4) {
            sort(order.begin(), order.end(), [&](int a, int b) {
                auto [ar, ac] = p_.rc(a);
                auto [br, bc] = p_.rc(b);
                int da = min(ar - 1, p_.n - 2 - ar);
                int db = min(br - 1, p_.n - 2 - br);
                return da == db ? by_row(a, b) : da < db;
            });
        } else if (cfg.order_type == 5) {
            sort(order.begin(), order.end(), [&](int a, int b) {
                auto [ar, ac] = p_.rc(a);
                auto [br, bc] = p_.rc(b);
                int da = min(ac - 1, p_.n - 2 - ac);
                int db = min(bc - 1, p_.n - 2 - bc);
                return da == db ? by_col(a, b) : da < db;
            });
        } else if (cfg.order_type == 6) {
            sort(order.begin(), order.end(), [&](int a, int b) {
                auto [ar, ac] = p_.rc(a);
                auto [br, bc] = p_.rc(b);
                int da = ar + ac;
                int db = br + bc;
                int ta = (ar % 2 == 0) ? ac : -ac;
                int tb = (br % 2 == 0) ? bc : -bc;
                return da == db ? ta < tb : da < db;
            });
        } else {
            sort(order.begin(), order.end(), [&](int a, int b) {
                auto [ar, ac] = p_.rc(a);
                auto [br, bc] = p_.rc(b);
                int da = abs(ar - p_.n / 2) + abs(ac - p_.n / 2);
                int db = abs(br - p_.n / 2) + abs(bc - p_.n / 2);
                return da == db ? by_row(a, b) : da < db;
            });
        }
        if ((cfg.seed & 1ULL) || cfg.order_type == 7) {
            const int block = max(2, min(12, p_.n / 2));
            for (int i = 0; i < static_cast<int>(order.size()); i += block) {
                int j = min<int>(order.size(), i + block);
                shuffle(order.begin() + i, order.begin() + j, rng);
            }
        }
        return order;
    }

    vector<int> bfs_path(const RunState& state, int start, int goal, int extra_block) {
        if (start == goal) {
            return {start};
        }
        ++stamp_;
        if (stamp_ == numeric_limits<int>::max()) {
            fill(seen_.begin(), seen_.end(), 0);
            stamp_ = 1;
        }
        deque<int> q;
        q.push_back(start);
        seen_[start] = stamp_;
        parent_[start] = -1;
        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        while (!q.empty() && !time_exhausted()) {
            int cur = q.front();
            q.pop_front();
            auto [r, c] = p_.rc(cur);
            for (int k = 0; k < 4; ++k) {
                int nr = r + dr[k];
                int nc = c + dc[k];
                if (!p_.in_bounds(nr, nc)) {
                    continue;
                }
                int nxt = p_.index(nr, nc);
                if (seen_[nxt] == stamp_) {
                    continue;
                }
                if (nxt != goal && (state.locked[nxt] || nxt == extra_block)) {
                    continue;
                }
                seen_[nxt] = stamp_;
                parent_[nxt] = cur;
                if (nxt == goal) {
                    vector<int> path;
                    for (int x = goal; x != -1; x = parent_[x]) {
                        path.push_back(x);
                    }
                    reverse(path.begin(), path.end());
                    return path;
                }
                q.push_back(nxt);
            }
        }
        return {};
    }

    bool append_applied_move(RunState& state, char cmd) {
        if (!apply_move(p_, state.board, state.blank, cmd)) {
            return false;
        }
        if (!state.moves.empty() && inverse_command(cmd) == state.moves.back()) {
            state.moves.pop_back();
            ++cancelled_opposites_;
        } else {
            state.moves.push_back(cmd);
        }
        return true;
    }

    bool move_blank_to(RunState& state, int dest, int controlled_tile) {
        vector<int> path = bfs_path(state, state.blank, dest, controlled_tile);
        if (path.empty()) {
            return false;
        }
        for (size_t i = 1; i < path.size(); ++i) {
            char cmd = command_for_blank_step(p_, state.blank, path[i]);
            if (!append_applied_move(state, cmd)) {
                return false;
            }
        }
        return true;
    }

    void build_dist_to_target(const RunState& state, int target) {
        fill(dist_to_target_.begin(), dist_to_target_.end(), -1);
        deque<int> q;
        q.push_back(target);
        dist_to_target_[target] = 0;
        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        while (!q.empty() && !time_exhausted()) {
            int cur = q.front();
            q.pop_front();
            auto [r, c] = p_.rc(cur);
            for (int k = 0; k < 4; ++k) {
                int nr = r + dr[k];
                int nc = c + dc[k];
                if (!p_.in_bounds(nr, nc)) {
                    continue;
                }
                int nxt = p_.index(nr, nc);
                if (dist_to_target_[nxt] != -1) {
                    continue;
                }
                if (state.locked[nxt] && nxt != target) {
                    continue;
                }
                dist_to_target_[nxt] = dist_to_target_[cur] + 1;
                q.push_back(nxt);
            }
        }
    }

    void build_blank_dist(const RunState& state, int blocked_tile) {
        fill(blank_dist_.begin(), blank_dist_.end(), -1);
        fill(blank_parent_.begin(), blank_parent_.end(), -1);
        deque<int> q;
        q.push_back(state.blank);
        blank_dist_[state.blank] = 0;
        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        while (!q.empty() && !time_exhausted()) {
            int cur = q.front();
            q.pop_front();
            auto [r, c] = p_.rc(cur);
            for (int k = 0; k < 4; ++k) {
                int nr = r + dr[k];
                int nc = c + dc[k];
                if (!p_.in_bounds(nr, nc)) {
                    continue;
                }
                int nxt = p_.index(nr, nc);
                if (blank_dist_[nxt] != -1 || state.locked[nxt] || nxt == blocked_tile) {
                    continue;
                }
                blank_dist_[nxt] = blank_dist_[cur] + 1;
                blank_parent_[nxt] = cur;
                q.push_back(nxt);
            }
        }
    }

    vector<int> blank_path_to(int dest) const {
        if (dest < 0 || dest >= static_cast<int>(blank_dist_.size()) || blank_dist_[dest] < 0) {
            return {};
        }
        vector<int> path;
        for (int cur = dest; cur != -1; cur = blank_parent_[cur]) {
            path.push_back(cur);
        }
        reverse(path.begin(), path.end());
        return path;
    }

    int center_traffic(const RunState& state, const vector<int>& path) const {
        int traffic = 0;
        for (int pos : path) {
            if (p_.is_inner(pos) && !state.locked[pos]) {
                ++traffic;
            }
        }
        return traffic;
    }

    double source_score(const RunState& state, const Config& cfg, int source, int target) const {
        if (source < 0 || source >= static_cast<int>(dist_to_target_.size()) ||
            dist_to_target_[source] < 0) {
            return 1e100;
        }
        int support = kInf;
        auto [tr, tc] = p_.rc(source);
        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            int nr = tr + dr[k];
            int nc = tc + dc[k];
            if (!p_.in_bounds(nr, nc)) {
                continue;
            }
            int adj = p_.index(nr, nc);
            if (!state.locked[adj] && blank_dist_[adj] >= 0) {
                support = min(support, blank_dist_[adj]);
            }
        }
        if (support == kInf) {
            return 1e100;
        }
        double score = static_cast<double>(cfg.cand_a * dist_to_target_[source] +
                                           cfg.cand_b * support);
        auto [sr, sc] = p_.rc(source);
        bool source_inner = p_.is_inner(source);
        bool source_border = sr == 0 || sc == 0 || sr + 1 == p_.n || sc + 1 == p_.n;
        if (source_inner && source != target) {
            score += cfg.cand_c * 4.0;
        }
        if (source_border) {
            score -= cfg.source_pref == 1 ? 18.0 : 4.0;
        }
        if (cfg.source_pref == 2) {
            score += 1.5 * support;
        }
        if (cfg.source_pref == 3 && p_.target_at[source] == unpack_cell(state.board[source])) {
            score += 30.0;
        }
        if (cfg.source_pref == 4 && source_inner) {
            score += p_.manhattan(source, target) <= 2 ? 6.0 : 18.0;
        }
        if (source == target) {
            score -= 100.0;
        }
        return score;
    }

    vector<SourceCandidate> collect_sources(const RunState& state, const Config& cfg, int target) {
        vector<SourceCandidate> sources;
        int want = p_.target_at[target];
        build_dist_to_target(state, target);
        build_blank_dist(state, -1);
        for (int pos = 0; pos < static_cast<int>(state.board.size()); ++pos) {
            if (state.locked[pos] || unpack_cell(state.board[pos]) != want) {
                continue;
            }
            double score = source_score(state, cfg, pos, target);
            if (score < 1e90) {
                sources.push_back({pos, score});
            }
        }
        sort(sources.begin(), sources.end(), [](const SourceCandidate& a, const SourceCandidate& b) {
            if (a.score != b.score) return a.score < b.score;
            return a.pos < b.pos;
        });
        if (!sources.empty()) {
            vector<SourceCandidate> filtered;
            const double best = sources.front().score;
            for (const SourceCandidate& source : sources) {
                if (static_cast<int>(filtered.size()) >= cfg.cand_limit) {
                    break;
                }
                if (source.score <= best + cfg.candidate_slack * 8.0 ||
                    static_cast<int>(filtered.size()) < max(4, cfg.cand_limit / 4)) {
                    filtered.push_back(source);
                }
            }
            sources.swap(filtered);
        }
        return sources;
    }

    double future_score(const RunState& state, const vector<int>& order, int next_index, int depth) const {
        double score = 0;
        int used = 0;
        for (int i = next_index; i < static_cast<int>(order.size()) && used < depth; ++i) {
            int target = order[i];
            if (state.locked[target] || unpack_cell(state.board[target]) == p_.target_at[target]) {
                continue;
            }
            int best = kInf / 4;
            for (int pos = 0; pos < static_cast<int>(state.board.size()); ++pos) {
                if (!state.locked[pos] && unpack_cell(state.board[pos]) == p_.target_at[target]) {
                    best = min(best, p_.manhattan(pos, target));
                }
            }
            score += best;
            ++used;
        }
        return score;
    }

    bool transport_tile_shortest(RunState& state, int source, int target) {
        if (source == target) {
            return true;
        }
        int tile = source;
        vector<int> tile_path = bfs_path(state, tile, target, -1);
        if (tile_path.empty()) {
            return false;
        }
        for (size_t i = 1; i < tile_path.size(); ++i) {
            if (time_exhausted()) {
                return false;
            }
            int next_tile_pos = tile_path[i];
            if (!move_blank_to(state, next_tile_pos, tile)) {
                return false;
            }
            char cmd = command_for_tile_step(p_, tile, next_tile_pos);
            if (!append_applied_move(state, cmd)) {
                return false;
            }
            tile = next_tile_pos;
        }
        return tile == target;
    }

    bool transport_tile_monotone(RunState& state, const Config& cfg, int source, int target) {
        if (source == target) {
            return true;
        }
        int tile = source;
        mt19937_64 rng(cfg.seed ^ static_cast<uint64_t>(state.moves.size() + source * 911 + target * 3571));
        unordered_set<uint64_t> seen_pairs;
        seen_pairs.reserve(256);
        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        const int guard_limit = max(16, p_.n * p_.n);
        for (int guard = 0; guard < guard_limit && tile != target; ++guard) {
            if (time_exhausted()) {
                return false;
            }
            build_dist_to_target(state, target);
            if (tile < 0 || tile >= static_cast<int>(dist_to_target_.size()) ||
                dist_to_target_[tile] <= 0) {
                return false;
            }
            const int cur_dist = dist_to_target_[tile];
            auto [tr, tc] = p_.rc(tile);
            struct Step {
                double score = 1e100;
                int next_tile = -1;
                vector<int> blank_path;
            };
            vector<Step> steps;
            for (int k = 0; k < 4; ++k) {
                int nr = tr + dr[k];
                int nc = tc + dc[k];
                if (!p_.in_bounds(nr, nc)) {
                    continue;
                }
                int next_tile = p_.index(nr, nc);
                if (next_tile != target && state.locked[next_tile]) {
                    continue;
                }
                if (dist_to_target_[next_tile] != cur_dist - 1) {
                    continue;
                }
                build_blank_dist(state, tile);
                vector<int> blank_path = blank_path_to(next_tile);
                if (blank_path.empty()) {
                    continue;
                }
                double score = static_cast<double>(cfg.step_q * max<int>(0, blank_path.size() - 1) +
                                                   cfg.step_t * center_traffic(state, blank_path));
                auto [nr2, nc2] = p_.rc(next_tile);
                const bool border = nr2 == 0 || nc2 == 0 || nr2 + 1 == p_.n || nc2 + 1 == p_.n;
                if (border && cfg.step_policy == 1) {
                    score -= 5.0;
                }
                if (cfg.step_policy == 2 && p_.is_inner(next_tile)) {
                    score += 2.0;
                }
                uint64_t next_key = (static_cast<uint64_t>(next_tile) << 32) ^ static_cast<uint64_t>(tile);
                if (seen_pairs.find(next_key) != seen_pairs.end()) {
                    score += 100.0;
                }
                score += static_cast<double>(splitmix64(cfg.seed + guard * 17ULL + k) & 1023ULL) / 4096.0;
                steps.push_back({score, next_tile, std::move(blank_path)});
            }
            if (steps.empty()) {
                return false;
            }
            sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) {
                if (a.score != b.score) return a.score < b.score;
                return a.next_tile < b.next_tile;
            });
            int choice_limit = 1;
            while (choice_limit < static_cast<int>(steps.size()) &&
                   steps[choice_limit].score <= steps.front().score + 1.0) {
                ++choice_limit;
            }
            const Step& best = steps[static_cast<int>(rng() % static_cast<uint64_t>(choice_limit))];
            seen_pairs.insert((static_cast<uint64_t>(tile) << 32) ^ static_cast<uint64_t>(state.blank));
            for (size_t i = 1; i < best.blank_path.size(); ++i) {
                char cmd = command_for_blank_step(p_, state.blank, best.blank_path[i]);
                if (!append_applied_move(state, cmd)) {
                    return false;
                }
            }
            char cmd = command_for_tile_step(p_, tile, best.next_tile);
            if (!append_applied_move(state, cmd)) {
                return false;
            }
            tile = best.next_tile;
        }
        return tile == target;
    }

    bool transport_tile(RunState& state, const Config& cfg, int source, int target, bool allow_fallback = true) {
        if (source == target) {
            return true;
        }
        if (opt_.anytime_engine == "monotone") {
            RunState saved = state;
            if (transport_tile_monotone(state, cfg, source, target)) {
                return true;
            }
            if (!allow_fallback) {
                return false;
            }
            state = saved;
            return transport_tile_shortest(state, source, target);
        }
        RunState saved = state;
        auto fallback = [&]() {
            if (!allow_fallback) {
                return false;
            }
            state = saved;
            return transport_tile_shortest(state, source, target);
        };
        int tile = source;
        mt19937_64 rng(cfg.seed ^ static_cast<uint64_t>(state.moves.size() + source * 131 + target));
        unordered_set<uint64_t> seen_states;
        seen_states.reserve(512);
        const int dr[4] = {1, -1, 0, 0};
        const int dc[4] = {0, 0, 1, -1};
        int guard_limit = max(32, p_.n * p_.n * 2);
        for (int guard = 0; guard < guard_limit && tile != target; ++guard) {
            if (time_exhausted()) {
                return false;
            }
            uint64_t state_key = (static_cast<uint64_t>(tile) << 32) ^ static_cast<uint64_t>(state.blank);
            seen_states.insert(state_key);
            auto [tr, tc] = p_.rc(tile);
            struct Step {
                double score = 1e100;
                int next_tile = -1;
                vector<int> blank_path;
            };
            vector<Step> steps;
            for (int k = 0; k < 4; ++k) {
                int nr = tr + dr[k];
                int nc = tc + dc[k];
                if (!p_.in_bounds(nr, nc)) {
                    continue;
                }
                int next_tile = p_.index(nr, nc);
                if (next_tile != target && state.locked[next_tile]) {
                    continue;
                }
                vector<int> blank_path = bfs_path(state, state.blank, next_tile, tile);
                if (blank_path.empty()) {
                    continue;
                }
                int cur_dist = p_.manhattan(tile, target);
                int next_dist = p_.manhattan(next_tile, target);
                double score = static_cast<double>(blank_path.size() - 1) + 4.5 * next_dist;
                if (next_dist > cur_dist) {
                    score += cfg.step_policy == 0 ? 25.0 : 8.0;
                }
                if (cfg.step_policy == 1) {
                    auto [nr2, nc2] = p_.rc(next_tile);
                    if (nr2 == 0 || nc2 == 0 || nr2 + 1 == p_.n || nc2 + 1 == p_.n) {
                        score -= 4.0;
                    }
                }
                uint64_t next_key = (static_cast<uint64_t>(next_tile) << 32) ^ static_cast<uint64_t>(tile);
                if (seen_states.find(next_key) != seen_states.end()) {
                    score += 80.0;
                }
                score += static_cast<double>(splitmix64(cfg.seed + guard + k) & 1023ULL) / 4096.0;
                steps.push_back({score, next_tile, std::move(blank_path)});
            }
            if (steps.empty()) {
                return fallback();
            }
            sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) {
                if (a.score != b.score) return a.score < b.score;
                return a.next_tile < b.next_tile;
            });
            int choice_limit = 1;
            while (choice_limit < static_cast<int>(steps.size()) &&
                   steps[choice_limit].score <= steps.front().score + 1.5) {
                ++choice_limit;
            }
            const Step& best = steps[static_cast<int>(rng() % static_cast<uint64_t>(choice_limit))];
            for (size_t i = 1; i < best.blank_path.size(); ++i) {
                char cmd = command_for_blank_step(p_, state.blank, best.blank_path[i]);
                if (!append_applied_move(state, cmd)) {
                    return fallback();
                }
            }
            char cmd = command_for_tile_step(p_, tile, best.next_tile);
            if (!append_applied_move(state, cmd)) {
                return fallback();
            }
            tile = best.next_tile;
        }
        if (tile == target) {
            return true;
        }
        return fallback();
    }

    optional<int> choose_source_with_rollout(const RunState& state,
                                             const Config& cfg,
                                             const vector<int>& order,
                                             int order_index,
                                             int target,
                                             mt19937_64& rng) {
        vector<SourceCandidate> sources = collect_sources(state, cfg, target);
        if (sources.empty()) {
            return nullopt;
        }
        int limit = min<int>(sources.size(), max(1, opt_.rollout_top_k));
        vector<SourceCandidate> scored;
        for (int i = 0; i < limit; ++i) {
            SourceCandidate cand = sources[i];
            if (opt_.rollout_depth > 0) {
                RunState sim = state;
                if (transport_tile(sim, cfg, cand.pos, target, false) &&
                    unpack_cell(sim.board[target]) == p_.target_at[target]) {
                    sim.locked[target] = 1;
                    cand.score += 1.5 * future_score(sim, order, order_index + 1, opt_.rollout_depth);
                    cand.score += 0.02 * static_cast<double>(sim.moves.size() - state.moves.size());
                } else {
                    cand.score += 1e6;
                }
            }
            cand.score += static_cast<double>(rng() & 2047ULL) / 4096.0;
            scored.push_back(cand);
        }
        sort(scored.begin(), scored.end(), [](const SourceCandidate& a, const SourceCandidate& b) {
            if (a.score != b.score) return a.score < b.score;
            return a.pos < b.pos;
        });
        int near = 1;
        while (near < static_cast<int>(scored.size()) && scored[near].score <= scored.front().score + 2.0) {
            ++near;
        }
        return scored[static_cast<int>(rng() % static_cast<uint64_t>(near))].pos;
    }

    PartialCheckpoint make_checkpoint(const RunState& state) const {
        return {state.board, state.blank, state.locked, state.moves, state.fixed_count};
    }

    optional<string> solve_config(const Config& cfg,
                                  const PartialCheckpoint* checkpoint,
                                  vector<PartialCheckpoint>* checkpoints) {
        mt19937_64 rng(cfg.seed);
        RunState state;
        if (checkpoint != nullptr) {
            state.board = checkpoint->board;
            state.blank = checkpoint->blank;
            state.locked = checkpoint->locked;
            state.moves = checkpoint->moves;
            state.fixed_count = checkpoint->fixed_count;
        } else {
            state.board = pack_board(p_.initial);
            state.blank = p_.initial_blank;
            state.locked.assign(p_.n * p_.n, 0);
            state.fixed_count = 0;
        }
        vector<int> order = target_order(cfg, rng);
        vector<int> checkpoint_thresholds;
        if (checkpoints != nullptr) {
            const double ratios[] = {0.40, 0.50, 0.60, 0.70, 0.80, 0.88, 0.93, 0.97};
            for (double ratio : ratios) {
                int threshold = static_cast<int>(ratio * static_cast<double>(p_.inner_positions.size()));
                threshold = min<int>(p_.inner_positions.size(), max(1, threshold));
                checkpoint_thresholds.push_back(threshold);
            }
            sort(checkpoint_thresholds.begin(), checkpoint_thresholds.end());
            checkpoint_thresholds.erase(unique(checkpoint_thresholds.begin(), checkpoint_thresholds.end()),
                                        checkpoint_thresholds.end());
        }
        size_t next_checkpoint = 0;
        int blank_target = -1;
        for (int pos : p_.inner_positions) {
            if (p_.target_at[pos] == -1) {
                blank_target = pos;
                break;
            }
        }

        const int total_targets = static_cast<int>(order.size());
        auto save_checkpoint_if_needed = [&]() {
            if (checkpoints != nullptr &&
                next_checkpoint < checkpoint_thresholds.size() &&
                state.fixed_count >= checkpoint_thresholds[next_checkpoint]) {
                checkpoints->push_back(make_checkpoint(state));
                while (next_checkpoint < checkpoint_thresholds.size() &&
                       state.fixed_count >= checkpoint_thresholds[next_checkpoint]) {
                    ++next_checkpoint;
                }
                if (checkpoints->size() > 24) {
                    checkpoints->erase(checkpoints->begin());
                }
            }
        };

        for (int pass = 0; pass < max(1, cfg.max_passes) && state.fixed_count < total_targets; ++pass) {
            bool progress = false;
            int begin = pass % 2 == 0 ? 0 : static_cast<int>(order.size()) - 1;
            int end = pass % 2 == 0 ? static_cast<int>(order.size()) : -1;
            int step = pass % 2 == 0 ? 1 : -1;
            for (int oi = begin; oi != end; oi += step) {
                int target = order[oi];
                if (time_exhausted()) {
                    return nullopt;
                }
                if (state.locked[target]) {
                    continue;
                }
                if (unpack_cell(state.board[target]) == p_.target_at[target]) {
                    state.locked[target] = 1;
                    ++state.fixed_count;
                    progress = true;
                } else {
                    RunState before = state;
                    optional<int> source = choose_source_with_rollout(state, cfg, order, oi, target, rng);
                    if (!source || !transport_tile(state, cfg, *source, target) ||
                        unpack_cell(state.board[target]) != p_.target_at[target]) {
                        state = std::move(before);
                        continue;
                    }
                    state.locked[target] = 1;
                    ++state.fixed_count;
                    progress = true;
                }
                save_checkpoint_if_needed();
                if (state.moves.size() >= incumbent_length_) {
                    return nullopt;
                }
            }
            if (!progress) {
                break;
            }
        }

        if (state.fixed_count < total_targets) {
            return nullopt;
        }

        if (blank_target >= 0) {
            if (!move_blank_to(state, blank_target, -1)) {
                return nullopt;
            }
        }
        if (state.moves.size() >= incumbent_length_) {
            return nullopt;
        }
        if (!validate_solution(p_, state.moves + "S")) {
            return nullopt;
        }
        return state.moves;
    }

    bool add_elite(const string& path, const Config& cfg, vector<PartialCheckpoint> checkpoints) {
        EliteSolution elite{path, cfg, std::move(checkpoints)};
        elites_.push_back(std::move(elite));
        sort(elites_.begin(), elites_.end(), [](const EliteSolution& a, const EliteSolution& b) {
            if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
            return a.config.id < b.config.id;
        });
        bool added = false;
        if (static_cast<int>(elites_.size()) <= opt_.elite_size ||
            path.size() <= elites_[min<int>(elites_.size(), opt_.elite_size) - 1].path.size()) {
            added = true;
            ++elite_updates_;
        }
        if (static_cast<int>(elites_.size()) > opt_.elite_size) {
            elites_.resize(opt_.elite_size);
        }
        return added;
    }

    const Problem& p_;
    const Options& opt_;
    const Timer& timer_;
    int deadline_ms_ = 0;
    int attempt_deadline_ms_ = 0;
    size_t incumbent_length_ = 0;
    vector<int> parent_;
    vector<int> seen_;
    vector<int> dist_to_target_;
    vector<int> blank_dist_;
    vector<int> blank_parent_;
    int stamp_ = 0;
    vector<EliteSolution> elites_;
    uint64_t elite_updates_ = 0;
    uint64_t cancelled_opposites_ = 0;
};

// Owns the production pipeline:
// paired seed -> multi-start constructor -> elite repair -> compression -> bounded patch cleanup.
// The PATCH01 checkpoint name is kept for backward compatibility with older runs.
class ConstructorFirstSolver {
public:
    ConstructorFirstSolver(const Problem& p, const Options& opt) : p_(p), opt_(opt) {
        snapshot_stride_ = max(1, opt_.snapshot_stride);
        unsigned hw = thread::hardware_concurrency();
        int default_threads = max(1, static_cast<int>(hw == 0 ? 2 : hw) - 1);
        patch_threads_ = opt_.patch_commit_policy == "deterministic"
                             ? 1
                             : max(1, opt_.patch_threads > 0 ? opt_.patch_threads : default_threads);
        patch_batch_size_ = max(1, opt_.patch_batch_size > 0 ? opt_.patch_batch_size : patch_threads_ * 4);
        patch_batch_timeslice_ms_ = max(1, opt_.patch_batch_timeslice_ms);
    }

    bool load_checkpoint(const string& path) {
        if (path.empty()) {
            return false;
        }
        ifstream in(path, ios::binary);
        if (!in) {
            return false;
        }
        char magic[8] = {};
        in.read(magic, sizeof(magic));
        string version(magic, magic + 7);
        if (version != "PATCH01") {
            return false;
        }
        int n = 0;
        Hash128 root_hash;
        read_raw(in, n);
        read_raw(in, root_hash.lo);
        read_raw(in, root_hash.hi);
        if (n != p_.n || !(root_hash == p_.initial_hash)) {
            throw runtime_error("patch checkpoint does not match this input");
        }
        best_path_ = read_string(in);
        best_valid_solution_ = best_path_;
        best_source_ = "checkpoint";
        read_raw(in, scan_cursor_);
        read_raw(in, accepted_improvements_);
        read_raw(in, snapshot_stride_);
        snapshot_stride_ = max(1, snapshot_stride_);
        uint64_t recent_count = 0;
        read_raw(in, recent_count);
        recent_patches_.clear();
        for (uint64_t i = 0; i < recent_count; ++i) {
            int a = 0;
            int b = 0;
            read_raw(in, a);
            read_raw(in, b);
            recent_patches_.push_back({a, b});
        }
        uint64_t rejected_count = 0;
        if (in.read(reinterpret_cast<char*>(&rejected_count), sizeof(rejected_count))) {
            rejected_candidates_.clear();
            for (uint64_t i = 0; i < rejected_count; ++i) {
                uint64_t key = 0;
                read_raw(in, key);
                rejected_candidates_.insert(key);
            }
            uint64_t stored_generation = 0;
            if (in.read(reinterpret_cast<char*>(&stored_generation), sizeof(stored_generation))) {
                generation_ = stored_generation;
                read_raw(in, total_batches_);
                read_raw(in, total_5x5_batches_);
                read_raw(in, total_jobs_);
                if (!in.read(reinterpret_cast<char*>(&macro_attempts_), sizeof(macro_attempts_))) {
                    clear_macro_stats();
                    clear_anytime_stats();
                    clear_large_stats();
                } else {
                    read_raw(in, macro_commits_);
                    if (in.read(reinterpret_cast<char*>(&macro_transport_attempts_), sizeof(macro_transport_attempts_))) {
                        read_raw(in, macro_transport_commits_);
                        read_raw(in, macro_strip_attempts_);
                        read_raw(in, macro_strip_commits_);
                        read_raw(in, macro_sweep_attempts_);
                        read_raw(in, macro_sweep_commits_);
                        read_raw(in, macro_stall_switches_);
                        if (in.read(reinterpret_cast<char*>(&anytime_configs_attempted_),
                                    sizeof(anytime_configs_attempted_))) {
                            read_raw(in, anytime_valid_solutions_);
                            read_raw(in, anytime_elite_updates_);
                            read_raw(in, anytime_repair_jobs_);
                            read_raw(in, anytime_repair_commits_);
                            if (in.read(reinterpret_cast<char*>(&large_attempts_), sizeof(large_attempts_))) {
                                read_raw(in, large_commits_);
                                read_raw(in, large_sweep_attempts_);
                                read_raw(in, large_sweep_commits_);
                                read_raw(in, large_construct_attempts_);
                                read_raw(in, large_construct_commits_);
                                read_raw(in, large_construct_configs_);
                                if (in.read(reinterpret_cast<char*>(&large_center_loop_candidates_),
                                            sizeof(large_center_loop_candidates_))) {
                                    read_raw(in, large_center_loop_commits_);
                                    read_raw(in, large_center_loop_best_delta_);
                                    read_raw(in, large_beam_states_);
                                    read_raw(in, large_band_commits_);
                                    if (!in.read(reinterpret_cast<char*>(&anytime_monotone_attempts_),
                                                 sizeof(anytime_monotone_attempts_))) {
                                        anytime_monotone_attempts_ = 0;
                                        anytime_monotone_valid_ = 0;
                                        anytime_repair_valid_ = 0;
                                        anytime_checkpoint_jobs_ = 0;
                                        anytime_cancelled_opposites_ = 0;
                                    } else {
                                        read_raw(in, anytime_monotone_valid_);
                                        read_raw(in, anytime_repair_valid_);
                                        read_raw(in, anytime_checkpoint_jobs_);
                                        read_raw(in, anytime_cancelled_opposites_);
                                        if (!in.read(reinterpret_cast<char*>(&large_partial_states_),
                                                     sizeof(large_partial_states_))) {
                                            large_partial_states_ = 0;
                                            large_suffix_jobs_ = 0;
                                            large_suffix_valid_ = 0;
                                            large_band_v2_commits_ = 0;
                                        } else {
                                        read_raw(in, large_suffix_jobs_);
                                        read_raw(in, large_suffix_valid_);
                                        read_raw(in, large_band_v2_commits_);
                                        if (!in.read(reinterpret_cast<char*>(&large_patch_cleanup_ms_),
                                                     sizeof(large_patch_cleanup_ms_))) {
                                            large_patch_cleanup_ms_ = 0;
                                            large_patch_cleanup_commits_ = 0;
                                        } else {
                                            read_raw(in, large_patch_cleanup_commits_);
                                        }
                                    }
                                    }
                                } else {
                                    clear_large_extra_stats();
                                }
                            } else {
                                clear_large_stats();
                            }
                        } else {
                            clear_anytime_stats();
                            clear_large_stats();
                        }
                    } else {
                        macro_transport_attempts_ = 0;
                        macro_transport_commits_ = 0;
                        macro_strip_attempts_ = 0;
                        macro_strip_commits_ = 0;
                        macro_sweep_attempts_ = 0;
                        macro_sweep_commits_ = 0;
                        macro_stall_switches_ = 0;
                        clear_anytime_stats();
                        clear_large_stats();
                    }
                }
            } else {
                clear_checkpoint_tail_stats();
            }
        } else {
            rejected_candidates_.clear();
            clear_checkpoint_tail_stats();
        }
        if (!validate_solution(p_, best_path_ + "S")) {
            throw runtime_error("patch checkpoint contains an invalid solution");
        }
        rebuild_trajectory();
        return true;
    }

    void set_seed(const string& path, const string& source) {
        best_path_ = path;
        best_valid_solution_ = path;
        best_source_ = source;
        scan_cursor_ = 0;
        clear_run_stats();
        recent_patches_.clear();
        rejected_candidates_.clear();
        if (!validate_solution(p_, best_path_ + "S")) {
            throw runtime_error("constructive seed is invalid");
        }
        rebuild_trajectory();
        publish("seed");
    }

    void run(const Timer& timer) {
        if (best_path_.empty() && !p_.is_goal(p_.initial)) {
            throw runtime_error("constructor-first solver has no seed solution");
        }
        if (best_path_.empty()) {
            publish("already_goal");
            return;
        }

        if (p_.n == 5 && best_valid_solution_.size() <= 30) {
            exit_reason_ = "small_board_target";
            last_run_elapsed_ms_ = timer.elapsed_ms();
            best_path_ = best_valid_solution_;
            publish("final");
            save_checkpoint(opt_.checkpoint_path);
            return;
        }
        try_small_board_search(timer);
        if (p_.n == 5 && best_valid_solution_.size() <= 30) {
            exit_reason_ = "small_board_target";
            last_run_elapsed_ms_ = timer.elapsed_ms();
            best_path_ = best_valid_solution_;
            publish("final");
            save_checkpoint(opt_.checkpoint_path);
            return;
        }
        if (p_.n >= 6) {
            try_anytime_constructive(timer);
            if (p_.n >= 30 && opt_.large_engine != "off") {
                try_large_board_resynthesis(timer);
            }
            while (try_macro_resynthesis(timer, "initial") && !time_exhausted(timer)) {
            }
        }

        const int commit_limit = opt_.patch_max_commits;
        const long long patch_start_ms = timer.elapsed_ms();
        const long long cleanup_budget_ms =
            opt_.no_cleanup
                ? 0
                : (opt_.cleanup_ms >= 0
                       ? static_cast<long long>(opt_.cleanup_ms)
                       : -1);
        const long long large_patch_budget_ms =
            p_.n >= 30
                ? (opt_.no_large_patch_cleanup
                       ? 0
                       : min<long long>(opt_.large_patch_cleanup_ms,
                                        opt_.time_limit_ms < 0
                                            ? opt_.large_patch_cleanup_ms
                                            : max<long long>(0, opt_.time_limit_ms * opt_.large_patch_ratio / 100)))
                : -1;
        uint64_t patch_commits_at_start = accepted_improvements_;
        int loops_since_5x5 = 0;
        int no_job_loops = 0;
        long long last_stats_ms = 0;
        long long last_improvement_ms = timer.elapsed_ms();
        exit_reason_ = "time_limit";
        while (!time_exhausted(timer)) {
            if (cleanup_budget_ms >= 0 && timer.elapsed_ms() - patch_start_ms >= cleanup_budget_ms) {
                exit_reason_ = opt_.no_cleanup ? "cleanup_disabled" : "cleanup_limit";
                break;
            }
            if (p_.n >= 30) {
                if (large_patch_budget_ms <= 0 || timer.elapsed_ms() - patch_start_ms >= large_patch_budget_ms) {
                    exit_reason_ = "large_patch_cleanup_limit";
                    break;
                }
                if (timer.elapsed_ms() - last_improvement_ms >= 2'000) {
                    exit_reason_ = "large_patch_stall";
                    break;
                }
            }
            if (commit_limit > 0 && accepted_improvements_ >= commit_limit) {
                exit_reason_ = "commit_limit";
                break;
            }
            if (!opt_.no_macro_resynthesis &&
                timer.elapsed_ms() - last_improvement_ms >= opt_.macro_stall_ms) {
                ++macro_stall_switches_;
                if (try_macro_resynthesis(timer, "stall")) {
                    last_improvement_ms = timer.elapsed_ms();
                    loops_since_5x5 = 0;
                    no_job_loops = 0;
                    continue;
                }
                last_improvement_ms = timer.elapsed_ms();
            }
            vector<Candidate> candidates = gather_candidates();
            vector<PatchJob> cheap_jobs = prepare_jobs(candidates, {3, 4}, static_cast<size_t>(patch_batch_size_));
            vector<PatchResult> cheap_results = run_batch(cheap_jobs, timer);
            for (const PatchJob& job : cheap_jobs) {
                rejected_candidates_.insert(job.signature);
            }
            total_batches_++;
            total_jobs_ += cheap_jobs.size();
            optional<PatchResult> best = select_result(cheap_results);
            if (best && best->delta >= max(3, best->job.old_length / 5)) {
                if (commit_result(*best)) {
                    last_improvement_ms = timer.elapsed_ms();
                    loops_since_5x5 = 0;
                    no_job_loops = 0;
                    continue;
                }
            }

            bool should_try_5x5 = p_.n >= 30 ? false : (!best || loops_since_5x5 + 1 >= max(1, opt_.patch_force_5x5_every));
            if (p_.n >= 30 && total_batches_ < 500 && !candidates.empty()) {
                should_try_5x5 = false;
            }
            optional<PatchResult> best5;
            bool had_5x5_jobs = false;
            if (should_try_5x5 && !time_exhausted(timer)) {
                vector<PatchJob> jobs5 = prepare_jobs(candidates, {5}, static_cast<size_t>(patch_batch_size_));
                had_5x5_jobs = !jobs5.empty();
                vector<PatchResult> results5 = run_batch(jobs5, timer);
                for (const PatchJob& job : jobs5) {
                    rejected_candidates_.insert(job.signature);
                }
                total_batches_++;
                total_5x5_batches_++;
                total_jobs_ += jobs5.size();
                best5 = select_result(results5);
                loops_since_5x5 = 0;
            } else {
                ++loops_since_5x5;
            }

            optional<PatchResult> chosen;
            if (best && best5) {
                chosen = better_result(*best, *best5) ? best : best5;
            } else if (best) {
                chosen = best;
            } else if (best5) {
                chosen = best5;
            }
            if (chosen && commit_result(*chosen)) {
                last_improvement_ms = timer.elapsed_ms();
                no_job_loops = 0;
                continue;
            }

            const bool had_any_jobs = !cheap_jobs.empty() || had_5x5_jobs;
            if (!had_any_jobs || candidates.empty()) {
                ++no_job_loops;
                scan_cursor_ = (scan_cursor_ + max<size_t>(4096, static_cast<size_t>(snapshot_stride_) * 64)) %
                               max<size_t>(1, best_path_.size());
                if (no_job_loops >= 8 && !rejected_candidates_.empty()) {
                    rejected_candidates_.clear();
                    no_job_loops = 0;
                }
            } else {
                no_job_loops = 0;
                scan_cursor_ = (scan_cursor_ + 997) % max<size_t>(1, best_path_.size());
            }
            if (opt_.patch_stats && timer.elapsed_ms() - last_stats_ms >= 5000) {
                last_stats_ms = timer.elapsed_ms();
                cerr << "[Patch stats: batches=" << total_batches_
                     << " jobs=" << total_jobs_
                     << " 5x5_batches=" << total_5x5_batches_
                     << " rejected=" << rejected_candidates_.size()
                     << " best=" << best_path_.size()
                     << " elapsed_ms=" << last_stats_ms << "]\n";
            }
        }
        if (time_exhausted(timer)) {
            exit_reason_ = "time_limit";
        }
        cleanup_time_ms_ += static_cast<uint64_t>(max<long long>(0, timer.elapsed_ms() - patch_start_ms));
        cleanup_commits_ += accepted_improvements_ - patch_commits_at_start;
        if (p_.n >= 30) {
            large_patch_cleanup_ms_ += static_cast<uint64_t>(max<long long>(0, timer.elapsed_ms() - patch_start_ms));
            large_patch_cleanup_commits_ += accepted_improvements_ - patch_commits_at_start;
        }
        last_run_elapsed_ms_ = timer.elapsed_ms();
        if (!validate_solution(p_, best_valid_solution_ + "S")) {
            throw runtime_error("best valid solution failed final validation");
        }
        best_path_ = best_valid_solution_;
        publish("final");
        save_checkpoint(opt_.checkpoint_path);
    }

    int best_length() const {
        return static_cast<int>(best_valid_solution_.size());
    }

    long long elapsed_ms() const {
        return last_run_elapsed_ms_;
    }

    const string& exit_reason() const {
        return exit_reason_;
    }

private:
    struct MoveInfo {
        char move = 0;
        int blank_before = -1;
        int blank_after = -1;
        int touched_a = -1;
        int touched_b = -1;
        int moved_tile = 0;
        uint64_t local_hash = 0;
    };

    struct TrajectorySnapshot {
        size_t move_index = 0;
        PackedBoard board;
        int blank = -1;
    };

    struct Candidate {
        int start = 0;
        int end = 0;
        int r0 = 0;
        int c0 = 0;
        int size = 0;
        double score = 0.0;
    };

    struct PatchJob {
        Candidate cand;
        PackedBoard start_patch;
        PackedBoard end_patch;
        uint64_t boundary_hash = 0;
        uint64_t start_patch_hash = 0;
        uint64_t endpoint_hash = 0;
        uint64_t signature = 0;
        int start_blank = -1;
        int end_blank = -1;
        int old_length = 0;
        int lower_bound = 0;
        double score_density = 0.0;
        uint64_t generation = 0;
    };

    struct PatchResult {
        bool success = false;
        PatchJob job;
        string replacement;
        int delta = 0;
    };

    uint64_t candidate_key(const Candidate& cand) const {
        uint64_t key = static_cast<uint64_t>(static_cast<uint32_t>(cand.start));
        key ^= static_cast<uint64_t>(static_cast<uint32_t>(cand.end)) << 21;
        key ^= static_cast<uint64_t>(static_cast<uint32_t>(cand.r0)) << 42;
        key ^= static_cast<uint64_t>(static_cast<uint32_t>(cand.c0)) << 50;
        key ^= static_cast<uint64_t>(static_cast<uint32_t>(cand.size)) << 58;
        return key;
    }

    uint64_t hash_packed(const PackedBoard& cells) const {
        uint64_t h = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(cells.size());
        for (uint16_t cell : cells) {
            h = splitmix64(h ^ static_cast<uint64_t>(cell + 0x10001U));
        }
        return h;
    }

    uint64_t outside_hash(const vector<int>& board, int r0, int c0, int size) const {
        Hash128 h;
        for (int pos = 0; pos < static_cast<int>(board.size()); ++pos) {
            if (!in_window(pos, r0, c0, size)) {
                h = xor_hash(h, cell_hash(pos, board[pos]));
            }
        }
        return h.lo ^ splitmix64(h.hi);
    }

    uint64_t job_signature(const Candidate& cand,
                           uint64_t endpoint_hash,
                           uint64_t start_patch_hash,
                           int start_blank) const {
        uint64_t key = candidate_key(cand);
        key ^= splitmix64(endpoint_hash + 0x632be59bd9b4e019ULL);
        key ^= splitmix64(start_patch_hash + 0x94d049bb133111ebULL);
        key ^= static_cast<uint64_t>(static_cast<uint32_t>(start_blank)) << 33;
        key ^= splitmix64(generation_);
        return key;
    }

    struct ParentStep {
        string parent;
        char move = 0;
        int depth = 0;
    };

    struct QueueItem {
        string key;
        int depth = 0;
    };

    struct SearchEntry {
        double key = 0;
        int g = 0;
        uint64_t order = 0;
        string board_key;
        int blank = -1;
        char last_move = 0;
    };

    struct SearchEntryGreater {
        bool operator()(const SearchEntry& a, const SearchEntry& b) const {
            if (a.key != b.key) return a.key > b.key;
            if (a.g != b.g) return a.g > b.g;
            return a.order > b.order;
        }
    };

    struct SearchParent {
        string parent;
        char move = 0;
        int g = 0;
    };

    bool time_exhausted(const Timer& timer) const {
        return opt_.time_limit_ms >= 0 && timer.elapsed_ms() >= opt_.time_limit_ms;
    }

    void clear_batch_stats() {
        total_batches_ = 0;
        total_5x5_batches_ = 0;
        total_jobs_ = 0;
    }

    void clear_macro_stats() {
        macro_attempts_ = 0;
        macro_commits_ = 0;
        macro_transport_attempts_ = 0;
        macro_transport_commits_ = 0;
        macro_strip_attempts_ = 0;
        macro_strip_commits_ = 0;
        macro_sweep_attempts_ = 0;
        macro_sweep_commits_ = 0;
        macro_stall_switches_ = 0;
    }

    void clear_anytime_stats() {
        anytime_configs_attempted_ = 0;
        anytime_valid_solutions_ = 0;
        anytime_elite_updates_ = 0;
        anytime_repair_jobs_ = 0;
        anytime_repair_commits_ = 0;
        anytime_monotone_attempts_ = 0;
        anytime_monotone_valid_ = 0;
        anytime_repair_valid_ = 0;
        anytime_checkpoint_jobs_ = 0;
        anytime_cancelled_opposites_ = 0;
        best_anytime_config_id_ = "none";
    }

    void clear_large_stats() {
        large_attempts_ = 0;
        large_commits_ = 0;
        large_sweep_attempts_ = 0;
        large_sweep_commits_ = 0;
        large_construct_attempts_ = 0;
        large_construct_commits_ = 0;
        large_construct_configs_ = 0;
        clear_large_extra_stats();
    }

    void clear_large_extra_stats() {
        large_center_loop_candidates_ = 0;
        large_center_loop_commits_ = 0;
        large_center_loop_best_delta_ = 0;
        large_beam_states_ = 0;
        large_band_commits_ = 0;
        large_partial_states_ = 0;
        large_suffix_jobs_ = 0;
        large_suffix_valid_ = 0;
        large_band_v2_commits_ = 0;
        large_patch_cleanup_ms_ = 0;
        large_patch_cleanup_commits_ = 0;
        large_best_band_config_ = "none";
        large_best_partial_band_ = "none";
        large_best_suffix_config_ = "none";
    }

    void clear_checkpoint_tail_stats() {
        generation_ = 0;
        clear_batch_stats();
        clear_macro_stats();
        clear_anytime_stats();
        clear_large_stats();
    }

    void clear_run_stats() {
        accepted_improvements_ = 0;
        cleanup_time_ms_ = 0;
        cleanup_commits_ = 0;
        clear_checkpoint_tail_stats();
    }

    bool goal_key(const string& key) const {
        for (int pos : p_.inner_positions) {
            int value = unpack_cell(static_cast<uint16_t>(
                static_cast<uint8_t>(key[pos * 2]) |
                (static_cast<uint16_t>(static_cast<uint8_t>(key[pos * 2 + 1])) << 8)));
            if (value != p_.target_at[pos]) {
                return false;
            }
        }
        return true;
    }

    int small_search_heuristic(const PackedBoard& board, int blank) const {
        int result = 0;
        unordered_map<int, vector<int>> sources;
        sources.reserve(board.size());
        for (int i = 0; i < static_cast<int>(board.size()); ++i) {
            sources[unpack_cell(board[i])].push_back(i);
        }
        unordered_map<int, vector<int>> targets;
        targets.reserve(p_.inner_positions.size());
        for (int pos : p_.inner_positions) {
            targets[p_.target_at[pos]].push_back(pos);
        }
        for (const auto& [value, tpos] : targets) {
            auto it = sources.find(value);
            if (it == sources.end() || it->second.size() < tpos.size()) {
                return kInf / 4;
            }
            vector<unsigned char> used(it->second.size(), 0);
            for (int target : tpos) {
                int best = kInf;
                int best_idx = -1;
                for (int i = 0; i < static_cast<int>(it->second.size()); ++i) {
                    if (used[i]) {
                        continue;
                    }
                    int d = p_.manhattan(it->second[i], target);
                    if (d < best) {
                        best = d;
                        best_idx = i;
                    }
                }
                if (best_idx >= 0) {
                    used[best_idx] = 1;
                    result += best;
                }
            }
        }
        int blank_term = kInf;
        for (int pos : p_.inner_positions) {
            if (p_.target_at[pos] != unpack_cell(board[pos])) {
                blank_term = min(blank_term, p_.manhattan(blank, pos));
            }
        }
        if (blank_term < kInf) {
            result += blank_term;
        }
        return result;
    }

    optional<string> weighted_small_board_search(double weight,
                                                 int incumbent_length,
                                                 const Timer& timer,
                                                 size_t node_cap) const {
        PackedBoard start = pack_board(p_.initial);
        string start_key = make_key(start);
        priority_queue<SearchEntry, vector<SearchEntry>, SearchEntryGreater> open;
        unordered_map<string, SearchParent> best;
        best.reserve(min<size_t>(node_cap, 1'000'000));
        uint64_t order = 0;
        int start_h = small_search_heuristic(start, p_.initial_blank);
        open.push({weight * static_cast<double>(start_h), 0, order++, start_key, p_.initial_blank, 0});
        best.emplace(start_key, SearchParent{"", 0, 0});
        const char moves_arr[4] = {'U', 'D', 'L', 'R'};

        while (!open.empty() && best.size() < node_cap && !time_exhausted(timer)) {
            SearchEntry cur = open.top();
            open.pop();
            auto bit = best.find(cur.board_key);
            if (bit == best.end() || bit->second.g != cur.g) {
                continue;
            }
            if (goal_key(cur.board_key)) {
                string path;
                string key = cur.board_key;
                while (true) {
                    auto it = best.find(key);
                    if (it == best.end() || it->second.parent.empty()) {
                        break;
                    }
                    path.push_back(it->second.move);
                    key = it->second.parent;
                }
                reverse(path.begin(), path.end());
                return path;
            }

            PackedBoard cells = key_to_cells(cur.board_key);
            for (char cmd : moves_arr) {
                if (cur.last_move != 0 && cmd == inverse_command(cur.last_move)) {
                    continue;
                }
                int next_blank = -1;
                if (!valid_command_for_blank(p_, cur.blank, cmd, next_blank)) {
                    continue;
                }
                PackedBoard next = cells;
                swap(next[cur.blank], next[next_blank]);
                int ng = cur.g + 1;
                if (ng >= incumbent_length) {
                    continue;
                }
                string next_key = make_key(next);
                auto found = best.find(next_key);
                if (found != best.end() && found->second.g <= ng) {
                    continue;
                }
                int h = small_search_heuristic(next, next_blank);
                best[next_key] = SearchParent{cur.board_key, cmd, ng};
                open.push({static_cast<double>(ng) + weight * static_cast<double>(h),
                           ng, order++, std::move(next_key), next_blank, cmd});
            }
        }
        return nullopt;
    }

    void try_small_board_search(const Timer& timer) {
        if (p_.n > 5 || best_valid_solution_.empty() || time_exhausted(timer)) {
            return;
        }
        int incumbent = static_cast<int>(best_valid_solution_.size());
        vector<double> weights = {5.0, 3.0, 2.0};
        for (double weight : weights) {
            if (time_exhausted(timer)) {
                break;
            }
            size_t cap = weight <= 2.0 ? 1'000'000 : 250'000;
            optional<string> candidate = weighted_small_board_search(weight, incumbent, timer, cap);
            if (candidate && static_cast<int>(candidate->size()) < incumbent &&
                validate_solution(p_, *candidate + "S")) {
                best_path_ = *candidate;
                best_valid_solution_ = *candidate;
                best_source_ = "small_search";
                incumbent = static_cast<int>(candidate->size());
                ++accepted_improvements_;
                ++generation_;
                rejected_candidates_.clear();
                rebuild_trajectory();
                publish("small_search");
                save_checkpoint(opt_.checkpoint_path);
                cerr << "[Small-board search improvement: best=" << incumbent
                     << " weight=" << weight << "]\n";
            }
        }
    }

    bool try_anytime_constructive(const Timer& timer) {
        if (!opt_.anytime_constructive || opt_.anytime_engine == "off" ||
            p_.n < 6 || best_valid_solution_.empty() || time_exhausted(timer)) {
            return false;
        }
        int remaining = opt_.time_limit_ms < 0 ? 30'000 : max(0, opt_.time_limit_ms - static_cast<int>(timer.elapsed_ms()));
        int budget = 0;
        if (p_.n >= 30) {
            int reserve = opt_.no_large_patch_cleanup ? 500 : max(500, opt_.large_patch_cleanup_ms + 500);
            budget = opt_.time_limit_ms < 0
                         ? opt_.large_constructor_min_ms
                         : min(max(opt_.large_constructor_min_ms,
                                   opt_.time_limit_ms * opt_.large_constructor_ratio / 100),
                               max(500, remaining - reserve));
        } else {
            budget = opt_.time_limit_ms < 0
                         ? 30'000
                         : min(max(500, opt_.time_limit_ms * opt_.anytime_ratio / 100), max(500, remaining - 500));
        }
        int deadline = static_cast<int>(timer.elapsed_ms()) + budget;
        AnytimeConstructiveSolver solver(p_, opt_, timer, deadline, best_valid_solution_.size());
        AnytimeConstructiveSolver::Result result = solver.run();
        anytime_configs_attempted_ += result.stats.configs_attempted;
        anytime_valid_solutions_ += result.stats.valid_solutions;
        anytime_elite_updates_ += result.stats.elite_updates;
        anytime_repair_jobs_ += result.stats.repair_jobs_attempted;
        anytime_repair_commits_ += result.stats.repair_commits;
        anytime_monotone_attempts_ += result.stats.monotone_attempts;
        anytime_monotone_valid_ += result.stats.monotone_valid;
        anytime_repair_valid_ += result.stats.repair_valid;
        anytime_checkpoint_jobs_ += result.stats.checkpoint_jobs;
        anytime_cancelled_opposites_ += result.stats.cancelled_opposites;
        if (result.best_path.empty() || result.best_path.size() >= best_valid_solution_.size() ||
            !validate_solution(p_, result.best_path + "S")) {
            return false;
        }
        best_path_ = result.best_path;
        best_valid_solution_ = result.best_path;
        best_source_ = p_.n >= 30 ? "large_constructor" : "anytime_constructive";
        best_anytime_config_id_ = result.stats.best_config_id;
        ++accepted_improvements_;
        ++generation_;
        rejected_candidates_.clear();
        rebuild_trajectory();
        publish(p_.n >= 30 ? "large_constructor" : "anytime_constructive");
        save_checkpoint(opt_.checkpoint_path);
        if (p_.n >= 30) {
            cerr << "[Large constructor improvement: best=" << best_valid_solution_.size()
                 << " config=" << result.stats.best_config_id << "]\n";
        } else {
            cerr << "[Anytime constructive improvement: best=" << best_valid_solution_.size()
                 << " config=" << result.stats.best_config_id << "]\n";
        }
        return true;
    }

    string border_bridge_path(int start_blank, int end_blank, bool horizontal_first) const {
        auto direct = [&](int from, int to, bool rows_first) {
            string path;
            int cur = from;
            auto [er, ec] = p_.rc(to);
            auto move_row = [&]() {
                while (cur / p_.n < er) {
                    int nxt = cur + p_.n;
                    path.push_back(command_for_blank_step(p_, cur, nxt));
                    cur = nxt;
                }
                while (cur / p_.n > er) {
                    int nxt = cur - p_.n;
                    path.push_back(command_for_blank_step(p_, cur, nxt));
                    cur = nxt;
                }
            };
            auto move_col = [&]() {
                while (cur % p_.n < ec) {
                    int nxt = cur + 1;
                    path.push_back(command_for_blank_step(p_, cur, nxt));
                    cur = nxt;
                }
                while (cur % p_.n > ec) {
                    int nxt = cur - 1;
                    path.push_back(command_for_blank_step(p_, cur, nxt));
                    cur = nxt;
                }
            };
            if (rows_first) {
                move_row();
                move_col();
            } else {
                move_col();
                move_row();
            }
            return path;
        };
        vector<int> border_targets = {
            p_.index(0, start_blank % p_.n),
            p_.index(p_.n - 1, start_blank % p_.n),
            p_.index(start_blank / p_.n, 0),
            p_.index(start_blank / p_.n, p_.n - 1),
        };
        string best = direct(start_blank, end_blank, !horizontal_first);
        for (int entry : border_targets) {
            string a = direct(start_blank, entry, !horizontal_first);
            string b = direct(entry, end_blank, horizontal_first);
            if (a.size() + b.size() < best.size()) {
                best = a + b;
            }
        }
        return best;
    }

    bool target_cells_unchanged(int start, int end, int& start_blank, int& end_blank) const {
        vector<int> start_board;
        vector<int> end_board;
        state_at(static_cast<size_t>(start), start_board, start_blank);
        state_at(static_cast<size_t>(end), end_board, end_blank);
        for (int pos : p_.inner_positions) {
            if (start_board[pos] != end_board[pos]) {
                return false;
            }
        }
        return true;
    }

    bool commit_large_candidate(const string& path, const string& label) {
        if (path.size() >= best_valid_solution_.size() || !validate_solution(p_, path + "S")) {
            return false;
        }
        best_path_ = path;
        best_valid_solution_ = path;
        best_source_ = label;
        ++accepted_improvements_;
        ++generation_;
        ++large_commits_;
        rejected_candidates_.clear();
        rebuild_trajectory();
        publish(label);
        save_checkpoint(opt_.checkpoint_path);
        cerr << "[Large-board improvement: best=" << best_valid_solution_.size()
             << " source=" << label << "]\n";
        return true;
    }

    struct CenterLoopKey {
        Hash128 center_hash;
        int blank = -1;

        bool operator==(const CenterLoopKey& other) const {
            return blank == other.blank && center_hash == other.center_hash;
        }
    };

    struct CenterLoopKeyHasher {
        size_t operator()(const CenterLoopKey& key) const {
            uint64_t x = key.center_hash.lo ^
                         (key.center_hash.hi + 0x9e3779b97f4a7c15ULL +
                          (static_cast<uint64_t>(key.blank) << 17));
            if constexpr (sizeof(size_t) >= 8) {
                return static_cast<size_t>(x);
            } else {
                return static_cast<size_t>((x >> 32) ^ x);
            }
        }
    };

    struct CenterHashRecord {
        int index = 0;
        int blank = -1;
    };

    Hash128 compute_center_hash(const vector<int>& board) const {
        Hash128 h;
        for (int pos : p_.inner_positions) {
            h = xor_hash(h, cell_hash(pos, board[pos]));
        }
        return h;
    }

    void update_center_hash_after_move(Hash128& center_hash,
                                       int blank_before,
                                       int tile_pos,
                                       int tile_value) const {
        if (p_.is_inner(blank_before)) {
            center_hash = xor_hash(center_hash, cell_hash(blank_before, -1));
            center_hash = xor_hash(center_hash, cell_hash(blank_before, tile_value));
        }
        if (p_.is_inner(tile_pos)) {
            center_hash = xor_hash(center_hash, cell_hash(tile_pos, tile_value));
            center_hash = xor_hash(center_hash, cell_hash(tile_pos, -1));
        }
    }

    bool is_large_sample_index(int idx,
                               const vector<int>& run_boundaries,
                               size_t& boundary_cursor) const {
        while (boundary_cursor < run_boundaries.size() && run_boundaries[boundary_cursor] < idx) {
            ++boundary_cursor;
        }
        return idx == 0 ||
               idx == static_cast<int>(best_valid_solution_.size()) ||
               (idx % 64 == 0) ||
               (boundary_cursor < run_boundaries.size() && run_boundaries[boundary_cursor] == idx);
    }

    bool try_large_center_hash_compression(const Timer& timer, int deadline) {
        if (best_valid_solution_.empty()) {
            return false;
        }
        vector<int> run_boundaries;
        run_boundaries.reserve(best_valid_solution_.size() / 4 + 2);
        run_boundaries.push_back(0);
        for (int i = 1; i < static_cast<int>(best_valid_solution_.size()); ++i) {
            if (best_valid_solution_[i] != best_valid_solution_[i - 1]) {
                run_boundaries.push_back(i);
            }
        }
        run_boundaries.push_back(static_cast<int>(best_valid_solution_.size()));

        vector<int> board = p_.initial;
        int blank = p_.initial_blank;
        Hash128 center_hash = compute_center_hash(board);
        unordered_map<CenterLoopKey, int, CenterLoopKeyHasher> first_exact;
        unordered_map<Hash128, CenterHashRecord, Hash128Hasher> first_center;
        first_exact.reserve(min<size_t>(best_valid_solution_.size() / 8 + 16, 300000));
        first_center.reserve(min<size_t>(best_valid_solution_.size() / 8 + 16, 300000));

        struct Candidate {
            int start = 0;
            int end = 0;
            int start_blank = -1;
            int end_blank = -1;
            bool exact_blank = false;
            int saving_hint = 0;
        };
        vector<Candidate> candidates;
        auto sample = [&](int idx) {
            CenterLoopKey key{center_hash, blank};
            auto exact_it = first_exact.find(key);
            if (exact_it != first_exact.end()) {
                int len = idx - exact_it->second;
                if (len >= p_.n) {
                    candidates.push_back({exact_it->second, idx, blank, blank, true, len});
                }
            } else {
                first_exact.emplace(key, idx);
            }

            auto center_it = first_center.find(center_hash);
            if (center_it != first_center.end()) {
                int len = idx - center_it->second.index;
                if (len >= p_.n * 2) {
                    string bridge_a = border_bridge_path(center_it->second.blank, blank, true);
                    string bridge_b = border_bridge_path(center_it->second.blank, blank, false);
                    int bridge_len = static_cast<int>(min(bridge_a.size(), bridge_b.size()));
                    if (bridge_len < len) {
                        candidates.push_back({center_it->second.index, idx,
                                              center_it->second.blank, blank, false, len - bridge_len});
                    }
                }
                if (idx < center_it->second.index || idx - center_it->second.index > p_.n * 4) {
                    center_it->second = {idx, blank};
                }
            } else {
                first_center.emplace(center_hash, CenterHashRecord{idx, blank});
            }
        };

        size_t boundary_cursor = 0;
        sample(0);
        for (int i = 0; i < static_cast<int>(best_valid_solution_.size()); ++i) {
            if ((i & 8191) == 0 && (time_exhausted(timer) || timer.elapsed_ms() >= deadline)) {
                break;
            }
            char cmd = best_valid_solution_[i];
            int next_blank = -1;
            if (!valid_command_for_blank(p_, blank, cmd, next_blank)) {
                return false;
            }
            int tile_value = board[next_blank];
            update_center_hash_after_move(center_hash, blank, next_blank, tile_value);
            if (!apply_move(p_, board, blank, cmd)) {
                return false;
            }
            int idx = i + 1;
            if (is_large_sample_index(idx, run_boundaries, boundary_cursor)) {
                sample(idx);
            }
        }
        large_center_loop_candidates_ += candidates.size();
        if (candidates.empty()) {
            return false;
        }
        sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.saving_hint != b.saving_hint) return a.saving_hint > b.saving_hint;
            return a.start < b.start;
        });

        unordered_set<uint64_t> tried;
        int checks = 0;
        int max_checks = p_.n >= 50 ? 180 : 260;
        for (const Candidate& cand : candidates) {
            if (checks >= max_checks || time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cand.start)) << 32) ^
                           static_cast<uint64_t>(static_cast<uint32_t>(cand.end));
            if (!tried.insert(key).second || cand.end <= cand.start) {
                continue;
            }
            ++checks;
            vector<string> bridges;
            if (cand.exact_blank) {
                bridges.push_back("");
            } else {
                bridges.push_back(border_bridge_path(cand.start_blank, cand.end_blank, true));
                bridges.push_back(border_bridge_path(cand.start_blank, cand.end_blank, false));
            }
            for (const string& bridge : bridges) {
                if (static_cast<int>(bridge.size()) >= cand.end - cand.start) {
                    continue;
                }
                string candidate_path = best_valid_solution_.substr(0, cand.start) +
                                        bridge +
                                        best_valid_solution_.substr(cand.end);
                if (candidate_path.size() >= best_valid_solution_.size()) {
                    continue;
                }
                uint64_t delta = static_cast<uint64_t>(best_valid_solution_.size() - candidate_path.size());
                if (commit_large_candidate(candidate_path, "large_center_loop")) {
                    ++large_center_loop_commits_;
                    large_center_loop_best_delta_ = max<uint64_t>(large_center_loop_best_delta_, delta);
                    return true;
                }
            }
        }
        return false;
    }

    bool try_large_sweep_compression(const Timer& timer, int deadline) {
        if (best_valid_solution_.empty() || moves_.empty()) {
            return false;
        }
        ++large_sweep_attempts_;
        struct Run {
            int start = 0;
            int end = 0;
            char cmd = 0;
        };
        vector<Run> runs;
        for (int i = 0; i < static_cast<int>(best_valid_solution_.size());) {
            int j = i + 1;
            while (j < static_cast<int>(best_valid_solution_.size()) && best_valid_solution_[j] == best_valid_solution_[i]) {
                ++j;
            }
            runs.push_back({i, j, best_valid_solution_[i]});
            i = j;
        }

        struct RepeatCandidate {
            int start = 0;
            int mid = 0;
            int end = 0;
            int saving = 0;
        };
        vector<RepeatCandidate> repeats;
        for (int i = 0; i < static_cast<int>(runs.size()); ++i) {
            for (int run_count = 2; run_count <= 8 && i + run_count * 2 <= static_cast<int>(runs.size()); ++run_count) {
                int a = runs[i].start;
                int b = runs[i + run_count].start;
                int c = runs[i + run_count * 2].start;
                int len = b - a;
                if (len < p_.n || c - b != len) {
                    continue;
                }
                if (best_valid_solution_.compare(a, len, best_valid_solution_, b, len) == 0) {
                    repeats.push_back({a, b, c, len});
                }
            }
        }
        sort(repeats.begin(), repeats.end(), [](const RepeatCandidate& a, const RepeatCandidate& b) {
            if (a.saving != b.saving) return a.saving > b.saving;
            return a.start < b.start;
        });
        int repeat_checks = 0;
        for (const RepeatCandidate& cand : repeats) {
            if (repeat_checks++ >= 80 || time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            string candidate_path = best_valid_solution_.substr(0, cand.start) +
                                    best_valid_solution_.substr(cand.mid);
            if (commit_large_candidate(candidate_path, "large_repeat_sweep")) {
                ++large_sweep_commits_;
                return true;
            }
        }

        struct SweepCandidate {
            int start = 0;
            int end = 0;
            int saving_hint = 0;
        };
        vector<SweepCandidate> candidates;
        for (int i = 0; i < static_cast<int>(runs.size()); ++i) {
            int len = 0;
            int long_runs = 0;
            for (int j = i; j < min<int>(runs.size(), i + 8); ++j) {
                len += runs[j].end - runs[j].start;
                if (runs[j].end - runs[j].start >= p_.n / 2) {
                    ++long_runs;
                }
                if (j > i && len >= p_.n * 2 && long_runs >= 2) {
                    candidates.push_back({runs[i].start, runs[j].end, len});
                }
            }
        }
        sort(candidates.begin(), candidates.end(), [](const SweepCandidate& a, const SweepCandidate& b) {
            if (a.saving_hint != b.saving_hint) return a.saving_hint > b.saving_hint;
            return a.start < b.start;
        });

        unordered_set<uint64_t> seen;
        int checked = 0;
        int max_checks = p_.n >= 50 ? 160 : 260;
        for (const SweepCandidate& cand : candidates) {
            if (checked >= max_checks || time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cand.start)) << 32) ^
                           static_cast<uint64_t>(static_cast<uint32_t>(cand.end));
            if (!seen.insert(key).second) {
                continue;
            }
            ++checked;
            int start_blank = -1;
            int end_blank = -1;
            bool target_neutral = target_cells_unchanged(cand.start, cand.end, start_blank, end_blank);
            vector<string> bridges = {
                border_bridge_path(start_blank, end_blank, true),
                border_bridge_path(start_blank, end_blank, false),
            };
            if (target_neutral && start_blank == end_blank) {
                bridges.push_back("");
            }
            for (const string& bridge : bridges) {
                if (static_cast<int>(bridge.size()) >= cand.end - cand.start) {
                    continue;
                }
                if (!target_neutral && checked > max_checks / 2) {
                    continue;
                }
                string candidate_path = best_valid_solution_.substr(0, cand.start) +
                                        bridge +
                                        best_valid_solution_.substr(cand.end);
                if (commit_large_candidate(candidate_path, "large_sweep")) {
                    ++large_sweep_commits_;
                    return true;
                }
            }
        }
        return false;
    }

    bool try_large_corridor_constructor(const Timer& timer, int deadline) {
        ++large_construct_attempts_;
        struct BeamState {
            vector<int> board;
            int blank = -1;
            vector<unsigned char> locked;
            string moves;
            int solved_bands = 0;
            int score = 0;
            string config;
        };

        vector<int> parent(p_.n * p_.n, -1);
        vector<int> seen(p_.n * p_.n, 0);
        int stamp = 0;
        auto bfs_path_large = [&](const BeamState& state, int start, int goal, int extra_block) {
            vector<int> empty;
            if (start == goal) {
                return vector<int>{start};
            }
            ++stamp;
            if (stamp == numeric_limits<int>::max()) {
                fill(seen.begin(), seen.end(), 0);
                stamp = 1;
            }
            deque<int> q;
            q.push_back(start);
            seen[start] = stamp;
            parent[start] = -1;
            const int dr[4] = {1, -1, 0, 0};
            const int dc[4] = {0, 0, 1, -1};
            while (!q.empty() && !time_exhausted(timer) && timer.elapsed_ms() < deadline) {
                int cur = q.front();
                q.pop_front();
                auto [r, c] = p_.rc(cur);
                for (int k = 0; k < 4; ++k) {
                    int nr = r + dr[k];
                    int nc = c + dc[k];
                    if (!p_.in_bounds(nr, nc)) {
                        continue;
                    }
                    int nxt = p_.index(nr, nc);
                    if (seen[nxt] == stamp) {
                        continue;
                    }
                    if (nxt != goal && (state.locked[nxt] || nxt == extra_block)) {
                        continue;
                    }
                    seen[nxt] = stamp;
                    parent[nxt] = cur;
                    if (nxt == goal) {
                        vector<int> path;
                        for (int x = goal; x != -1; x = parent[x]) {
                            path.push_back(x);
                        }
                        reverse(path.begin(), path.end());
                        return path;
                    }
                    q.push_back(nxt);
                }
            }
            return empty;
        };

        auto move_blank_to_large = [&](BeamState& state, int dest, int controlled_tile) {
            vector<int> path = bfs_path_large(state, state.blank, dest, controlled_tile);
            if (path.empty()) {
                return false;
            }
            for (size_t i = 1; i < path.size(); ++i) {
                char cmd = command_for_blank_step(p_, state.blank, path[i]);
                if (!apply_move(p_, state.board, state.blank, cmd)) {
                    return false;
                }
                state.moves.push_back(cmd);
                if (state.moves.size() >= best_valid_solution_.size()) {
                    return false;
                }
            }
            return true;
        };

        auto transport_tile_large = [&](BeamState& state, int source, int target) {
            if (source == target) {
                return true;
            }
            int tile = source;
            vector<int> tile_path = bfs_path_large(state, tile, target, -1);
            if (tile_path.empty()) {
                return false;
            }
            for (size_t i = 1; i < tile_path.size(); ++i) {
                int next_tile_pos = tile_path[i];
                if (!move_blank_to_large(state, next_tile_pos, tile)) {
                    return false;
                }
                char cmd = command_for_tile_step(p_, tile, next_tile_pos);
                if (!apply_move(p_, state.board, state.blank, cmd)) {
                    return false;
                }
                state.moves.push_back(cmd);
                tile = next_tile_pos;
                if (state.moves.size() >= best_valid_solution_.size()) {
                    return false;
                }
            }
            return tile == target;
        };

        auto make_bands = [&](bool columns, bool reverse_order) {
            vector<vector<int>> bands;
            int band_size = max(2, opt_.large_band_size);
            if (!columns) {
                for (int r0 = 1; r0 + 1 < p_.n; r0 += band_size) {
                    int r1 = min(p_.n - 2, r0 + band_size - 1);
                    vector<int> band;
                    for (int r = r0; r <= r1; ++r) {
                        for (int c = 1; c + 1 < p_.n; ++c) {
                            band.push_back(p_.index(r, c));
                        }
                    }
                    bands.push_back(std::move(band));
                }
            } else {
                for (int c0 = 1; c0 + 1 < p_.n; c0 += band_size) {
                    int c1 = min(p_.n - 2, c0 + band_size - 1);
                    vector<int> band;
                    for (int c = c0; c <= c1; ++c) {
                        for (int r = 1; r + 1 < p_.n; ++r) {
                            band.push_back(p_.index(r, c));
                        }
                    }
                    bands.push_back(std::move(band));
                }
            }
            if (reverse_order) {
                reverse(bands.begin(), bands.end());
            }
            return bands;
        };

        auto source_cost = [&](const BeamState& state, int source, int target, int variant) {
            int cost = p_.manhattan(source, target) * 8 + p_.manhattan(state.blank, source);
            auto [sr, sc] = p_.rc(source);
            bool border = sr == 0 || sc == 0 || sr + 1 == p_.n || sc + 1 == p_.n;
            if (border) {
                cost -= 20;
            }
            if (p_.is_inner(source) && p_.target_at[source] == state.board[source] && source != target) {
                cost += variant >= 2 ? 80 : 35;
            }
            if (source == target) {
                cost -= 500;
            }
            return cost;
        };

        auto solve_band = [&](BeamState state, const vector<int>& band, int variant) -> optional<BeamState> {
            vector<int> order = band;
            if (variant & 1) {
                reverse(order.begin(), order.end());
            }
            for (int target : order) {
                if (time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                    return nullopt;
                }
                int want = p_.target_at[target];
                if (want == -1) {
                    continue;
                }
                if (state.board[target] == want) {
                    continue;
                }
                int best_source = -1;
                int best_cost = kInf;
                for (int pos = 0; pos < static_cast<int>(state.board.size()); ++pos) {
                    if (state.locked[pos] || state.board[pos] != want) {
                        continue;
                    }
                    int cost = source_cost(state, pos, target, variant);
                    if (cost < best_cost) {
                        best_cost = cost;
                        best_source = pos;
                    }
                }
                if (best_source < 0 || !transport_tile_large(state, best_source, target) ||
                    state.board[target] != want) {
                    return nullopt;
                }
            }
            for (int target : band) {
                int want = p_.target_at[target];
                if (want != -1 && state.board[target] != want) {
                    return nullopt;
                }
            }
            for (int target : band) {
                if (p_.target_at[target] != -1) {
                    state.locked[target] = 1;
                }
            }
            ++state.solved_bands;
            state.score = static_cast<int>(state.moves.size());
            return state;
        };

        vector<pair<string, vector<vector<int>>>> configs;
        configs.push_back({"row", make_bands(false, false)});
        configs.push_back({"row-rev", make_bands(false, true)});
        configs.push_back({"col", make_bands(true, false)});
        configs.push_back({"col-rev", make_bands(true, true)});
        const int beam_width = 4;
        const int variants_per_band = 6;
        for (auto& [config_name, bands] : configs) {
            if (time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            ++large_construct_configs_;
            BeamState start;
            start.board = p_.initial;
            start.blank = p_.initial_blank;
            start.locked.assign(p_.n * p_.n, 0);
            start.config = config_name;
            vector<BeamState> beam = {std::move(start)};
            for (const vector<int>& band : bands) {
                vector<BeamState> next_beam;
                for (const BeamState& state : beam) {
                    for (int variant = 0; variant < variants_per_band; ++variant) {
                        optional<BeamState> solved = solve_band(state, band, variant);
                        ++large_beam_states_;
                        if (solved) {
                            solved->config = config_name + "/v" + to_string(variant);
                            next_beam.push_back(std::move(*solved));
                        }
                    }
                }
                if (next_beam.empty()) {
                    beam.clear();
                    break;
                }
                sort(next_beam.begin(), next_beam.end(), [](const BeamState& a, const BeamState& b) {
                    if (a.score != b.score) return a.score < b.score;
                    return a.moves.size() < b.moves.size();
                });
                if (static_cast<int>(next_beam.size()) > beam_width) {
                    next_beam.resize(beam_width);
                }
                beam = std::move(next_beam);
            }
            for (BeamState& state : beam) {
                int blank_target = -1;
                for (int pos : p_.inner_positions) {
                    if (p_.target_at[pos] == -1) {
                        blank_target = pos;
                        break;
                    }
                }
                if (blank_target >= 0 && !move_blank_to_large(state, blank_target, -1)) {
                    continue;
                }
                if (state.moves.size() < best_valid_solution_.size() &&
                    validate_solution(p_, state.moves + "S")) {
                    large_best_band_config_ = state.config;
                    if (commit_large_candidate(state.moves, "large_band_beam")) {
                        ++large_construct_commits_;
                        ++large_band_commits_;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool try_large_band_v2_constructor(const Timer& timer, int deadline) {
        ++large_construct_attempts_;
        struct BeamState {
            vector<int> board;
            int blank = -1;
            vector<unsigned char> locked;
            string moves;
            int solved_bands = 0;
            int fixed_count = 0;
            int score = 0;
            string label;
        };

        vector<int> parent(p_.n * p_.n, -1);
        vector<int> seen(p_.n * p_.n, 0);
        int stamp = 0;
        auto bfs_path = [&](const BeamState& state, int start, int goal, int extra_block) {
            vector<int> empty;
            if (start == goal) {
                return vector<int>{start};
            }
            ++stamp;
            if (stamp == numeric_limits<int>::max()) {
                fill(seen.begin(), seen.end(), 0);
                stamp = 1;
            }
            deque<int> q;
            q.push_back(start);
            seen[start] = stamp;
            parent[start] = -1;
            const int dr[4] = {1, -1, 0, 0};
            const int dc[4] = {0, 0, 1, -1};
            while (!q.empty() && !time_exhausted(timer) && timer.elapsed_ms() < deadline) {
                int cur = q.front();
                q.pop_front();
                auto [r, c] = p_.rc(cur);
                for (int k = 0; k < 4; ++k) {
                    int nr = r + dr[k];
                    int nc = c + dc[k];
                    if (!p_.in_bounds(nr, nc)) {
                        continue;
                    }
                    int nxt = p_.index(nr, nc);
                    if (seen[nxt] == stamp || nxt == extra_block) {
                        continue;
                    }
                    if (nxt != goal && state.locked[nxt]) {
                        continue;
                    }
                    seen[nxt] = stamp;
                    parent[nxt] = cur;
                    if (nxt == goal) {
                        vector<int> path;
                        for (int x = goal; x != -1; x = parent[x]) {
                            path.push_back(x);
                        }
                        reverse(path.begin(), path.end());
                        return path;
                    }
                    q.push_back(nxt);
                }
            }
            return empty;
        };

        auto append_move = [&](BeamState& state, char cmd) {
            if (!apply_move(p_, state.board, state.blank, cmd)) {
                return false;
            }
            if (!state.moves.empty() && inverse_command(cmd) == state.moves.back()) {
                state.moves.pop_back();
            } else {
                state.moves.push_back(cmd);
            }
            return state.moves.size() < best_valid_solution_.size();
        };

        auto move_blank_to = [&](BeamState& state, int dest, int controlled_tile) {
            vector<int> path = bfs_path(state, state.blank, dest, controlled_tile);
            if (path.empty()) {
                return false;
            }
            for (size_t i = 1; i < path.size(); ++i) {
                if (!append_move(state, command_for_blank_step(p_, state.blank, path[i]))) {
                    return false;
                }
            }
            return true;
        };

        auto transport_tile = [&](BeamState& state, int source, int target) {
            if (source == target) {
                return true;
            }
            int tile = source;
            vector<int> path = bfs_path(state, source, target, -1);
            if (path.empty()) {
                return false;
            }
            for (size_t i = 1; i < path.size(); ++i) {
                int next_tile = path[i];
                if (!move_blank_to(state, next_tile, tile)) {
                    return false;
                }
                if (!append_move(state, command_for_tile_step(p_, tile, next_tile))) {
                    return false;
                }
                tile = next_tile;
            }
            return tile == target;
        };

        auto make_bands = [&](bool columns, bool reverse_order, int band_size) {
            vector<vector<int>> bands;
            if (!columns) {
                for (int r0 = 1; r0 <= p_.n - 2; r0 += band_size) {
                    int r1 = min(p_.n - 2, r0 + band_size - 1);
                    vector<int> band;
                    for (int r = r0; r <= r1; ++r) {
                        if ((r - r0) % 2 == 0) {
                            for (int c = 1; c <= p_.n - 2; ++c) band.push_back(p_.index(r, c));
                        } else {
                            for (int c = p_.n - 2; c >= 1; --c) band.push_back(p_.index(r, c));
                        }
                    }
                    bands.push_back(std::move(band));
                }
            } else {
                for (int c0 = 1; c0 <= p_.n - 2; c0 += band_size) {
                    int c1 = min(p_.n - 2, c0 + band_size - 1);
                    vector<int> band;
                    for (int c = c0; c <= c1; ++c) {
                        if ((c - c0) % 2 == 0) {
                            for (int r = 1; r <= p_.n - 2; ++r) band.push_back(p_.index(r, c));
                        } else {
                            for (int r = p_.n - 2; r >= 1; --r) band.push_back(p_.index(r, c));
                        }
                    }
                    bands.push_back(std::move(band));
                }
            }
            if (reverse_order) {
                reverse(bands.begin(), bands.end());
            }
            return bands;
        };

        auto source_score = [&](const BeamState& state, int source, int target, bool columns, int variant) {
            int cost = 10 * p_.manhattan(source, target) + p_.manhattan(state.blank, source);
            auto [sr, sc] = p_.rc(source);
            auto [tr, tc] = p_.rc(target);
            bool border = sr == 0 || sc == 0 || sr + 1 == p_.n || sc + 1 == p_.n;
            if (border) {
                cost -= 35;
            }
            if (columns) {
                cost += 2 * abs(sc - tc);
            } else {
                cost += 2 * abs(sr - tr);
            }
            if (p_.is_inner(source) && p_.target_at[source] == state.board[source] && source != target) {
                cost += variant >= 4 ? 140 : 70;
            }
            if (source == target) {
                cost -= 1000;
            }
            return cost;
        };

        auto solve_band = [&](BeamState state,
                              const vector<int>& band,
                              bool columns,
                              int variant) -> optional<BeamState> {
            unordered_map<int, vector<int>> buckets;
            buckets.reserve(state.board.size());
            auto rebuild_buckets = [&]() {
                buckets.clear();
                for (int pos = 0; pos < static_cast<int>(state.board.size()); ++pos) {
                    if (!state.locked[pos]) {
                        buckets[state.board[pos]].push_back(pos);
                    }
                }
            };
            vector<int> order = band;
            if (variant & 1) {
                reverse(order.begin(), order.end());
            }
            rebuild_buckets();
            for (int target : order) {
                if (time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                    return nullopt;
                }
                int want = p_.target_at[target];
                if (want == -1 || state.board[target] == want) {
                    continue;
                }
                auto it = buckets.find(want);
                if (it == buckets.end()) {
                    return nullopt;
                }
                vector<pair<int, int>> scored;
                scored.reserve(it->second.size());
                for (int source : it->second) {
                    if (source < 0 || source >= static_cast<int>(state.board.size()) ||
                        state.locked[source] || state.board[source] != want) {
                        continue;
                    }
                    scored.push_back({source_score(state, source, target, columns, variant), source});
                }
                if (scored.empty()) {
                    return nullopt;
                }
                sort(scored.begin(), scored.end());
                const int cand_limit = min<int>(scored.size(), max(1, opt_.large_band_candidates));
                bool placed = false;
                for (int ci = 0; ci < cand_limit; ++ci) {
                    BeamState trial = state;
                    int source = scored[ci].second;
                    if (transport_tile(trial, source, target) && trial.board[target] == want) {
                        state = std::move(trial);
                        rebuild_buckets();
                        placed = true;
                        break;
                    }
                }
                if (!placed) {
                    return nullopt;
                }
            }
            int fixed_added = 0;
            for (int target : band) {
                int want = p_.target_at[target];
                if (want == -1) {
                    continue;
                }
                if (state.board[target] != want) {
                    return nullopt;
                }
                if (!state.locked[target]) {
                    state.locked[target] = 1;
                    ++fixed_added;
                }
            }
            ++state.solved_bands;
            state.fixed_count += fixed_added;
            int blank_border = min({state.blank / p_.n, state.blank % p_.n,
                                    p_.n - 1 - state.blank / p_.n, p_.n - 1 - state.blank % p_.n});
            state.score = static_cast<int>(state.moves.size()) - state.solved_bands * 500 +
                          blank_border * 4 - state.fixed_count;
            return state;
        };

        vector<AnytimeConstructiveSolver::ExternalCheckpoint> partials;
        const int partial_limit = max(opt_.large_suffix_jobs, opt_.large_beam_width * 4);
        auto remember_partial = [&](const BeamState& state, const string& label) {
            if (state.fixed_count <= 0 || partials.size() >= static_cast<size_t>(partial_limit)) {
                return;
            }
            AnytimeConstructiveSolver::ExternalCheckpoint cp;
            cp.board.reserve(state.board.size());
            for (int value : state.board) {
                cp.board.push_back(pack_cell(value));
            }
            cp.blank = state.blank;
            cp.locked = state.locked;
            cp.moves = state.moves;
            cp.fixed_count = state.fixed_count;
            cp.label = label;
            partials.push_back(std::move(cp));
        };

        vector<tuple<string, bool, bool, int>> configs = {
            {"row", false, false, max(3, min(5, opt_.large_band_size))},
            {"row-rev", false, true, max(3, min(5, opt_.large_band_size))},
            {"col", true, false, max(3, min(5, opt_.large_band_size))},
            {"col-rev", true, true, max(3, min(5, opt_.large_band_size))},
        };
        for (const auto& [name, columns, reverse_order, band_size] : configs) {
            if (time_exhausted(timer) || timer.elapsed_ms() >= deadline ||
                partials.size() >= static_cast<size_t>(partial_limit)) {
                break;
            }
            ++large_construct_configs_;
            vector<vector<int>> bands = make_bands(columns, reverse_order, band_size);
            BeamState start;
            start.board = p_.initial;
            start.blank = p_.initial_blank;
            start.locked.assign(p_.n * p_.n, 0);
            start.label = name;
            vector<BeamState> beam = {std::move(start)};
            for (int bi = 0; bi < static_cast<int>(bands.size()); ++bi) {
                vector<BeamState> next_beam;
                for (const BeamState& state : beam) {
                    for (int variant = 0; variant < 8; ++variant) {
                        optional<BeamState> solved = solve_band(state, bands[bi], columns, variant);
                        ++large_beam_states_;
                        if (solved) {
                            solved->label = name + "/b" + to_string(bi) + "/v" + to_string(variant);
                            next_beam.push_back(std::move(*solved));
                        }
                    }
                }
                if (next_beam.empty()) {
                    break;
                }
                sort(next_beam.begin(), next_beam.end(), [](const BeamState& a, const BeamState& b) {
                    if (a.score != b.score) return a.score < b.score;
                    return a.moves.size() < b.moves.size();
                });
                if (static_cast<int>(next_beam.size()) > opt_.large_beam_width) {
                    next_beam.resize(opt_.large_beam_width);
                }
                for (const BeamState& state : next_beam) {
                    remember_partial(state, state.label);
                }
                beam = std::move(next_beam);
                if (partials.size() >= static_cast<size_t>(partial_limit) ||
                    timer.elapsed_ms() >= deadline) {
                    break;
                }
            }
        }

        auto band_complete = [&](const vector<int>& board, const vector<int>& band) {
            for (int pos : band) {
                int want = p_.target_at[pos];
                if (want != -1 && board[pos] != want) {
                    return false;
                }
            }
            return true;
        };

        auto mine_incumbent_partials = [&]() {
            if (partials.size() >= static_cast<size_t>(partial_limit) || best_valid_solution_.empty()) {
                return;
            }
            vector<pair<string, vector<vector<int>>>> mined_configs;
            int band_size = max(3, min(5, opt_.large_band_size));
            mined_configs.push_back({"inc-row", make_bands(false, false, band_size)});
            mined_configs.push_back({"inc-row-rev", make_bands(false, true, band_size)});
            mined_configs.push_back({"inc-col", make_bands(true, false, band_size)});
            mined_configs.push_back({"inc-col-rev", make_bands(true, true, band_size)});
            vector<int> board = p_.initial;
            int blank = p_.initial_blank;
            const int stride = max(512, static_cast<int>(best_valid_solution_.size() / 384));
            unordered_set<uint64_t> seen_labels;
            auto sample = [&](int move_index) {
                for (auto& [name, bands] : mined_configs) {
                    vector<unsigned char> locked(p_.n * p_.n, 0);
                    int fixed_count = 0;
                    int solved = 0;
                    for (const vector<int>& band : bands) {
                        if (!band_complete(board, band)) {
                            break;
                        }
                        ++solved;
                        for (int pos : band) {
                            if (p_.target_at[pos] != -1 && !locked[pos]) {
                                locked[pos] = 1;
                                ++fixed_count;
                            }
                        }
                    }
                    if (solved <= 0 || fixed_count <= 0) {
                        continue;
                    }
                    uint64_t key = (static_cast<uint64_t>(move_index) << 32) ^
                                   (static_cast<uint64_t>(solved) << 8) ^
                                   static_cast<uint64_t>(name[4]);
                    if (!seen_labels.insert(key).second) {
                        continue;
                    }
                    AnytimeConstructiveSolver::ExternalCheckpoint cp;
                    cp.board.reserve(board.size());
                    for (int value : board) {
                        cp.board.push_back(pack_cell(value));
                    }
                    cp.blank = blank;
                    cp.locked = std::move(locked);
                    cp.moves = best_valid_solution_.substr(0, move_index);
                    cp.fixed_count = fixed_count;
                    cp.label = name + "/m" + to_string(move_index) + "/b" + to_string(solved);
                    partials.push_back(std::move(cp));
                    if (partials.size() >= static_cast<size_t>(partial_limit)) {
                        return;
                    }
                }
            };
            sample(0);
            for (int i = 0; i < static_cast<int>(best_valid_solution_.size()); ++i) {
                if (time_exhausted(timer) || timer.elapsed_ms() >= deadline ||
                    partials.size() >= static_cast<size_t>(partial_limit)) {
                    break;
                }
                if (!apply_move(p_, board, blank, best_valid_solution_[i])) {
                    break;
                }
                int move_index = i + 1;
                if (move_index % stride == 0 || move_index + 1 == static_cast<int>(best_valid_solution_.size())) {
                    sample(move_index);
                }
            }
        };

        mine_incumbent_partials();

        large_partial_states_ += partials.size();
        if (partials.empty() || opt_.large_suffix_jobs <= 0 || time_exhausted(timer) ||
            timer.elapsed_ms() >= deadline) {
            return false;
        }
        AnytimeConstructiveSolver suffix_solver(p_, opt_, timer, deadline, best_valid_solution_.size());
        AnytimeConstructiveSolver::Result suffix =
            suffix_solver.run_from_external_checkpoints(partials, opt_.large_suffix_jobs);
        large_suffix_jobs_ += suffix.stats.repair_jobs_attempted;
        large_suffix_valid_ += suffix.stats.repair_valid;
        if (!suffix.best_path.empty() && suffix.best_path.size() < best_valid_solution_.size() &&
            validate_solution(p_, suffix.best_path + "S")) {
            large_best_suffix_config_ = suffix.stats.best_config_id;
            large_best_partial_band_ = partials.front().label;
            if (commit_large_candidate(suffix.best_path, "large_band_v2")) {
                ++large_construct_commits_;
                ++large_band_v2_commits_;
                return true;
            }
        }
        return false;
    }

    bool try_large_board_resynthesis(const Timer& timer) {
        if (!opt_.large_constructive || p_.n < 30 || best_valid_solution_.empty() || time_exhausted(timer)) {
            return false;
        }
        if (opt_.large_engine == "off") {
            return false;
        }
        ++large_attempts_;
        int remaining = opt_.time_limit_ms < 0 ? 30'000 : max(0, opt_.time_limit_ms - static_cast<int>(timer.elapsed_ms()));
        int budget = opt_.time_limit_ms < 0
                         ? 30'000
                         : min(max(1'000, opt_.time_limit_ms * opt_.large_ratio / 100), max(1'000, remaining - 500));
        if (p_.n >= 30 && opt_.large_engine == "v1") {
            budget = min(budget, 2'500);
        }
        int deadline = static_cast<int>(timer.elapsed_ms()) + budget;
        while (!time_exhausted(timer) && timer.elapsed_ms() < deadline) {
            if (try_large_center_hash_compression(timer, deadline)) {
                return true;
            }
            if (try_large_sweep_compression(timer, deadline)) {
                return true;
            }
            if (opt_.large_engine == "band-v2") {
                if (try_large_band_v2_constructor(timer, deadline)) {
                    return true;
                }
            } else if (try_large_corridor_constructor(timer, min(deadline, static_cast<int>(timer.elapsed_ms()) + 2'000))) {
                return true;
            }
            if (try_large_center_hash_compression(timer, deadline)) {
                return true;
            }
            break;
        }
        return false;
    }

    bool commit_macro_path(const string& path, const string& source, const string& label) {
        if (path.size() >= best_valid_solution_.size()) {
            return false;
        }
        if (!validate_solution(p_, path + "S")) {
            return false;
        }
        best_path_ = path;
        best_valid_solution_ = path;
        best_source_ = source;
        ++accepted_improvements_;
        ++macro_commits_;
        ++generation_;
        rejected_candidates_.clear();
        rebuild_trajectory();
        publish(source);
        save_checkpoint(opt_.checkpoint_path);
        cerr << "[Macro " << label << " improvement: best=" << best_valid_solution_.size() << "]\n";
        return true;
    }

    bool try_transport_macro(const Timer& timer, int deadline) {
        ++macro_transport_attempts_;
        ConstructiveSolver macro(p_, "macro", &timer, deadline);
        string path;
        if (!macro.solve(path)) {
            return false;
        }
        if (commit_macro_path(path, "macro_transport", "transport")) {
            ++macro_transport_commits_;
            return true;
        }
        return false;
    }

    vector<string> strip_variants() const {
        vector<string> variants = {
            "order-nearest", "order-edge", "order-center",
            "strip-row2", "strip-row3", "strip-row4",
            "strip-row2-rev", "strip-row3-rev", "strip-row4-rev",
            "strip-col2", "strip-col3", "strip-col4",
            "strip-col2-rev", "strip-col3-rev", "strip-col4-rev",
            "strip-row3-center", "strip-col3-center"
        };
        return variants;
    }

    bool try_strip_macro(const Timer& timer, int deadline) {
        vector<string> variants = strip_variants();
        bool committed = false;
        size_t best_len = best_valid_solution_.size();
        string best_path;
        string best_variant;
        for (const string& variant : variants) {
            if (time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            ++macro_strip_attempts_;
            ConstructiveSolver strip(p_, variant, &timer, deadline);
            string path;
            if (!strip.solve(path) || path.size() >= best_len || !validate_solution(p_, path + "S")) {
                continue;
            }
            best_len = path.size();
            best_path = std::move(path);
            best_variant = variant;
        }
        if (!best_path.empty()) {
            committed = commit_macro_path(best_path, "macro_strip", "strip/" + best_variant);
            if (committed) {
                ++macro_strip_commits_;
            }
        }
        return committed;
    }

    string simplify_backtracks(const string& path) const {
        string out;
        out.reserve(path.size());
        for (char cmd : path) {
            if (!out.empty() && inverse_command(cmd) == out.back()) {
                out.pop_back();
            } else {
                out.push_back(cmd);
            }
        }
        return out;
    }

    bool try_sweep_macro(const Timer& timer, int deadline) {
        ++macro_sweep_attempts_;
        string simplified = simplify_backtracks(best_valid_solution_);
        if (simplified.size() < best_valid_solution_.size() && validate_solution(p_, simplified + "S")) {
            if (commit_macro_path(simplified, "macro_sweep", "sweep/backtrack")) {
                ++macro_sweep_commits_;
                return true;
            }
        }

        // Exact state-loop removal is cheap and catches any full-board sweep that returns
        // to a previous board state, even when the move text is not a direct inverse pair.
        vector<int> board = p_.initial;
        int blank = p_.initial_blank;
        Hash128 hash = p_.initial_hash;
        unordered_map<Hash128, int, Hash128Hasher> first_seen;
        first_seen.reserve(min<size_t>(best_valid_solution_.size() + 1, 1'000'000));
        first_seen.emplace(hash, 0);
        int best_a = -1;
        int best_b = -1;
        for (int i = 0; i < static_cast<int>(best_valid_solution_.size()); ++i) {
            if (time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            if (!apply_move(p_, board, blank, best_valid_solution_[i], &hash)) {
                return false;
            }
            auto it = first_seen.find(hash);
            if (it != first_seen.end()) {
                if (i + 1 - it->second > max(2, best_b - best_a)) {
                    best_a = it->second;
                    best_b = i + 1;
                }
            } else {
                first_seen.emplace(hash, i + 1);
            }
        }
        if (best_a >= 0 && best_b > best_a) {
            string candidate = best_valid_solution_.substr(0, best_a) + best_valid_solution_.substr(best_b);
            if (candidate.size() < best_valid_solution_.size() && validate_solution(p_, candidate + "S")) {
                if (commit_macro_path(candidate, "macro_sweep", "sweep/state-loop")) {
                    ++macro_sweep_commits_;
                    return true;
                }
            }
        }

        struct Run {
            int start = 0;
            int end = 0;
            char cmd = 0;
        };
        vector<Run> runs;
        for (int i = 0; i < static_cast<int>(best_valid_solution_.size());) {
            int j = i + 1;
            while (j < static_cast<int>(best_valid_solution_.size()) && best_valid_solution_[j] == best_valid_solution_[i]) {
                ++j;
            }
            runs.push_back({i, j, best_valid_solution_[i]});
            i = j;
        }
        auto blank_delta = [](char cmd) {
            if (cmd == 'U') return pair<int, int>{1, 0};
            if (cmd == 'D') return pair<int, int>{-1, 0};
            if (cmd == 'L') return pair<int, int>{0, 1};
            if (cmd == 'R') return pair<int, int>{0, -1};
            return pair<int, int>{0, 0};
        };
        auto blank_bridge = [&](int start_blank, int end_blank) {
            string bridge;
            int cur = start_blank;
            auto [er, ec] = p_.rc(end_blank);
            while (cur / p_.n < er) {
                int nxt = cur + p_.n;
                bridge.push_back(command_for_blank_step(p_, cur, nxt));
                cur = nxt;
            }
            while (cur / p_.n > er) {
                int nxt = cur - p_.n;
                bridge.push_back(command_for_blank_step(p_, cur, nxt));
                cur = nxt;
            }
            while (cur % p_.n < ec) {
                int nxt = cur + 1;
                bridge.push_back(command_for_blank_step(p_, cur, nxt));
                cur = nxt;
            }
            while (cur % p_.n > ec) {
                int nxt = cur - 1;
                bridge.push_back(command_for_blank_step(p_, cur, nxt));
                cur = nxt;
            }
            return bridge;
        };
        struct DeleteCandidate {
            int start = 0;
            int end = 0;
            int len = 0;
            string replacement;
        };

        vector<DeleteCandidate> mined;
        vector<Candidate> high_waste = gather_candidates();
        for (const Candidate& cand : high_waste) {
            int len = cand.end - cand.start;
            if (len < 4 || cand.end > static_cast<int>(moves_.size())) {
                continue;
            }
            int start_blank = cand.start == 0 ? p_.initial_blank : moves_[cand.start - 1].blank_after;
            int end_blank = moves_[cand.end - 1].blank_after;
            string bridge = blank_bridge(start_blank, end_blank);
            if (static_cast<int>(bridge.size()) < len) {
                mined.push_back({cand.start, cand.end, len, std::move(bridge)});
            }
        }
        sort(mined.begin(), mined.end(), [](const DeleteCandidate& a, const DeleteCandidate& b) {
            int ad = a.len - static_cast<int>(a.replacement.size());
            int bd = b.len - static_cast<int>(b.replacement.size());
            if (ad != bd) return ad > bd;
            return a.start < b.start;
        });
        int mined_cap = p_.n >= 20 ? 32 : 128;
        for (const DeleteCandidate& del : mined) {
            if (--mined_cap < 0 || time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            string candidate = best_valid_solution_.substr(0, del.start) +
                               del.replacement +
                               best_valid_solution_.substr(del.end);
            if (candidate.size() < best_valid_solution_.size() && validate_solution(p_, candidate + "S")) {
                if (commit_macro_path(candidate, "macro_sweep", "sweep/candidate-bridge")) {
                    ++macro_sweep_commits_;
                    return true;
                }
            }
        }
        vector<DeleteCandidate> deletions;
        for (int i = 0; i < static_cast<int>(runs.size()); ++i) {
            int dr = 0;
            int dc = 0;
            for (int j = i; j < min<int>(runs.size(), i + 10); ++j) {
                auto [r, c] = blank_delta(runs[j].cmd);
                int count = runs[j].end - runs[j].start;
                dr += r * count;
                dc += c * count;
                int len = runs[j].end - runs[i].start;
                if (j > i && len >= 8) {
                    int start_blank = runs[i].start == 0 ? p_.initial_blank : moves_[runs[i].start - 1].blank_after;
                    int end_blank = moves_[runs[j].end - 1].blank_after;
                    string bridge = blank_bridge(start_blank, end_blank);
                    if (static_cast<int>(bridge.size()) < len) {
                        deletions.push_back({runs[i].start, runs[j].end, len, std::move(bridge)});
                    } else if (dr == 0 && dc == 0) {
                        deletions.push_back({runs[i].start, runs[j].end, len, ""});
                    }
                }
            }
        }
        sort(deletions.begin(), deletions.end(), [](const DeleteCandidate& a, const DeleteCandidate& b) {
            int ad = a.len - static_cast<int>(a.replacement.size());
            int bd = b.len - static_cast<int>(b.replacement.size());
            if (ad != bd) return ad > bd;
            if (a.len != b.len) return a.len > b.len;
            return a.start < b.start;
        });
        const int validation_cap = p_.n >= 20 ? 48 : 256;
        int tried = 0;
        for (const DeleteCandidate& del : deletions) {
            if (++tried > validation_cap || time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            string candidate = best_valid_solution_.substr(0, del.start) +
                               del.replacement +
                               best_valid_solution_.substr(del.end);
            if (candidate.size() >= best_valid_solution_.size()) {
                continue;
            }
            if (validate_solution(p_, candidate + "S")) {
                if (commit_macro_path(candidate, "macro_sweep", "sweep/bridge")) {
                    ++macro_sweep_commits_;
                    return true;
                }
            }
        }
        return false;
    }

    bool try_macro_resynthesis(const Timer& timer, const string& reason) {
        if (opt_.no_macro_resynthesis || p_.n < 6 || best_valid_solution_.empty() || time_exhausted(timer)) {
            return false;
        }
        (void)reason;
        ++macro_attempts_;
        int remaining = opt_.time_limit_ms < 0 ? 30'000 : max(0, opt_.time_limit_ms - static_cast<int>(timer.elapsed_ms()));
        int budget = opt_.time_limit_ms < 0
                         ? 30'000
                         : min(max(1'000, opt_.time_limit_ms * opt_.macro_time_ratio / 100), max(1'000, remaining - 500));
        int deadline = static_cast<int>(timer.elapsed_ms()) + budget;

        vector<string> strategies;
        if (opt_.macro_strategy == "auto" || opt_.macro_strategy == "all") {
            if (p_.n >= 30) {
                strategies = {"sweep"};
            } else {
                strategies = {"transport", "sweep", "strip"};
            }
        } else {
            strategies = {opt_.macro_strategy};
        }
        for (const string& strategy : strategies) {
            if (time_exhausted(timer) || timer.elapsed_ms() >= deadline) {
                break;
            }
            if (strategy == "transport" && try_transport_macro(timer, deadline)) {
                return true;
            }
            if (strategy == "strip" && try_strip_macro(timer, deadline)) {
                return true;
            }
            if (strategy == "sweep" && try_sweep_macro(timer, deadline)) {
                return true;
            }
        }
        return false;
    }

    static PackedBoard pack_board(const vector<int>& board) {
        PackedBoard packed;
        packed.reserve(board.size());
        for (int value : board) {
            packed.push_back(pack_cell(value));
        }
        return packed;
    }

    static vector<int> unpack_board(const PackedBoard& board) {
        vector<int> out;
        out.reserve(board.size());
        for (uint16_t cell : board) {
            out.push_back(unpack_cell(cell));
        }
        return out;
    }

    void rebuild_trajectory() {
        snapshots_.clear();
        moves_.clear();
        vector<int> board = p_.initial;
        int blank = p_.initial_blank;
        snapshots_.push_back({0, pack_board(board), blank});
        moves_.reserve(best_path_.size());
        for (size_t i = 0; i < best_path_.size(); ++i) {
            char cmd = best_path_[i];
            int next_blank = -1;
            if (!valid_command_for_blank(p_, blank, cmd, next_blank)) {
                throw runtime_error("invalid move while rebuilding patch trajectory");
            }
            MoveInfo info;
            info.move = cmd;
            info.blank_before = blank;
            info.blank_after = next_blank;
            info.touched_a = blank;
            info.touched_b = next_blank;
            info.moved_tile = board[next_blank];
            info.local_hash = splitmix64(static_cast<uint64_t>(blank) ^
                                         (static_cast<uint64_t>(next_blank) << 21) ^
                                         (static_cast<uint64_t>(info.moved_tile + 4099) << 42));
            if (!apply_move(p_, board, blank, cmd)) {
                throw runtime_error("invalid move while applying patch trajectory");
            }
            moves_.push_back(info);
            if ((i + 1) % static_cast<size_t>(snapshot_stride_) == 0) {
                snapshots_.push_back({i + 1, pack_board(board), blank});
            }
        }
        if (!p_.is_goal(board)) {
            throw runtime_error("patch trajectory does not end at goal");
        }
    }

    void state_at(size_t move_index, vector<int>& board, int& blank) const {
        if (move_index > best_path_.size()) {
            throw runtime_error("trajectory index out of range");
        }
        size_t snap_id = min(move_index / static_cast<size_t>(snapshot_stride_), snapshots_.size() - 1);
        while (snap_id + 1 < snapshots_.size() && snapshots_[snap_id + 1].move_index <= move_index) {
            ++snap_id;
        }
        while (snap_id > 0 && snapshots_[snap_id].move_index > move_index) {
            --snap_id;
        }
        board = unpack_board(snapshots_[snap_id].board);
        blank = snapshots_[snap_id].blank;
        for (size_t i = snapshots_[snap_id].move_index; i < move_index; ++i) {
            if (!apply_move(p_, board, blank, best_path_[i])) {
                throw runtime_error("failed to replay from patch snapshot");
            }
        }
    }

    bool in_window(int pos, int r0, int c0, int size) const {
        auto [r, c] = p_.rc(pos);
        return r >= r0 && r < r0 + size && c >= c0 && c < c0 + size;
    }

    int global_index(int local, int r0, int c0, int size) const {
        return p_.index(r0 + local / size, c0 + local % size);
    }

    vector<int> segment_touched(int start, int end) const {
        unordered_set<int> seen;
        seen.reserve(static_cast<size_t>(end - start) * 2 + 4);
        for (int i = start; i < end; ++i) {
            seen.insert(moves_[i].touched_a);
            seen.insert(moves_[i].touched_b);
        }
        vector<int> cells(seen.begin(), seen.end());
        return cells;
    }

    bool choose_window(const vector<int>& touched, int size, int& r0, int& c0) const {
        if (touched.empty() || p_.n < size) {
            return false;
        }
        int min_r = p_.n;
        int min_c = p_.n;
        int max_r = -1;
        int max_c = -1;
        long long sum_r = 0;
        long long sum_c = 0;
        for (int pos : touched) {
            auto [r, c] = p_.rc(pos);
            min_r = min(min_r, r);
            min_c = min(min_c, c);
            max_r = max(max_r, r);
            max_c = max(max_c, c);
            sum_r += r;
            sum_c += c;
        }
        int center_r = static_cast<int>(sum_r / static_cast<long long>(touched.size()));
        int center_c = static_cast<int>(sum_c / static_cast<long long>(touched.size()));
        if (max_r - min_r + 1 <= size && max_c - min_c + 1 <= size) {
            center_r = (min_r + max_r) / 2;
            center_c = (min_c + max_c) / 2;
        }
        r0 = min(max(0, center_r - size / 2), p_.n - size);
        c0 = min(max(0, center_c - size / 2), p_.n - size);
        int inside = 0;
        for (int pos : touched) {
            if (in_window(pos, r0, c0, size)) {
                ++inside;
            }
        }
        return static_cast<double>(inside) / static_cast<double>(touched.size()) >= 0.70;
    }

    double score_segment(int start, int end) const {
        int backtracks = 0;
        int repeated_blank = 0;
        int repeated_touched = 0;
        int loopiness = 0;
        unordered_set<int> blank_seen;
        unordered_set<int> touched_seen;
        unordered_set<uint64_t> hash_seen;
        blank_seen.reserve(static_cast<size_t>(end - start) + 4);
        touched_seen.reserve(static_cast<size_t>(end - start) * 2 + 4);
        hash_seen.reserve(static_cast<size_t>(end - start) + 4);
        for (int i = start; i < end; ++i) {
            if (i > start && moves_[i].move == inverse_command(moves_[i - 1].move)) {
                ++backtracks;
            }
            if (!blank_seen.insert(moves_[i].blank_after).second) {
                ++repeated_blank;
            }
            if (!touched_seen.insert(moves_[i].touched_a).second) {
                ++repeated_touched;
            }
            if (!touched_seen.insert(moves_[i].touched_b).second) {
                ++repeated_touched;
            }
            if (!hash_seen.insert(moves_[i].local_hash).second) {
                ++loopiness;
            }
        }
        int inefficiency = (end - start) - p_.manhattan(moves_[start].blank_before, moves_[end - 1].blank_after);
        int overlap_penalty = 0;
        for (const auto& [a, b] : recent_patches_) {
            int overlap = max(0, min(end, b) - max(start, a));
            overlap_penalty += overlap * 3;
        }
        return static_cast<double>(end - start) +
               3.0 * backtracks +
               2.0 * repeated_blank +
               repeated_touched +
               max(0, inefficiency) +
               loopiness -
               overlap_penalty;
    }

    vector<Candidate> gather_candidates() const {
        vector<int> widths = {2, 4, 8, 12, 16, 20, 32, 40, 64, 80, 128, 160};
        vector<Candidate> all;
        const int path_len = static_cast<int>(best_path_.size());
        if (path_len <= 1) {
            return all;
        }
        const int top_k = max(1, opt_.patch_top_k);
        const int max_windows_per_width = max(256, top_k * 8);
        auto consider = [&](int start, int end) {
            if (end <= start + 1 || end > path_len) {
                return;
            }
            vector<int> touched = segment_touched(start, end);
            double base_score = score_segment(start, end);
            for (int size : {3, 4, 5}) {
                int r0 = 0;
                int c0 = 0;
                if (!choose_window(touched, size, r0, c0)) {
                    continue;
                }
                Candidate cand{start, end, r0, c0, size, base_score + size};
                if (rejected_candidates_.find(candidate_key(cand)) != rejected_candidates_.end()) {
                    continue;
                }
                all.push_back(cand);
            }
        };

        for (int width : widths) {
            if (width >= path_len) {
                continue;
            }
            int step = max(1, width / 2);
            int first = static_cast<int>(scan_cursor_ % static_cast<size_t>(max(1, path_len - width)));
            int start = first;
            for (int seen = 0; seen < max_windows_per_width; ++seen) {
                consider(start, start + width);
                start += step;
                if (start + width > path_len) {
                    start = 0;
                }
                if (start == first) {
                    break;
                }
            }
        }
        sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) {
            return a.score > b.score;
        });
        if (static_cast<int>(all.size()) > top_k) {
            all.resize(top_k);
        }
        return all;
    }

    string make_key(const PackedBoard& cells) const {
        string key;
        key.reserve(cells.size() * 2);
        for (uint16_t value : cells) {
            key.push_back(static_cast<char>(value & 0xff));
            key.push_back(static_cast<char>((value >> 8) & 0xff));
        }
        return key;
    }

    PackedBoard key_to_cells(const string& key) const {
        PackedBoard cells;
        cells.reserve(key.size() / 2);
        for (size_t i = 0; i + 1 < key.size(); i += 2) {
            uint16_t lo = static_cast<uint8_t>(key[i]);
            uint16_t hi = static_cast<uint8_t>(key[i + 1]);
            cells.push_back(static_cast<uint16_t>(lo | (hi << 8)));
        }
        return cells;
    }

    int find_blank_local(const PackedBoard& cells) const {
        for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
            if (cells[i] == kBlankCell) {
                return i;
            }
        }
        return -1;
    }

    string reconstruct_meeting_path(const string& meet,
                                    const unordered_map<string, ParentStep>& forward,
                                    const unordered_map<string, ParentStep>& backward) const {
        string left;
        string cur = meet;
        while (true) {
            auto it = forward.find(cur);
            if (it == forward.end() || it->second.parent.empty()) {
                break;
            }
            left.push_back(it->second.move);
            cur = it->second.parent;
        }
        reverse(left.begin(), left.end());

        string right;
        cur = meet;
        while (true) {
            auto it = backward.find(cur);
            if (it == backward.end() || it->second.parent.empty()) {
                break;
            }
            right.push_back(inverse_command(it->second.move));
            cur = it->second.parent;
        }
        return left + right;
    }

    bool expand_patch_frontier(deque<QueueItem>& q,
                               unordered_map<string, ParentStep>& own,
                               const unordered_map<string, ParentStep>& other,
                               int r0,
                               int c0,
                               int size,
                               int max_depth,
                               size_t node_cap,
                               const Timer& attempt_timer,
                               int time_cap_ms,
                               string& meet) const {
        size_t level_count = q.size();
        while (level_count-- > 0) {
            if (attempt_timer.elapsed_ms() >= time_cap_ms || own.size() >= node_cap) {
                return false;
            }
            QueueItem item = q.front();
            q.pop_front();
            if (item.depth >= max_depth) {
                continue;
            }
            PackedBoard cells = key_to_cells(item.key);
            int blank_local = find_blank_local(cells);
            if (blank_local < 0) {
                continue;
            }
            int br = blank_local / size;
            int bc = blank_local % size;
            const int dr[4] = {1, -1, 0, 0};
            const int dc[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) {
                int nr = br + dr[k];
                int nc = bc + dc[k];
                if (nr < 0 || nr >= size || nc < 0 || nc >= size) {
                    continue;
                }
                int next_local = nr * size + nc;
                PackedBoard next = cells;
                swap(next[blank_local], next[next_local]);
                string next_key = make_key(next);
                if (own.find(next_key) != own.end()) {
                    continue;
                }
                int global_blank = global_index(blank_local, r0, c0, size);
                int global_next = global_index(next_local, r0, c0, size);
                char cmd = command_for_blank_step(p_, global_blank, global_next);
                own.emplace(next_key, ParentStep{item.key, cmd, item.depth + 1});
                if (other.find(next_key) != other.end()) {
                    meet = next_key;
                    return true;
                }
                q.push_back({next_key, item.depth + 1});
            }
        }
        return false;
    }

    optional<string> solve_exact_patch(const PackedBoard& start,
                                       const PackedBoard& target,
                                       int r0,
                                       int c0,
                                       int size,
                                       int old_length,
                                       int time_cap_ms,
                                       size_t node_cap) const {
        string start_key = make_key(start);
        string target_key = make_key(target);
        if (start_key == target_key) {
            return string();
        }
        Timer attempt_timer;
        unordered_map<string, ParentStep> forward;
        unordered_map<string, ParentStep> backward;
        forward.reserve(min<size_t>(node_cap, 4096));
        backward.reserve(min<size_t>(node_cap, 4096));
        deque<QueueItem> qf;
        deque<QueueItem> qb;
        forward.emplace(start_key, ParentStep{"", 0, 0});
        backward.emplace(target_key, ParentStep{"", 0, 0});
        qf.push_back({start_key, 0});
        qb.push_back({target_key, 0});
        string meet;
        const int max_depth = max(0, old_length - 1);
        while (!qf.empty() && !qb.empty() && attempt_timer.elapsed_ms() < time_cap_ms &&
               forward.size() + backward.size() < node_cap) {
            bool found = false;
            if (qf.size() <= qb.size()) {
                found = expand_patch_frontier(qf, forward, backward, r0, c0, size, max_depth,
                                              node_cap, attempt_timer, time_cap_ms, meet);
            } else {
                found = expand_patch_frontier(qb, backward, forward, r0, c0, size, max_depth,
                                              node_cap, attempt_timer, time_cap_ms, meet);
            }
            if (found) {
                string path = reconstruct_meeting_path(meet, forward, backward);
                if (static_cast<int>(path.size()) < old_length) {
                    return path;
                }
                return nullopt;
            }
        }
        return nullopt;
    }

    bool relaxed_match_blank_anywhere(const PackedBoard& cells, const PackedBoard& target) const {
        for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
            if (target[i] == kBlankCell) {
                continue;
            }
            if (cells[i] != target[i]) {
                return false;
            }
        }
        return true;
    }

    optional<pair<string, PackedBoard>> solve_relaxed_prefix(const PackedBoard& start,
                                                            const PackedBoard& target,
                                                            int r0,
                                                            int c0,
                                                            int size,
                                                            int old_length,
                                                            int time_cap_ms,
                                                            size_t node_cap) const {
        string start_key = make_key(start);
        Timer attempt_timer;
        unordered_map<string, ParentStep> parent;
        deque<QueueItem> q;
        parent.reserve(min<size_t>(node_cap, 4096));
        parent.emplace(start_key, ParentStep{"", 0, 0});
        q.push_back({start_key, 0});
        while (!q.empty() && attempt_timer.elapsed_ms() < time_cap_ms && parent.size() < node_cap) {
            QueueItem item = q.front();
            q.pop_front();
            PackedBoard cells = key_to_cells(item.key);
            if (relaxed_match_blank_anywhere(cells, target)) {
                string path;
                string cur = item.key;
                while (true) {
                    auto it = parent.find(cur);
                    if (it == parent.end() || it->second.parent.empty()) {
                        break;
                    }
                    path.push_back(it->second.move);
                    cur = it->second.parent;
                }
                reverse(path.begin(), path.end());
                return make_pair(path, cells);
            }
            if (item.depth >= old_length - 1) {
                continue;
            }
            int blank_local = find_blank_local(cells);
            if (blank_local < 0) {
                continue;
            }
            int br = blank_local / size;
            int bc = blank_local % size;
            const int dr[4] = {1, -1, 0, 0};
            const int dc[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) {
                int nr = br + dr[k];
                int nc = bc + dc[k];
                if (nr < 0 || nr >= size || nc < 0 || nc >= size) {
                    continue;
                }
                int next_local = nr * size + nc;
                PackedBoard next = cells;
                swap(next[blank_local], next[next_local]);
                string next_key = make_key(next);
                if (parent.find(next_key) != parent.end()) {
                    continue;
                }
                int global_blank = global_index(blank_local, r0, c0, size);
                int global_next = global_index(next_local, r0, c0, size);
                char cmd = command_for_blank_step(p_, global_blank, global_next);
                parent.emplace(next_key, ParentStep{item.key, cmd, item.depth + 1});
                q.push_back({next_key, item.depth + 1});
            }
        }
        return nullopt;
    }

    optional<string> solve_relaxed_5x5(const PackedBoard& start,
                                       const PackedBoard& target,
                                       int r0,
                                       int c0,
                                       int old_length) const {
        int prefix_ms = max(1, opt_.patch_attempt_ms - 5);
        auto relaxed = solve_relaxed_prefix(start, target, r0, c0, 5, old_length, prefix_ms, 100000);
        if (!relaxed) {
            return nullopt;
        }
        int remaining = old_length - static_cast<int>(relaxed->first.size());
        if (remaining <= 0) {
            return nullopt;
        }
        auto rejoin = solve_exact_patch(relaxed->second, target, r0, c0, 5, remaining, 5, 20000);
        if (!rejoin) {
            return nullopt;
        }
        string path = relaxed->first + *rejoin;
        if (static_cast<int>(path.size()) < old_length) {
            return path;
        }
        return nullopt;
    }

    PackedBoard extract_patch(const vector<int>& board, int r0, int c0, int size) const {
        PackedBoard cells;
        cells.reserve(size * size);
        for (int r = r0; r < r0 + size; ++r) {
            for (int c = c0; c < c0 + size; ++c) {
                cells.push_back(pack_cell(board[p_.index(r, c)]));
            }
        }
        return cells;
    }

    int patch_lower_bound(const PackedBoard& start, const PackedBoard& target, int size) const {
        int lb = 0;
        int start_blank = find_blank_local(start);
        int target_blank = find_blank_local(target);
        int blank_to_changed = kInf;
        for (int i = 0; i < static_cast<int>(start.size()); ++i) {
            if (start[i] == kBlankCell || start[i] == target[i]) {
                continue;
            }
            if (start_blank >= 0) {
                blank_to_changed = min(blank_to_changed,
                                       abs(start_blank / size - i / size) + abs(start_blank % size - i % size));
            }
            int best = kInf;
            for (int j = 0; j < static_cast<int>(target.size()); ++j) {
                if (target[j] == start[i]) {
                    best = min(best, abs(i / size - j / size) + abs(i % size - j % size));
                }
            }
            if (best < kInf) {
                lb += best;
            }
        }
        if (start_blank >= 0 && target_blank >= 0) {
            lb += abs(start_blank / size - target_blank / size) + abs(start_blank % size - target_blank % size);
        }
        if (blank_to_changed < kInf) {
            lb += blank_to_changed;
        }
        return lb;
    }

    bool outside_unchanged(const vector<int>& start_board,
                           const vector<int>& end_board,
                           int r0,
                           int c0,
                           int size) const {
        for (int pos = 0; pos < static_cast<int>(start_board.size()); ++pos) {
            if (!in_window(pos, r0, c0, size) && start_board[pos] != end_board[pos]) {
                return false;
            }
        }
        return true;
    }

    vector<PatchJob> prepare_jobs(const vector<Candidate>& candidates,
                                  const vector<int>& sizes,
                                  size_t limit) {
        vector<PatchJob> jobs;
        unordered_set<uint64_t> seen;
        jobs.reserve(limit);
        seen.reserve(limit * 2 + 8);
        for (const Candidate& cand : candidates) {
            if (jobs.size() >= limit) {
                break;
            }
            if (find(sizes.begin(), sizes.end(), cand.size) == sizes.end()) {
                continue;
            }
            const int old_length = cand.end - cand.start;
            if (old_length <= 1) {
                rejected_candidates_.insert(candidate_key(cand));
                continue;
            }
            vector<int> start_board;
            vector<int> end_board;
            int start_blank = -1;
            int end_blank = -1;
            state_at(cand.start, start_board, start_blank);
            state_at(cand.end, end_board, end_blank);
            if (!in_window(start_blank, cand.r0, cand.c0, cand.size) ||
                !in_window(end_blank, cand.r0, cand.c0, cand.size)) {
                rejected_candidates_.insert(candidate_key(cand));
                continue;
            }
            const uint64_t start_boundary_hash = outside_hash(start_board, cand.r0, cand.c0, cand.size);
            const uint64_t end_boundary_hash = outside_hash(end_board, cand.r0, cand.c0, cand.size);
            if (start_boundary_hash != end_boundary_hash ||
                !outside_unchanged(start_board, end_board, cand.r0, cand.c0, cand.size)) {
                rejected_candidates_.insert(candidate_key(cand));
                continue;
            }
            PackedBoard start_patch = extract_patch(start_board, cand.r0, cand.c0, cand.size);
            PackedBoard end_patch = extract_patch(end_board, cand.r0, cand.c0, cand.size);
            const int margin = cand.size == 3 ? 0 : (cand.size == 4 ? 2 : 4);
            const int lb = patch_lower_bound(start_patch, end_patch, cand.size);
            if (old_length <= lb + margin) {
                rejected_candidates_.insert(candidate_key(cand));
                continue;
            }
            const uint64_t start_hash = hash_packed(start_patch);
            const uint64_t endpoint_hash = hash_packed(end_patch);
            const uint64_t signature = job_signature(cand, endpoint_hash, start_hash, start_blank);
            if (rejected_candidates_.find(signature) != rejected_candidates_.end() ||
                !seen.insert(signature).second) {
                continue;
            }
            PatchJob job;
            job.cand = cand;
            job.start_patch = std::move(start_patch);
            job.end_patch = std::move(end_patch);
            job.boundary_hash = start_boundary_hash;
            job.start_patch_hash = start_hash;
            job.endpoint_hash = endpoint_hash;
            job.signature = signature;
            job.start_blank = start_blank;
            job.end_blank = end_blank;
            job.old_length = old_length;
            job.lower_bound = lb;
            job.score_density = cand.score / static_cast<double>(max(1, old_length));
            job.generation = generation_;
            jobs.push_back(std::move(job));
        }
        return jobs;
    }

    PatchResult solve_job(const PatchJob& job) const {
        PatchResult result;
        result.job = job;
        optional<string> replacement;
        if (job.cand.size == 3) {
            replacement = solve_exact_patch(job.start_patch, job.end_patch, job.cand.r0, job.cand.c0, job.cand.size,
                                            job.old_length, min(opt_.patch_attempt_ms, 20), 200000);
        } else if (job.cand.size == 4) {
            replacement = solve_exact_patch(job.start_patch, job.end_patch, job.cand.r0, job.cand.c0, job.cand.size,
                                            job.old_length, min(opt_.patch_attempt_ms, 20), 500000);
        } else {
            replacement = solve_relaxed_5x5(job.start_patch, job.end_patch, job.cand.r0, job.cand.c0, job.old_length);
        }
        if (!replacement || static_cast<int>(replacement->size()) >= job.old_length) {
            return result;
        }
        result.success = true;
        result.replacement = *replacement;
        result.delta = job.old_length - static_cast<int>(result.replacement.size());
        return result;
    }

    bool better_result(const PatchResult& a, const PatchResult& b) const {
        if (!b.success) return a.success;
        if (!a.success) return false;
        if (a.delta != b.delta) return a.delta > b.delta;
        if (a.job.score_density != b.job.score_density) return a.job.score_density > b.job.score_density;
        if (a.job.cand.start != b.job.cand.start) return a.job.cand.start < b.job.cand.start;
        return a.job.cand.size < b.job.cand.size;
    }

    vector<PatchResult> run_batch(const vector<PatchJob>& jobs, const Timer& total_timer) const {
        vector<PatchResult> results;
        if (jobs.empty() || time_exhausted(total_timer)) {
            return results;
        }
        const int thread_count = opt_.patch_commit_policy == "deterministic" ? 1 : patch_threads_;
        const int timeslice = opt_.patch_commit_policy == "deterministic" ? opt_.patch_attempt_ms * static_cast<int>(jobs.size())
                                                                           : patch_batch_timeslice_ms_;
        Timer batch_timer;
        atomic<size_t> next_job{0};
        mutex results_mutex;
        vector<thread> workers;
        const int workers_to_start = min<int>(thread_count, static_cast<int>(jobs.size()));
        for (int t = 0; t < workers_to_start; ++t) {
            workers.emplace_back([&, t]() {
                (void)t;
                while (!time_exhausted(total_timer) && batch_timer.elapsed_ms() < timeslice) {
                    size_t idx = next_job.fetch_add(1);
                    if (idx >= jobs.size()) {
                        break;
                    }
                    PatchResult result = solve_job(jobs[idx]);
                    lock_guard<mutex> lock(results_mutex);
                    results.push_back(std::move(result));
                }
            });
        }
        for (thread& worker : workers) {
            worker.join();
        }
        return results;
    }

    optional<PatchResult> select_result(const vector<PatchResult>& results) const {
        optional<PatchResult> best;
        for (const PatchResult& result : results) {
            if (!result.success || result.job.generation != generation_) {
                continue;
            }
            if (opt_.patch_commit_policy == "first") {
                return result;
            }
            if (!best || better_result(result, *best)) {
                best = result;
            }
        }
        return best;
    }

    bool commit_result(const PatchResult& result) {
        if (!result.success || result.job.generation != generation_ ||
            static_cast<int>(result.replacement.size()) >= result.job.old_length) {
            return false;
        }
        const Candidate& cand = result.job.cand;
        string candidate_path = best_path_.substr(0, cand.start) +
                                result.replacement +
                                best_path_.substr(cand.end);
        if (!validate_solution(p_, candidate_path + "S")) {
            return false;
        }
        best_path_ = candidate_path;
        best_valid_solution_ = candidate_path;
        best_source_ = "patch";
        ++accepted_improvements_;
        ++generation_;
        rejected_candidates_.clear();
        recent_patches_.push_back({cand.start, cand.start + static_cast<int>(result.replacement.size())});
        if (recent_patches_.size() > 32) {
            recent_patches_.erase(recent_patches_.begin());
        }
        scan_cursor_ = static_cast<size_t>(max(0, cand.start - snapshot_stride_));
        rebuild_trajectory();
        publish("patch");
        save_checkpoint(opt_.checkpoint_path);
        cerr << "[Patch improvement: " << result.job.old_length << " -> " << result.replacement.size()
             << " at moves " << cand.start << ".." << cand.end
             << " | best=" << best_path_.size() << "]\n";
        return true;
    }

    void publish(const string& event) const {
        string out = best_valid_solution_;
        if (out.empty() || out.back() != 'S') {
            out.push_back('S');
        }
        atomic_write(opt_.output_path, out + "\n");

        ostringstream meta;
        meta << "{\n";
        meta << "  \"best_length\": " << best_valid_solution_.size() << ",\n";
        meta << "  \"improvement_mode\": \"constructor-first\",\n";
        meta << "  \"event\": \"" << event << "\",\n";
        meta << "  \"best_source\": \"" << best_source_ << "\",\n";
        meta << "  \"constructor_engine\": \"" << opt_.constructor_engine << "\",\n";
        meta << "  \"constructor_threads\": " << (opt_.constructor_threads > 0 ? opt_.constructor_threads : (opt_.anytime_threads > 0 ? opt_.anytime_threads : max(1, static_cast<int>((thread::hardware_concurrency() == 0 ? 2 : thread::hardware_concurrency()) - 1)))) << ",\n";
        meta << "  \"constructor_configs\": " << (opt_.constructor_configs > 0 ? opt_.constructor_configs : (p_.n >= 30 ? (opt_.large_constructor_configs > 0 ? opt_.large_constructor_configs : 2048) : (p_.n >= 16 ? 768 : 512))) << ",\n";
        meta << "  \"constructor_seed\": " << opt_.constructor_seed << ",\n";
        meta << "  \"constructor_ratio\": " << opt_.constructor_ratio << ",\n";
        meta << "  \"constructor_config_time_ms\": " << opt_.constructor_config_time_ms << ",\n";
        meta << "  \"constructor_repair_jobs\": " << opt_.constructor_repair_jobs << ",\n";
        meta << "  \"cleanup_ms\": " << opt_.cleanup_ms << ",\n";
        meta << "  \"cleanup_time_ms\": " << cleanup_time_ms_ << ",\n";
        meta << "  \"cleanup_commits\": " << cleanup_commits_ << ",\n";
        meta << "  \"constructor_configs_attempted\": " << anytime_configs_attempted_ << ",\n";
        meta << "  \"constructor_valid\": " << anytime_valid_solutions_ << ",\n";
        meta << "  \"constructor_elites\": " << anytime_elite_updates_ << ",\n";
        meta << "  \"constructor_best_config\": \"" << best_anytime_config_id_ << "\",\n";
        meta << "  \"constructor_checkpoints\": " << anytime_checkpoint_jobs_ << ",\n";
        meta << "  \"elite_repair_jobs\": " << anytime_repair_jobs_ << ",\n";
        meta << "  \"elite_repair_valid\": " << anytime_repair_valid_ << ",\n";
        meta << "  \"accepted_improvements\": " << accepted_improvements_ << ",\n";
        meta << "  \"rejected_candidates\": " << rejected_candidates_.size() << ",\n";
        meta << "  \"snapshot_stride\": " << snapshot_stride_ << ",\n";
        meta << "  \"snapshots\": " << snapshots_.size() << ",\n";
        meta << "  \"patch_top_k\": " << opt_.patch_top_k << ",\n";
        meta << "  \"patch_attempt_ms\": " << opt_.patch_attempt_ms << ",\n";
        meta << "  \"patch_threads\": " << patch_threads_ << ",\n";
        meta << "  \"patch_batch_size\": " << patch_batch_size_ << ",\n";
        meta << "  \"patch_batch_timeslice_ms\": " << patch_batch_timeslice_ms_ << ",\n";
        meta << "  \"patch_commit_policy\": \"" << opt_.patch_commit_policy << "\",\n";
        meta << "  \"macro_strategy\": \"" << opt_.macro_strategy << "\",\n";
        meta << "  \"macro_stall_ms\": " << opt_.macro_stall_ms << ",\n";
        meta << "  \"macro_time_ratio\": " << opt_.macro_time_ratio << ",\n";
        meta << "  \"generation\": " << generation_ << ",\n";
        meta << "  \"batches\": " << total_batches_ << ",\n";
        meta << "  \"batches_5x5\": " << total_5x5_batches_ << ",\n";
        meta << "  \"jobs_attempted\": " << total_jobs_ << ",\n";
        meta << "  \"macro_attempts\": " << macro_attempts_ << ",\n";
        meta << "  \"macro_commits\": " << macro_commits_ << ",\n";
        meta << "  \"macro_transport_attempts\": " << macro_transport_attempts_ << ",\n";
        meta << "  \"macro_transport_commits\": " << macro_transport_commits_ << ",\n";
        meta << "  \"macro_strip_attempts\": " << macro_strip_attempts_ << ",\n";
        meta << "  \"macro_strip_commits\": " << macro_strip_commits_ << ",\n";
        meta << "  \"macro_sweep_attempts\": " << macro_sweep_attempts_ << ",\n";
        meta << "  \"macro_sweep_commits\": " << macro_sweep_commits_ << ",\n";
        meta << "  \"macro_stall_switches\": " << macro_stall_switches_ << ",\n";
        meta << "  \"anytime_constructive\": " << (opt_.anytime_constructive ? "true" : "false") << ",\n";
        meta << "  \"anytime_engine\": \"" << opt_.anytime_engine << "\",\n";
        meta << "  \"anytime_threads\": " << (opt_.anytime_threads > 0 ? opt_.anytime_threads : max(1, static_cast<int>((thread::hardware_concurrency() == 0 ? 2 : thread::hardware_concurrency()) - 1))) << ",\n";
        meta << "  \"anytime_attempt_ms\": " << opt_.anytime_attempt_ms << ",\n";
        meta << "  \"anytime_ratio\": " << opt_.anytime_ratio << ",\n";
        meta << "  \"anytime_seed\": " << opt_.anytime_seed << ",\n";
        meta << "  \"anytime_configs_attempted\": " << anytime_configs_attempted_ << ",\n";
        meta << "  \"anytime_valid_solutions\": " << anytime_valid_solutions_ << ",\n";
        meta << "  \"anytime_elite_updates\": " << anytime_elite_updates_ << ",\n";
        meta << "  \"anytime_repair_jobs\": " << anytime_repair_jobs_ << ",\n";
        meta << "  \"anytime_repair_commits\": " << anytime_repair_commits_ << ",\n";
        meta << "  \"anytime_monotone_attempts\": " << anytime_monotone_attempts_ << ",\n";
        meta << "  \"anytime_monotone_valid\": " << anytime_monotone_valid_ << ",\n";
        meta << "  \"anytime_repair_attempts\": " << anytime_repair_jobs_ << ",\n";
        meta << "  \"anytime_repair_valid\": " << anytime_repair_valid_ << ",\n";
        meta << "  \"anytime_checkpoint_jobs\": " << anytime_checkpoint_jobs_ << ",\n";
        meta << "  \"anytime_cancelled_opposites\": " << anytime_cancelled_opposites_ << ",\n";
        meta << "  \"best_anytime_config_id\": \"" << best_anytime_config_id_ << "\",\n";
        meta << "  \"large_constructive\": " << (opt_.large_constructive ? "true" : "false") << ",\n";
        meta << "  \"large_engine\": \"" << opt_.large_engine << "\",\n";
        meta << "  \"large_ratio\": " << opt_.large_ratio << ",\n";
        meta << "  \"large_band_size\": " << opt_.large_band_size << ",\n";
        meta << "  \"large_beam_width\": " << opt_.large_beam_width << ",\n";
        meta << "  \"large_band_candidates\": " << opt_.large_band_candidates << ",\n";
        meta << "  \"large_suffix_job_limit\": " << opt_.large_suffix_jobs << ",\n";
        meta << "  \"large_constructor_ratio\": " << opt_.large_constructor_ratio << ",\n";
        meta << "  \"large_patch_ratio\": " << opt_.large_patch_ratio << ",\n";
        meta << "  \"large_constructor_configs\": " << (opt_.large_constructor_configs > 0 ? opt_.large_constructor_configs : 2048) << ",\n";
        meta << "  \"large_constructor_min_ms\": " << opt_.large_constructor_min_ms << ",\n";
        meta << "  \"large_config_time_ms\": " << opt_.large_config_time_ms << ",\n";
        meta << "  \"large_repair_from_elites\": " << opt_.large_repair_from_elites << ",\n";
        meta << "  \"large_attempts\": " << large_attempts_ << ",\n";
        meta << "  \"large_commits\": " << large_commits_ << ",\n";
        meta << "  \"large_sweep_attempts\": " << large_sweep_attempts_ << ",\n";
        meta << "  \"large_sweep_commits\": " << large_sweep_commits_ << ",\n";
        meta << "  \"large_construct_attempts\": " << large_construct_attempts_ << ",\n";
        meta << "  \"large_construct_commits\": " << large_construct_commits_ << ",\n";
        meta << "  \"large_construct_configs\": " << large_construct_configs_ << ",\n";
        meta << "  \"large_center_loop_candidates\": " << large_center_loop_candidates_ << ",\n";
        meta << "  \"large_center_loop_commits\": " << large_center_loop_commits_ << ",\n";
        meta << "  \"large_center_loop_best_delta\": " << large_center_loop_best_delta_ << ",\n";
        meta << "  \"large_beam_states\": " << large_beam_states_ << ",\n";
        meta << "  \"large_band_commits\": " << large_band_commits_ << ",\n";
        meta << "  \"large_best_band_config\": \"" << large_best_band_config_ << "\",\n";
        meta << "  \"large_partial_states\": " << large_partial_states_ << ",\n";
        meta << "  \"large_suffix_jobs\": " << large_suffix_jobs_ << ",\n";
        meta << "  \"large_suffix_valid\": " << large_suffix_valid_ << ",\n";
        meta << "  \"large_band_v2_commits\": " << large_band_v2_commits_ << ",\n";
        meta << "  \"large_best_partial_band\": \"" << large_best_partial_band_ << "\",\n";
        meta << "  \"large_best_suffix_config\": \"" << large_best_suffix_config_ << "\",\n";
        meta << "  \"large_constructor_configs_attempted\": " << (p_.n >= 30 ? anytime_configs_attempted_ : 0) << ",\n";
        meta << "  \"large_constructor_valid\": " << (p_.n >= 30 ? anytime_valid_solutions_ : 0) << ",\n";
        meta << "  \"large_constructor_elites\": " << (p_.n >= 30 ? anytime_elite_updates_ : 0) << ",\n";
        meta << "  \"large_constructor_best_config\": \"" << (p_.n >= 30 ? best_anytime_config_id_ : string("none")) << "\",\n";
        meta << "  \"large_constructor_checkpoints\": " << (p_.n >= 30 ? anytime_checkpoint_jobs_ : 0) << ",\n";
        meta << "  \"large_elite_repair_jobs\": " << (p_.n >= 30 ? anytime_repair_jobs_ : 0) << ",\n";
        meta << "  \"large_elite_repair_valid\": " << (p_.n >= 30 ? anytime_repair_valid_ : 0) << ",\n";
        meta << "  \"large_patch_cleanup_ms\": " << large_patch_cleanup_ms_ << ",\n";
        meta << "  \"large_patch_cleanup_commits\": " << large_patch_cleanup_commits_ << ",\n";
        meta << "  \"elapsed_ms\": " << last_run_elapsed_ms_ << ",\n";
        meta << "  \"exit_reason\": \"" << exit_reason_ << "\",\n";
        meta << "  \"constructive_mode\": \"" << opt_.constructive_mode << "\"\n";
        meta << "}\n";
        atomic_write(opt_.meta_path, meta.str());
    }

    template <class T>
    static void write_raw(ofstream& out, const T& value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    template <class T>
    static void read_raw(ifstream& in, T& value) {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        if (!in) {
            throw runtime_error("bad patch checkpoint");
        }
    }

    static void write_string(ofstream& out, const string& s) {
        uint64_t len = s.size();
        write_raw(out, len);
        out.write(s.data(), static_cast<streamsize>(s.size()));
    }

    static string read_string(ifstream& in) {
        uint64_t len = 0;
        read_raw(in, len);
        string s(len, '\0');
        in.read(s.data(), static_cast<streamsize>(len));
        if (!in) {
            throw runtime_error("bad patch checkpoint string");
        }
        return s;
    }

    void save_checkpoint(const string& path) const {
        if (path.empty()) {
            return;
        }
        const string tmp = path + ".tmp";
        ofstream out(tmp, ios::binary);
        if (!out) {
            return;
        }
        const char magic[8] = {'P', 'A', 'T', 'C', 'H', '0', '1', '\0'};
        out.write(magic, sizeof(magic));
        write_raw(out, p_.n);
        write_raw(out, p_.initial_hash.lo);
        write_raw(out, p_.initial_hash.hi);
        write_string(out, best_valid_solution_);
        write_raw(out, scan_cursor_);
        write_raw(out, accepted_improvements_);
        write_raw(out, snapshot_stride_);
        uint64_t recent_count = recent_patches_.size();
        write_raw(out, recent_count);
        for (const auto& [a, b] : recent_patches_) {
            write_raw(out, a);
            write_raw(out, b);
        }
        uint64_t rejected_count = rejected_candidates_.size();
        write_raw(out, rejected_count);
        for (uint64_t key : rejected_candidates_) {
            write_raw(out, key);
        }
        write_raw(out, generation_);
        write_raw(out, total_batches_);
        write_raw(out, total_5x5_batches_);
        write_raw(out, total_jobs_);
        write_raw(out, macro_attempts_);
        write_raw(out, macro_commits_);
        write_raw(out, macro_transport_attempts_);
        write_raw(out, macro_transport_commits_);
        write_raw(out, macro_strip_attempts_);
        write_raw(out, macro_strip_commits_);
        write_raw(out, macro_sweep_attempts_);
        write_raw(out, macro_sweep_commits_);
        write_raw(out, macro_stall_switches_);
        write_raw(out, anytime_configs_attempted_);
        write_raw(out, anytime_valid_solutions_);
        write_raw(out, anytime_elite_updates_);
        write_raw(out, anytime_repair_jobs_);
        write_raw(out, anytime_repair_commits_);
        write_raw(out, large_attempts_);
        write_raw(out, large_commits_);
        write_raw(out, large_sweep_attempts_);
        write_raw(out, large_sweep_commits_);
        write_raw(out, large_construct_attempts_);
        write_raw(out, large_construct_commits_);
        write_raw(out, large_construct_configs_);
        write_raw(out, large_center_loop_candidates_);
        write_raw(out, large_center_loop_commits_);
        write_raw(out, large_center_loop_best_delta_);
        write_raw(out, large_beam_states_);
        write_raw(out, large_band_commits_);
        write_raw(out, anytime_monotone_attempts_);
        write_raw(out, anytime_monotone_valid_);
        write_raw(out, anytime_repair_valid_);
        write_raw(out, anytime_checkpoint_jobs_);
        write_raw(out, anytime_cancelled_opposites_);
        write_raw(out, large_partial_states_);
        write_raw(out, large_suffix_jobs_);
        write_raw(out, large_suffix_valid_);
        write_raw(out, large_band_v2_commits_);
        write_raw(out, large_patch_cleanup_ms_);
        write_raw(out, large_patch_cleanup_commits_);
        out.close();
        remove(path.c_str());
        rename(tmp.c_str(), path.c_str());
    }

    const Problem& p_;
    const Options& opt_;
    string best_path_;
    string best_valid_solution_;
    string best_source_ = "none";
    vector<TrajectorySnapshot> snapshots_;
    vector<MoveInfo> moves_;
    vector<pair<int, int>> recent_patches_;
    unordered_set<uint64_t> rejected_candidates_;
    size_t scan_cursor_ = 0;
    int accepted_improvements_ = 0;
    int snapshot_stride_ = 64;
    int patch_threads_ = 1;
    int patch_batch_size_ = 4;
    int patch_batch_timeslice_ms_ = 30;
    uint64_t generation_ = 0;
    uint64_t total_batches_ = 0;
    uint64_t total_5x5_batches_ = 0;
    uint64_t total_jobs_ = 0;
    uint64_t macro_attempts_ = 0;
    uint64_t macro_commits_ = 0;
    uint64_t macro_transport_attempts_ = 0;
    uint64_t macro_transport_commits_ = 0;
    uint64_t macro_strip_attempts_ = 0;
    uint64_t macro_strip_commits_ = 0;
    uint64_t macro_sweep_attempts_ = 0;
    uint64_t macro_sweep_commits_ = 0;
    uint64_t macro_stall_switches_ = 0;
    uint64_t anytime_configs_attempted_ = 0;
    uint64_t anytime_valid_solutions_ = 0;
    uint64_t anytime_elite_updates_ = 0;
    uint64_t anytime_repair_jobs_ = 0;
    uint64_t anytime_repair_commits_ = 0;
    uint64_t anytime_monotone_attempts_ = 0;
    uint64_t anytime_monotone_valid_ = 0;
    uint64_t anytime_repair_valid_ = 0;
    uint64_t anytime_checkpoint_jobs_ = 0;
    uint64_t anytime_cancelled_opposites_ = 0;
    string best_anytime_config_id_ = "none";
    uint64_t large_attempts_ = 0;
    uint64_t large_commits_ = 0;
    uint64_t large_sweep_attempts_ = 0;
    uint64_t large_sweep_commits_ = 0;
    uint64_t large_construct_attempts_ = 0;
    uint64_t large_construct_commits_ = 0;
    uint64_t large_construct_configs_ = 0;
    uint64_t large_center_loop_candidates_ = 0;
    uint64_t large_center_loop_commits_ = 0;
    uint64_t large_center_loop_best_delta_ = 0;
    uint64_t large_beam_states_ = 0;
    uint64_t large_band_commits_ = 0;
    uint64_t large_partial_states_ = 0;
    uint64_t large_suffix_jobs_ = 0;
    uint64_t large_suffix_valid_ = 0;
    uint64_t large_band_v2_commits_ = 0;
    uint64_t large_patch_cleanup_ms_ = 0;
    uint64_t large_patch_cleanup_commits_ = 0;
    uint64_t cleanup_time_ms_ = 0;
    uint64_t cleanup_commits_ = 0;
    string large_best_band_config_ = "none";
    string large_best_partial_band_ = "none";
    string large_best_suffix_config_ = "none";
    long long last_run_elapsed_ms_ = 0;
    string exit_reason_ = "not_run";
};

class SolverApp {
public:
    SolverApp(Problem p, Options opt) : p_(std::move(p)), opt_(std::move(opt)) {}

    int run() {
        Timer timer;
        if (opt_.improvement_mode == "constructor-first") {
            ConstructorFirstSolver improver(p_, opt_);
            bool resumed = improver.load_checkpoint(opt_.resume_path);
            if (!resumed) {
                if (opt_.no_constructive) {
                    throw runtime_error("--no-constructive cannot start constructor-first mode without a checkpoint");
                }
                ConstructiveSolver constructive(p_, opt_.constructive_mode);
                string seed;
                if (!constructive.solve(seed)) {
                    cerr << "Constructive solver failed to produce a seed.\n";
                    return 2;
                }
                improver.set_seed(seed, "constructive");
            }
            improver.run(timer);
            cout << "best_length=" << improver.best_length()
                 << " improvement=constructor-first"
                 << " elapsed_ms=" << improver.elapsed_ms()
                 << " exit_reason=" << improver.exit_reason()
                 << "\n";
            return 0;
        }

        if (opt_.improvement_mode == "none") {
            if (opt_.no_constructive) {
                throw runtime_error("--improvement none requires constructive solving");
            }
            ConstructiveSolver constructive(p_, opt_.constructive_mode);
            string seed;
            if (!constructive.solve(seed)) {
                cerr << "Constructive solver failed to produce a seed.\n";
                return 2;
            }
            string out = seed;
            if (out.empty() || out.back() != 'S') {
                out.push_back('S');
            }
            atomic_write(opt_.output_path, out + "\n");
            ostringstream meta;
            meta << "{\n";
            meta << "  \"best_length\": " << seed.size() << ",\n";
            meta << "  \"improvement_mode\": \"none\",\n";
            meta << "  \"best_source\": \"constructive\",\n";
            meta << "  \"constructive_mode\": \"" << opt_.constructive_mode << "\"\n";
            meta << "}\n";
            atomic_write(opt_.meta_path, meta.str());
            cout << "best_length=" << seed.size() << " improvement=none\n";
            return 0;
        }

        throw runtime_error("unsupported improvement mode");
    }

private:
    Problem p_;
    Options opt_;
};

Problem make_problem_for_test(int n, vector<int> board, vector<int> target_center) {
    Problem p;
    p.n = n;
    p.initial = std::move(board);
    p.target_at.assign(n * n, kNoTarget);
    for (int i = 0; i < n * n; ++i) {
        if (p.initial[i] == -1) {
            p.initial_blank = i;
        }
    }
    int k = 0;
    for (int r = 1; r + 1 < n; ++r) {
        for (int c = 1; c + 1 < n; ++c) {
            int pos = p.index(r, c);
            p.target_at[pos] = target_center[k++];
            p.inner_positions.push_back(pos);
        }
    }
    p.initial_hash = p.compute_hash(p.initial);
    return p;
}

void run_self_tests() {
    {
        Problem p = make_problem_for_test(3, {1, 2, 3, 4, -1, 5, 6, 7, 8}, {7});
        vector<int> b = p.initial;
        int blank = p.initial_blank;
        bool ok = apply_move(p, b, blank, 'U');
        assert(ok);
        assert(blank == p.index(2, 1));
        assert(b[p.index(1, 1)] == 7);
        ok = apply_move(p, b, blank, 'D');
        assert(ok);
        assert(blank == p.index(1, 1));
    }
    {
        Problem p = make_problem_for_test(3, {9, 9, 9, 9, 5, 9, 9, -1, 9}, {5});
        assert(p.is_goal(p.initial));
    }
    {
        Problem p = make_problem_for_test(3, {1, 2, 3, 4, -1, 5, 6, 7, 8}, {7});
        ConstructiveSolver constructive(p, "paired");
        string path;
        bool ok = constructive.solve(path);
        assert(ok);
        assert(validate_solution(p, path + "S"));
    }
    {
        Problem p = make_problem_for_test(3, {1, 2, 3, 4, -1, 5, 6, 7, 8}, {7});
        Options opt;
        opt.output_path.clear();
        opt.meta_path.clear();
        opt.time_limit_ms = 200;
        opt.patch_top_k = 20;
        opt.patch_max_commits = 1;
        ConstructorFirstSolver improver(p, opt);
        improver.set_seed("UDU", "test");
        Timer timer;
        improver.run(timer);
        assert(improver.best_length() == 1);
    }
    cout << "self-tests passed\n";
}

}  // namespace

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    try {
        Options opt = parse_options(argc, argv);
        if (opt.self_test) {
            run_self_tests();
            return 0;
        }

        Problem problem;
        if (!opt.input_path.empty()) {
            ifstream in(opt.input_path);
            if (!in) {
                throw runtime_error("failed to open input file: " + opt.input_path);
            }
            problem = Problem::read(in);
        } else {
            problem = Problem::read(cin);
        }
        apply_constructor_first_defaults(problem, opt);

        if (!opt.validate_solution_path.empty()) {
            ifstream sol(opt.validate_solution_path);
            if (!sol) {
                throw runtime_error("failed to open solution file: " + opt.validate_solution_path);
            }
            string raw((istreambuf_iterator<char>(sol)), istreambuf_iterator<char>());
            string moves;
            for (char ch : raw) {
                if (ch == 'U' || ch == 'D' || ch == 'L' || ch == 'R' || ch == 'S') {
                    moves.push_back(ch);
                    if (ch == 'S') {
                        break;
                    }
                }
            }
            bool ok = validate_solution(problem, moves);
            int length = 0;
            for (char ch : moves) {
                if (ch == 'S') break;
                ++length;
            }
            cout << (ok ? "valid" : "invalid") << " length=" << length << "\n";
            return ok ? 0 : 3;
        }

        SolverApp app(std::move(problem), std::move(opt));
        return app.run();
    } catch (const exception& ex) {
        cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
