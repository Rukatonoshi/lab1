#include <sched.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <sys/mman.h>
#include <pthread.h>
#include <iomanip>
#include <cassert>
#include <cstdint>

using namespace std;
using namespace chrono;

// Configuration constans
const size_t PAGE_SIZE = sysconf(_SC_PAGE_SIZE);
const size_t BUFFER_SIZE = 128 * 1024 * 1024; // Large buffer for warm-up
const size_t ITERATIONS = 12'000'000;         // Pointer-chasing loop

int MEASURE_REPEATS = 16;                     // Number of repeats for median calculation
volatile uint64_t dummy_sink = 0;             // Prevent compiler from removing loads

// Pin process to a single core (reduces noise)
bool set_process_affinity(int cpu) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    return sched_setaffinity(0, sizeof(mask), &mask) == 0;
}

// Avoid unwanted compiler optimizations
template<typename T>
inline void blackhole(T v) {
    asm volatile("" : : "r"(v) : "memory");
}

template<typename T>
inline void blackhole_ptr(T* p) {
    asm volatile("" : : "r"(p) : "memory");
}

// Allocate aligned memory for pointer chains
void* allocate_aligned(size_t align, size_t size) {
    void* p = nullptr;
    if (posix_memalign(&p, align, size) != 0) return nullptr;
    return p;
}

// Build a random pointer cycle
void create_random_chain(void** array, size_t count) {
    vector<size_t> idx(count);
    for (size_t i = 0; i < count; i++) idx[i] = i;

    mt19937_64 rng(1234567);
    shuffle(idx.begin(), idx.end(), rng);

    for (size_t i = 0; i < count; i++)
        array[idx[i]] = &array[idx[(i + 1) % count]];
}

// Minimal warm-up pass through a chain
void warmup_chain(void** start, size_t count) {
    const void* p = start;
    for (size_t i = 0; i < count; i++) {
        p = *(void* const*)p;
        asm volatile("" : "+r"(p));
    }
    blackhole_ptr((void*)p);
}

// Pointer-chase measurement
double measure_chain_latency(void** start, size_t count) {
    // Small warm-up to stabilize line fills
    warmup_chain(start, min<size_t>(count, 8192));

    const void* p = start;
    auto t0 = steady_clock::now();

    for (size_t i = 0; i < ITERATIONS; i++) {
        p = *(void* const*)p;

        // Occasional barrier to stop the optimizer
        if ((i & 4095) == 0) asm volatile("" : : "r"(i) : "memory");
        asm volatile("" : "+r"(p));
    }

    auto t1 = steady_clock::now();
    blackhole_ptr((void*)p);
    dummy_sink ^= (uint64_t)(uintptr_t)p;

    double ns = duration_cast<duration<double, nano>>(t1 - t0).count();
    return ns / ITERATIONS;
}

double median_of_vector(vector<double> v) {
    sort(v.begin(), v.end());
    size_t n = v.size();
    if (n == 0) return 0;
    if (n & 1) return v[n/2];
    return (v[n/2 - 1] + v[n/2]) * 0.5;
}

// Line size detection: stride-based pointer chasing
// Looks for the stride at which latency jumps noticeably.
size_t detect_line_size() {
    cout << "Detecting L1 line size..." << endl;

    vector<size_t> stride_bytes = {4, 8, 16, 32, 64, 128, 256};
    vector<double> times;

    size_t ptr_count = 256 * 1024;
    void **arr = (void**)allocate_aligned(PAGE_SIZE, ptr_count * sizeof(void*));
    create_random_chain(arr, ptr_count);

    for (size_t sb : stride_bytes) {
        size_t step = max<size_t>(1, sb / sizeof(void*));
        size_t count = ptr_count / step;

        if (count < 16) { times.push_back(0); continue; }

        // Build stride-based cycle
        for (size_t i = 0; i < count; i++)
            arr[i * step] = &arr[((i + 1) % count) * step];

        // Warm-up attempts
        for (int w = 0; w < 5; w++)
            warmup_chain(arr, min<size_t>(count, 32768));

        vector<double> reps;
        for (int r = 0; r < MEASURE_REPEATS; r++)
            reps.push_back(measure_chain_latency(arr, count));

        double med = median_of_vector(reps);
        cout << "Stride " << setw(4) << sb << " bytes -> "
             << fixed << setprecision(6) << med << " ns\n";

        times.push_back(med);
    }

    // Look for relative jumps
    vector<double> rel(times.size(), 0.0);
    for (size_t i = 1; i < times.size(); i++)
        rel[i] = (times[i] - times[i-1]) / times[i-1];

    double r32_64 = rel[4];
    double r64_128 = rel[5];
    double r128_256 = rel[6];

    cout << "Rel jumps: 32->64=" << r32_64
         << ", 64->128=" << r64_128
         << ", 128->256=" << r128_256 << endl;

    // Simple heuristic: prefer 64 unless the evidence strongly indicates otherwise
    size_t chosen = 64;

    bool big_32_64  = r32_64  > 0.12;
    bool big_64_128 = r64_128 > 0.20;

    if (big_32_64 && big_64_128) chosen = 64;
    else if (big_32_64)         chosen = 64;
    else if (r64_128 > 0.35)    chosen = 128;

    if (chosen > 64) {
        if (r64_128 < 2.0 * r32_64)
            chosen = 64;
    }

    cout << "--> chosen line size = " << chosen << " bytes\n\n";

    free(arr);
    return chosen;
}

