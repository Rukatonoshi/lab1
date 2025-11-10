#include <bits/stdc++.h>
#include <sys/mman.h>
#include <sched.h>
#include <unistd.h>
#include <chrono>

using namespace std;
using clk = chrono::steady_clock;

constexpr size_t MAX_MEMORY = 256ull * 1024 * 1024; // 256 MB total buffer
constexpr int MAX_ASSOC = 32;                       // allow large associativities
constexpr int ITERATIONS = 2000 * 1000;             // pointer-chase iterations
constexpr int REPEATS = 20;                         // repeat to smooth jitter

// ======== Low-level helpers ========
void pin_to_core0() {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        perror("sched_setaffinity");
}

char* alloc_page_aligned(size_t bytes) {
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    madvise(p, bytes, MADV_RANDOM);
    return (char*)p;
}

// build pointer-chasing ring: each element points H bytes back
__attribute__((optimize(0)))
long long measure_latency(char* data, size_t H, int S) {
    char** ptr;
    for (int i = (S - 1) * H; i >= 0; i -= H) {
        ptr = (char**)&data[i];
        *ptr = i >= H ? &data[i - H] : &data[(S - 1) * H];
    }

    char** x = (char**)&data[0];
    vector<long long> samples;
    samples.reserve(REPEATS);
    for (int k = 0; k < REPEATS; k++) {
        auto t0 = clk::now();
        for (int i = 0; i < ITERATIONS; i++) {
            x = (char**)*x;
        }
        auto t1 = clk::now();
        samples.push_back(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count());
    }
    nth_element(samples.begin(), samples.begin() + samples.size()/2, samples.end());
    return samples[samples.size()/2];
}

// detection of jumps (phase 1)
pair<vector<set<int>>, int> detect_cache_levels(char* data) {
    cout << "\n[Phase 1: Detection] Scanning strides and associativities...\n";
    vector<set<int>> jumps;
    int H = MAX_ASSOC;

    for (; H < MAX_MEMORY / MAX_ASSOC; H *= 2) {
        cout << "  stride (H) = " << setw(7) << H << " bytes\n";
        long long prev_time = measure_latency(data, H, 1);
        set<int> new_jumps;

        for (int S = 2; S <= MAX_ASSOC; S++) {
            long long curr_time = measure_latency(data, H, S);
            double diff_ns = (double)(curr_time - prev_time);
            cout << "    S=" << setw(2) << S
                 << " time=" << setw(8) << curr_time
                 << " ns  Δ=" << setw(6) << diff_ns << " ns";
            if ((curr_time - prev_time) * 10 > curr_time) {
                new_jumps.insert(S - 1);
                cout << "  <-- jump";
            }
            cout << "\n";
            prev_time = curr_time;
        }

        cout << "    detected jumps at S={";
        for (auto s : new_jumps) cout << s << " ";
        cout << "}\n";

        bool same_jump = true;
        if (!jumps.empty()) {
            auto& last = jumps.back();
            for (int j : new_jumps)
                if (!last.count(j)) same_jump = false;
            for (int j : last)
                if (!new_jumps.count(j)) same_jump = false;
        }

        if (same_jump && H >= 512 * 1024) {
            cout << "  pattern stabilized, breaking detection loop\n";
            break;
        }

        jumps.push_back(new_jumps);
    }
    return {jumps, H};
}

// reconstruct caches (phase 2)
pair<int,int> build_cache_hierarchy(const vector<set<int>>& jumps, int H) {
    cout << "\n[Phase 2: Hierarchy reconstruction]\n";
    vector<pair<int,int>> caches;
    set<int> active = jumps.back();

    for (int i = (int)jumps.size() - 1; i >= 0; i--) {
        set<int> remove;
        for (int s : active)
            if (!jumps[i].count(s)) {
                caches.emplace_back(H * s, s);
                remove.insert(s);
                cout << "  level detected: stride=" << H
                     << " assoc=" << s
                     << " cap~" << (H * s / 1024) << " KB\n";
            }
        for (int s : remove) active.erase(s);
        H /= 2;
    }

    if (caches.empty()) {
        cerr << "Could not detect cache hierarchy\n";
        exit(1);
    }

    sort(caches.begin(), caches.end());
    return caches.front(); // L1
}

// disambiguation: detect line size (phase 3)
int detect_cache_line(char* data, int cache_size, int assoc) {
    cout << "\n[Phase 3: Disambiguation] Determining line size...\n";
    int prev_first_jump = 1025;
    int line_size = -1;
    for (int L = 16; L <= cache_size; L *= 2) {
        long long prev_t = measure_latency(data, cache_size / assoc + L, 2);
        int first_jump = -1;
        for (int S = 1; S <= 512; S *= 2) {
            long long curr_t = measure_latency(data, cache_size / assoc + L, S + 1);
            if ((curr_t - prev_t) * 10 > curr_t) {
                if (first_jump < 0)
                    first_jump = S;
            }
            prev_t = curr_t;
        }
        cout << "  L=" << setw(4) << L
             << "  first_jump=" << setw(5) << first_jump
             << "  prev=" << setw(5) << prev_first_jump << "\n";
        if (first_jump > prev_first_jump) {
            line_size = L;
            cout << "    ↑ pattern change detected → line_size ≈ " << line_size << " bytes\n";
            break;
        }
        prev_first_jump = first_jump;
    }
    return line_size;
}

int main() {
    ios::sync_with_stdio(false);
    cout << "=== Robust L1 Cache Characterization ===\n";
    pin_to_core0();

    char* data = alloc_page_aligned(MAX_MEMORY);
    cout << "Allocated " << (MAX_MEMORY >> 20) << " MB via mmap (page-aligned)\n";

    auto [jumps, H] = detect_cache_levels(data);
    auto [cache_size, cache_assoc] = build_cache_hierarchy(jumps, H);
    int line_size = detect_cache_line(data, cache_size, cache_assoc);

    cout << "\n==== RESULTS ====\n";
    cout << "L1_CACHE_SIZE      = " << cache_size / 1024 << " KB\n";
    cout << "L1_CACHE_ASSOC     = " << cache_assoc << "\n";
    cout << "L1_CACHE_LINE_SIZE = " << line_size << " bytes\n";

    munmap(data, MAX_MEMORY);
    return 0;
}

