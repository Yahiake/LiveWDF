#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wdf::ppdiag {

inline long long sample = 0;

struct BlockDumpRecord {
    std::string run, circuit, scope, result;
    long long sample = 0, solve = 0;
    int block = 0, iterations = 0, n = 0;
    bool failed = false;
    struct Device {
        int group = 0, component = 0, portBase = 0, portCount = 0;
        std::string kind, name;
        std::vector<int> global, local;
        std::vector<double> g;
    };
    std::vector<Device> devices;
    std::vector<int> ports;
    std::vector<double> z, vOc;
};

class BlockDump {
    std::string prefix_;
    std::vector<int> ports_;
    int limit_ = 0;
    std::vector<BlockDumpRecord> records_;

    static void jsonString(std::FILE* f, const std::string& s) {
        std::fputc('"', f);
        for (const unsigned char c : s) {
            if (c == '"' || c == '\\') std::fputc('\\', f);
            if (c >= 0x20) std::fputc(c, f);
        }
        std::fputc('"', f);
    }

    static bool worse(const BlockDumpRecord& a, const BlockDumpRecord& b) {
        return a.failed != b.failed ? !a.failed : a.iterations < b.iterations;
    }

public:
    BlockDump() {
        const char* prefix = std::getenv("LWDF_BLOCK_DUMP");
        const char* wanted = std::getenv("LWDF_BLOCK_DUMP_PORTS");
        if (!wanted) wanted = std::getenv("PORTS");
        if (!prefix || !*prefix || !wanted || !*wanted) return;
        prefix_ = prefix;
        for (const char* p = wanted; *p;) {
            char* end = nullptr;
            const long value = std::strtol(p, &end, 10);
            if (end == p) break;
            if (value > 0) ports_.push_back(static_cast<int>(value));
            p = *end == ',' ? end + 1 : end;
        }
        const char* limit = std::getenv("LWDF_BLOCK_DUMP_LIMIT");
        if (!limit) limit = std::getenv("LIMIT");
        limit_ = std::clamp(limit ? std::atoi(limit) : 64, 0, 64);
        records_.reserve(static_cast<size_t>(limit_));
    }

    ~BlockDump() {
        if (records_.empty()) return;
        std::ranges::sort(records_, [](const auto& a, const auto& b) {
            if (a.failed != b.failed) return a.failed > b.failed;
            if (a.iterations != b.iterations) return a.iterations > b.iterations;
            return a.solve < b.solve;
        });
        std::FILE* f = std::fopen((prefix_ + ".jsonl").c_str(), "w");
        if (!f) return;
        for (const auto& r : records_) {
            std::fputs("{\"run\":", f); jsonString(f, r.run);
            std::fputs(",\"circuit\":", f); jsonString(f, r.circuit);
            std::fputs(",\"scope\":", f); jsonString(f, r.scope);
            std::fprintf(f, ",\"sample\":%lld,\"solve\":%lld,\"block\":%d,\"result\":", r.sample, r.solve, r.block);
            jsonString(f, r.result);
            std::fprintf(f, ",\"iterations\":%d,\"ports\":[", r.iterations);
            for (size_t i = 0; i < r.ports.size(); ++i) std::fprintf(f, "%s%d", i ? "," : "", r.ports[i]);
            std::fputs("],\"devices\":[", f);
            for (size_t d = 0; d < r.devices.size(); ++d) {
                const auto& dev = r.devices[d];
                std::fprintf(f, "%s{\"group\":%d,\"kind\":", d ? "," : "", dev.group); jsonString(f, dev.kind);
                std::fprintf(f, ",\"component\":%d,\"name\":", dev.component); jsonString(f, dev.name);
                std::fprintf(f, ",\"port_base\":%d,\"port_count\":%d,\"global_ports\":[", dev.portBase, dev.portCount);
                for (size_t i = 0; i < dev.global.size(); ++i) std::fprintf(f, "%s%d", i ? "," : "", dev.global[i]);
                std::fputs("],\"local_ports\":[", f);
                for (size_t i = 0; i < dev.local.size(); ++i) std::fprintf(f, "%s%d", i ? "," : "", dev.local[i]);
                std::fputs("],\"G\":[", f);
                for (size_t i = 0; i < dev.g.size(); ++i) std::fprintf(f, "%s%.17g", i ? "," : "", dev.g[i]);
                std::fputs("]}", f);
            }
            std::fputs("],\"Z\":[", f);
            for (size_t i = 0; i < r.z.size(); ++i) std::fprintf(f, "%s%.17g", i ? "," : "", r.z[i]);
            std::fputs("],\"vOc\":[", f);
            for (size_t i = 0; i < r.vOc.size(); ++i) std::fprintf(f, "%s%.17g", i ? "," : "", r.vOc[i]);
            std::fputs("]}\n", f);
        }
        std::fclose(f);
    }

    bool wants(int n) const {
        return limit_ && std::ranges::find(ports_, n) != ports_.end();
    }
    bool enabled() const { return limit_ != 0; }

    void offer(BlockDumpRecord record) {
        if (static_cast<int>(records_.size()) < limit_) {
            records_.push_back(std::move(record));
            return;
        }
        auto lowest = std::ranges::min_element(records_, [](const auto& a, const auto& b) { return worse(a, b); });
        if (worse(*lowest, record)) *lowest = std::move(record);
    }
};

inline BlockDump& blockDump() {
    static BlockDump dump;
    return dump;
}
inline long long blockDumpSolves = 0;

} // namespace wdf::ppdiag