// L1 size detection using increasing working-set sizes.
// Latency increases when the working set no longer fits L1.
size_t detect_l1_size(size_t line_size) {
    cout << "Detecting L1 cache size..." << endl;

    vector<size_t> sizes_kb = {
        4,8,12,16,20,24,28,32,36,40,48,56,64,80,96,112,128
    };

    vector<double> times;

    for (size_t kb : sizes_kb) {
        size_t bytes = kb * 1024;
        size_t count = max<size_t>(4, bytes / sizeof(void*));

        void** arr = (void**)allocate_aligned(PAGE_SIZE, max(bytes, PAGE_SIZE));
        create_random_chain(arr, count);

        vector<double> res;
        res.reserve(MEASURE_REPEATS);
        for (int r = 0; r < MEASURE_REPEATS; r++)
            res.push_back(measure_chain_latency(arr, count));

        double med = median_of_vector(res);
        cout << kb << " KB: " << fixed << setprecision(6) << med << " ns" << endl;

        times.push_back(med);
        free(arr);
    }

    // Look for the first noticeable jump
    for (size_t i = 1; i < times.size(); i++) {
        if (times[i] > times[i-1] * 1.15) {
            size_t detected = sizes_kb[i - 1] * 1024;
            cout << "L1 size detected: " << (detected/1024) << " KB\n\n";
            return detected;
        }
    }

    cout << "Fallback: L1 = 32 KB\n\n";
    return 32 * 1024;
}

// Associativity detection.
// Builds conflict sets mapped to the same index by spacing
// elements one cache-size apart. Latency rises after #ways+1.
int detect_associativity(size_t line_size, size_t l1_size) {
    cout << "Detecting associativity..." << endl;

    vector<double> times;
    int max_conflicts = 20;

    // Number of pointers that place successive entries in the same set
    size_t stride_ptrs = max<size_t>(1, l1_size / sizeof(void*));

    for (int conflicts = 1; conflicts <= max_conflicts; ++conflicts) {
        size_t needed = (size_t)conflicts * stride_ptrs + 64;
        vector<void*> buf(needed, nullptr);
        vector<size_t> idx(conflicts);
        for (int i = 0; i < conflicts; ++i) idx[i] = i * stride_ptrs;

        // Randomize access order among conflict positions
        vector<int> perm(conflicts);
        for (int i = 0; i < conflicts; ++i) perm[i] = i;
        mt19937_64 rng((uint64_t)123456 + conflicts);
        for (int i = conflicts - 1; i > 0; --i)
            swap(perm[i], perm[rng() % (i + 1)]);

        // Build the cycle over the conflict points
        for (int i = 0; i < conflicts; ++i)
            buf[idx[perm[i]]] = &buf[idx[perm[(i + 1) % conflicts]]];

        // Fill remaining entries so everything forms a valid cycle
        for (size_t i = 0; i < needed; ++i)
            if (!buf[i]) buf[i] = &buf[(i + 1) % needed];

        // Warm-up
        warmup_chain((void**)&buf[idx[0]], min<size_t>(needed, 65536));

        // Measurement
        vector<double> reps;
        reps.reserve(MEASURE_REPEATS);
        for (int r = 0; r < MEASURE_REPEATS; ++r)
            reps.push_back(measure_chain_latency((void**)&buf[idx[0]], conflicts));

        double med = median_of_vector(reps);
        times.push_back(med);

        cout << setw(2) << conflicts << " conflicts -> "
             << fixed << setprecision(6) << med << " ns" << endl;
    }

    // Use the first few points as the baseline
    int baseN = min<int>(3, (int)times.size());
    double base_med = median_of_vector(vector<double>(times.begin(), times.begin() + baseN));

    // Detect jump
    for (size_t k = 1; k < times.size(); ++k) {
        double rel = times[k] / base_med;
        double absdiff = times[k] - base_med;

        if (rel > 1.25 && absdiff > 0.5) {
            bool stable = true;
            if (k + 1 < times.size()) {
                if (times[k + 1] < times[k] * 0.85)
                    stable = false;
            }
            if (stable) {
                cout << "--> associativity ≈ " << k << " ways\n\n";
                return (int)k;
            }
        }
    }

    cout << "--> associativity not clear, fallback 8 ways\n\n";
    return 8;
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    cout << "=== L1 Cache Detection ===\n";

    // Fix CPU to reduce jitter
    set_process_affinity(0);

    // Initial warm-up of memory subsystem
    void* buf = allocate_aligned(PAGE_SIZE, BUFFER_SIZE);
    memset(buf, 0xAA, BUFFER_SIZE);
    blackhole_ptr(buf);

    size_t line = detect_line_size();
    size_t l1_raw = detect_l1_size(line);
    int assoc = detect_associativity(line, l1_raw);

    // Compute number of sets
    size_t unit = line * assoc;
    size_t sets = (l1_raw + unit/2) / unit;
    size_t l1_corrected = sets * unit;

    dummy_sink ^= (int)line ^ (int)l1_corrected ^ assoc;

    cout << "\n===== FINAL RESULTS =====\n";
    cout << "Line size: " << line << " bytes\n";
    cout << "L1 size:   " << l1_corrected/1024 << " KB\n";
    cout << "Assoc:     " << assoc << " ways\n";
    cout << "Dummy:     " << dummy_sink << "\n";
}
