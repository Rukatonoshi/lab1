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

using namespace std;
using namespace chrono;

// Configuration constants
const size_t PAGE_SIZE = 4096;
const size_t BUFFER_SIZE = 128 * 1024 * 1024; // 128 MB buffer
const size_t ITERATIONS = 10000000;           // 10 million iterations for pointer chase
const int MEASURE_REPEATS = 15;               // Number of repeats for median calculation
const int FALLBACK_TRIALS = 50;               // Number of trials for line size detection
const int FALLBACK_ITERATIONS = 1000000;      // 1 million iterations per thread in line size detection method

// Allocate aligned memory
void* allocate_aligned(size_t alignment, size_t size) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        cerr << "Failed to allocate aligned memory" << endl;
        return nullptr;
    }
    return ptr;
}

// Free aligned memory
void free_aligned(void* ptr) {
    free(ptr);
}

// Prevent compiler optimization by using the pointer
void use_pointer(const void* p) {
    asm volatile("" : : "r"(p) : "memory");
}

// Calculate median of a vector of doubles
double get_median(vector<double>& values) {
    if (values.empty()) return 0.0;
    sort(values.begin(), values.end());
    size_t n = values.size();
    if (n % 2 == 1) {
        return values[n / 2];
    } else {
        return (values[n / 2 - 1] + values[n / 2]) / 2.0;
    }
}

// Measure pointer chase latency
double measure_pointer_chase(void** start, size_t iterations = ITERATIONS) {
    const void* current = start;
    auto start_time = steady_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        current = *(const void**)current;
        // Prevent optimization
        asm volatile("" : "+r"(current));
    }

    auto end_time = steady_clock::now();
    use_pointer(current);

    double nanoseconds = duration_cast<duration<double, nano>>(end_time - start_time).count();
    return nanoseconds / (double)iterations;
}

// Create random cyclic pointer chain to prevent from prefetching
void create_random_chain(void** array, size_t size) {
    vector<size_t> indices(size);
    for (size_t i = 0; i < size; ++i) {
        indices[i] = i;
    }

    // Shuffle indices using Fisher-Yates algorithm
    mt19937_64 rng(1234567); // seed
    for (size_t i = size - 1; i > 0; --i) {
        size_t j = rng() % (i + 1);
        swap(indices[i], indices[j]);
    }

    // Create cyclic chain: each element points to next in shuffled order
    for (size_t i = 0; i < size; ++i) {
        array[indices[i]] = &array[indices[(i + 1) % size]];
    }
}

// Detect L1 cache size by measuring access time for different array sizes
size_t find_l1_cache_size() {
    cout << "Detecting L1 cache size..." << endl;

    // Test array sizes from 1KB to 512KB (powers of two)
    vector<size_t> sizes_kb = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    vector<double> access_times;

    for (size_t kb : sizes_kb) {
        size_t bytes = kb * 1024;
        size_t elements = max<size_t>(4, bytes / sizeof(void*));

        void** buffer = (void**)allocate_aligned(PAGE_SIZE, max(sizeof(void*) * elements, PAGE_SIZE));
        create_random_chain(buffer, elements);

        // Take multiple measurements
        vector<double> measurements;
        for (int r = 0; r < MEASURE_REPEATS; ++r) {
            measurements.push_back(measure_pointer_chase(buffer));
        }

        double median_time = get_median(measurements);
        cout << kb << " KB: " << median_time << " ns" << endl;
        access_times.push_back(median_time);

        free_aligned(buffer);
    }

    // Find size where access time jumps significantly (50+%)
    size_t detected_size = 0;
    for (size_t i = 1; i < access_times.size(); ++i) {
        if (access_times[i] > access_times[i - 1] * 1.5) {
            detected_size = sizes_kb[i - 1] * 1024;
            break;
        }
    }

    if (detected_size == 0) {
        cout << "--> Could not detect size, using default 32 KB" << endl << endl;
        detected_size = 32 * 1024;
    } else {
        cout << "L1 cache size detected: " << (detected_size / 1024) << " KB" << endl << endl;
    }

    return detected_size;
}

// Detect cache associativity by creating cache conflicts
int find_cache_associativity(size_t line_size, size_t cache_size) {
    cout << "Detecting cache associativity..." << endl;

    size_t step_bytes = cache_size;
    vector<double> access_times;

    // Test different numbers of conflicting addresses
    for (int conflicts = 1; conflicts <= 16; ++conflicts) {
        size_t needed_elements = (size_t)conflicts * (step_bytes / sizeof(void*)) + 16;
        vector<void*> buffer(needed_elements, nullptr);

        size_t stride_elements = max<size_t>(1, step_bytes / sizeof(void*));
        vector<size_t> indices(conflicts);
        for (int i = 0; i < conflicts; ++i) {
            indices[i] = i * stride_elements;
        }

        // Create random cyclic chain among conflicting elements
        vector<size_t> permutation(conflicts);
        for (int i = 0; i < conflicts; ++i) {
            permutation[i] = i;
        }

        mt19937_64 rng(123456 + conflicts);
        for (int i = conflicts - 1; i > 0; --i) {
            size_t j = rng() % (i + 1);
            swap(permutation[i], permutation[j]);
        }

        for (int i = 0; i < conflicts; ++i) {
            buffer[indices[permutation[i]]] = &buffer[indices[permutation[(i + 1) % conflicts]]];
        }

        // Fill remaining pointers to complete the chain
        for (size_t i = 0; i < needed_elements; ++i) {
            if (buffer[i] == nullptr) {
                buffer[i] = &buffer[(i + 1) % needed_elements];
            }
        }

        vector<double> measurements;
        for (int r = 0; r < MEASURE_REPEATS; ++r) {
            measurements.push_back(measure_pointer_chase((void**)&buffer[indices[0]]));
        }

        double median_time = get_median(measurements);
        access_times.push_back(median_time);
        cout << conflicts << " conflicts: " << median_time << " ns" << endl;
    }

    // Calculate baseline time (first few measurements)
    int baseline_count = min(3, (int)access_times.size());
    vector<double> baseline_times(access_times.begin(), access_times.begin() + baseline_count);
    double baseline_median = get_median(baseline_times);

    // Find where access time increases significantly
    for (size_t i = 1; i < access_times.size(); ++i) {
        if (access_times[i] > baseline_median * 1.5 && access_times[i] > baseline_median + 2.0) {
            cout << "Associativity approximately " << i << " ways" << endl;
            return (int)i;
        }
    }

    // Fallback with relaxed threshold
    for (size_t i = 1; i < access_times.size(); ++i) {
        if (access_times[i] > baseline_median * 1.35) {
            cout << "Associativity " << i << " ways" << endl << endl;
            return (int)i;
        }
    }

    cout << "--> Could not detect, using default 8 ways" << endl << endl;
    return 8;
}

// Thread barrier for synchronization
class ThreadBarrier {
public:
    ThreadBarrier(size_t count) : thread_count(count) {}

    void wait() {
        unique_lock<mutex> lock(mutex_);
        if (--thread_count == 0) {
            condition_.notify_all();
        } else {
            condition_.wait(lock, [this] { return thread_count == 0; });
        }
    }

private:
    mutex mutex_;
    condition_variable condition_;
    size_t thread_count;
};

// Thread function for line size detection
unsigned run_thread_function(ThreadBarrier& barrier, unsigned thread_id, int* element) {
    mt19937 rng(thread_id * 1103515245u + 12345u);
    uniform_int_distribution<int> dist(0, 4096);

    barrier.wait();

    int accumulator = 0;
    for (unsigned i = 0; i < FALLBACK_ITERATIONS; ++i) {
        *element = dist(rng);
        accumulator ^= *element;
    }

    return (unsigned)accumulator;
}

// Run multiple threads and measure total execution time
template<typename Func>
double run_multithreaded_test(Func func, unsigned thread_count) {
    ThreadBarrier barrier(thread_count + 1);
    vector<thread> threads;
    vector<future<unsigned>> results;

    // Launch threads
    for (unsigned t = 0; t < thread_count; ++t) {
        packaged_task<unsigned()> task([&, t] { return func(barrier, t); });
        results.push_back(task.get_future());
        threads.emplace_back(move(task));
    }

    auto start_time = steady_clock::now();
    barrier.wait(); // Start all threads simultaneously

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    auto end_time = steady_clock::now();

    // Collect results
    unsigned combined_result = 0;
    for (auto& result : results) {
        combined_result += result.get();
    }

    use_pointer(&combined_result);

    return duration_cast<duration<double>>(end_time - start_time).count();
}

// Test a specific line size candidate using multi-threaded false sharing test
pair<double, double> test_line_size_candidate(size_t candidate_bytes, unsigned thread_count) {
    size_t element_count = 64 * 1024 * 1024 / candidate_bytes;

    // Two buffers: one with natural alignment, one with forced false sharing
    vector<char> naive_buffer(element_count * candidate_bytes, 0);
    vector<char> aligned_buffer(element_count * candidate_bytes, 0);

    struct Element {
        int* pointer;
    };

    vector<Element> naive_view(element_count);
    vector<Element> aligned_view(element_count);

    // Create arrays of pointers to elements
    for (size_t i = 0; i < element_count; ++i) {
        naive_view[i].pointer = reinterpret_cast<int*>(&naive_buffer[i * candidate_bytes]);
        aligned_view[i].pointer = reinterpret_cast<int*>(&aligned_buffer[i * candidate_bytes]);
    }

    // Test function for a given view
    auto test_view = [&](vector<Element>& view) -> double {
        auto thread_func = [&](ThreadBarrier& barrier, unsigned thread_id) -> unsigned {
            size_t index = view.size() / 2 + thread_id;
            return run_thread_function(barrier, thread_id, view[index].pointer);
        };

        double total_time = 0.0;
        for (unsigned trial = 0; trial < FALLBACK_TRIALS; ++trial) {
            total_time += run_multithreaded_test(thread_func, thread_count);
        }
        return total_time / FALLBACK_TRIALS;
    };

    return make_pair(test_view(naive_view), test_view(aligned_view));
}

// Determine cache line size using false sharing detection
size_t find_line_size_with_fallback() {
    cout << "Detecting cache line size using false sharing test..." << endl;

    vector<size_t> candidates = {16, 32, 64, 128};
    unsigned hardware_threads = thread::hardware_concurrency();
    if (hardware_threads == 0) hardware_threads = 4;
    if (hardware_threads > 16) hardware_threads = 16;

    struct TestResult {
        size_t candidate;
        double naive_time;
        double aligned_time;
        double ratio;
    };

    vector<TestResult> results;

    // Test each candidate line size
    for (size_t candidate : candidates) {
        cout << "Testing " << candidate << " bytes... ";
        auto times = test_line_size_candidate(candidate, hardware_threads);
        double ratio = times.first / max(1e-12, times.second);

        cout << "naive: " << times.first << "s, aligned: " << times.second
             << "s, ratio: " << ratio << endl;

        results.push_back({candidate, times.first, times.second, ratio});
    }

    // Select candidate with highest ratio (most false sharing effect)
    double best_ratio = 0;
    size_t best_candidate = 0;

    for (const auto& result : results) {
        if (result.ratio > best_ratio) {
            best_ratio = result.ratio;
            best_candidate = result.candidate;
        }
    }

    cout << "Best ratio: " << best_ratio << " -> selecting " << best_candidate << " bytes" << endl << endl;
    return best_candidate;
}

int main() {
    cout << "L1 Cache Characteristics Detection" << endl;

    // Allocate main buffer
    void* buffer = allocate_aligned(PAGE_SIZE, BUFFER_SIZE);
    if (!buffer) {
        cerr << "Failed to allocate main buffer" << endl;
        return 1;
    }

    // Fill buffer with pattern
    memset(buffer, 0x5A, BUFFER_SIZE);

    // 1: Determine cache line size using false sharing test
    size_t line_size = find_line_size_with_fallback();

    // 2: Determine L1 cache size
    size_t l1_size = find_l1_cache_size();

    // 3: Determine cache associativity
    int associativity = find_cache_associativity(line_size, l1_size);

    // Print final results
    cout << endl << "=====================" << endl;
    cout << "=  Final results:  =" << endl;
    cout << "Cache line size: " << line_size << " bytes" << endl;
    cout << "L1 cache size: " << (l1_size / 1024) << " KB" << endl;
    cout << "Associativity: " << associativity << " ways" << endl;

    // Clean up
    free_aligned(buffer);

    return 0;
}
