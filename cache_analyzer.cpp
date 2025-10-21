#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <bitset>

class MemoryAllocator {
private:
    std::vector<std::pair<void*, size_t>> allocated_regions;
    size_t page_size;

public:
    MemoryAllocator() : page_size(getpagesize()) {}

    ~MemoryAllocator() {
        cleanup();
    }

    void* allocate_aligned(size_t size, size_t alignment) {
        if (size == 0) throw std::invalid_argument("Size cannot be zero");

        // Выделяем с запасом для выравнивания
        size_t total_size = size + alignment + page_size;

        void* ptr = mmap(nullptr, total_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (ptr == MAP_FAILED) throw std::runtime_error("mmap failed");

        // Выравниваем на границу
        uintptr_t aligned_ptr = (reinterpret_cast<uintptr_t>(ptr) + alignment - 1) & ~(alignment - 1);
        aligned_ptr = (aligned_ptr + page_size - 1) & ~(page_size - 1);

        allocated_regions.push_back({ptr, total_size});
        return reinterpret_cast<void*>(aligned_ptr);
    }

    void cleanup() {
        for (auto& [ptr, size] : allocated_regions) {
            munmap(ptr, size);
        }
        allocated_regions.clear();
    }
};

// Функция измерения времени
double measure_access_time(char* array, size_t size, size_t stride) {
    volatile char temp;
    const size_t iterations = 100000;

    // Прогрев кэша
    for (size_t i = 0; i < size; i += stride) {
        temp = array[i];
    }

    auto start = std::chrono::steady_clock::now();

    // Основные измерения с защитой от оптимизации
    for (size_t i = 0; i < iterations; i++) {
        size_t index = (i * stride) % size;
        temp = array[index];
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    // Принудительно используем temp
    if (temp == 0) {
        asm volatile("" : : "r"(temp) : "memory");
    }

    return duration.count() / static_cast<double>(iterations);
}

// Определение размера кэш-линии
int find_cache_line_size(MemoryAllocator& allocator) {
    const size_t ARRAY_SIZE = 1024 * 1024; // 1MB
    const int ITERATIONS = 100;

    std::cout << std::endl << "Определение размера кэш-линии..." << std::endl;

    char* array = static_cast<char*>(allocator.allocate_aligned(ARRAY_SIZE, 64));

    // Инициализация
    for (size_t i = 0; i < ARRAY_SIZE; i++) {
        array[i] = i % 256;
    }

    // Шаги для прохода по массиву. При маленьких шагах (1, 2, 4 байта) должны обращаться к одной кэш-линии
    std::vector<int> test_strides = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    std::vector<double> times;

    for (int stride : test_strides) {
        double total_time = 0;

        for (int iter = 0; iter < ITERATIONS; iter++) {
            total_time += measure_access_time(array, ARRAY_SIZE, stride);
        }

        double avg_time = total_time / ITERATIONS;
        times.push_back(avg_time);
        std::cout << "  Шаг: " << stride << " байт, Время: " << avg_time << " нс" << std::endl;
    }

    // Ищем точку, где время начинает расти (размер кэш-линии)
    // Когда шаг становится равен размеру кэш-линии, каждое обращение попадает в новую линию
    // Время минимально, когда шаг оптимально использует кэш-линию
    int cache_line_size = -1; // значение по умолчанию
    for (size_t i = 1; i < times.size(); i++) {
        if (times[i] > times[i-1] * 1.3) {
            cache_line_size = test_strides[i];
            break;
        }
    }

    std::cout << "==> Определенный размер кэш-линии: " << cache_line_size << " байт" << std::endl;
    return cache_line_size;
}

// Определение объема кэша
int find_cache_size(MemoryAllocator& allocator) {
    std::cout << std::endl << "Определение объема кэша..." << std::endl;

    // Тестируемые размеры (типичные для современных процессоров)
    std::vector<int> test_sizes_kb = {8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024};
    std::vector<double> times;

    for (int size_kb : test_sizes_kb) {
        int size_bytes = size_kb * 1024;
        char* array = static_cast<char*>(allocator.allocate_aligned(size_bytes, 64));

        // Инициализация
        for (int i = 0; i < size_bytes; i++) {
            array[i] = i % 256;
        }

        // Измеряем время последовательного доступа
        volatile char temp;
        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < size_bytes; i += 64) { // шаг = размер кэш-линии
            temp = array[i];
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

        double time_per_access = duration.count() / static_cast<double>(size_bytes / 64);
        times.push_back(time_per_access);

        std::cout << "  Размер: " << size_kb << " KB, Время: " << time_per_access << " нс" << std::endl;

        // Используем temp
        if (temp == 0) asm volatile("" : : "r"(temp) : "memory");
    }

    // Анализ: ищем точку, где время резко возрастает (20%)
    int l1_cache_size = -1; // значение по умолчанию
    double max_increase = 0;

    for (size_t i = 1; i < times.size(); i++) {
        double increase = times[i] / times[i-1];
        if (increase > max_increase && increase > 1.2) {
            max_increase = increase;
            l1_cache_size = test_sizes_kb[i-1] * 1024;
        }
    }

    std::cout << "==> Определенный объем L1 кэша: " << l1_cache_size / 1024 << " KB" << std::endl;
    return l1_cache_size;
}

// Определение ассоциативности
int find_associativity(MemoryAllocator& allocator, int cache_size, int cache_line_size) {
    std::cout << std::endl << "Определение ассоциативности..." << std::endl;

    const int MAX_WAYS = 16;
    const int SET_COUNT = cache_size / (cache_line_size * MAX_WAYS);

    std::vector<char*> arrays;

    // Создаем массивы, которые будут конфликтовать в кэше
    for (int i = 0; i < MAX_WAYS; i++) {
        char* arr = static_cast<char*>(allocator.allocate_aligned(cache_size * 2, cache_line_size));
        arrays.push_back(arr);
    }

    std::vector<double> times;

    for (int ways = 1; ways <= MAX_WAYS; ways++) {
        volatile char temp;
        auto start = std::chrono::steady_clock::now();

        // Доступ к конфликтующим адресам
        for (int set = 0; set < SET_COUNT; set++) {
            for (int way = 0; way < ways; way++) {
                int offset = set * cache_line_size * MAX_WAYS + way * cache_line_size;
                temp = arrays[way][offset];
            }
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        double time_per_access = duration.count() / static_cast<double>(SET_COUNT * ways);

        times.push_back(time_per_access);
        std::cout << "  Путей: " << ways << ", Время: " << time_per_access << " нс" << std::endl;

        if (temp == 0) asm volatile("" : : "r"(temp) : "memory");
    }

    // Анализ: ищем точку, где время резко возрастает (более чем на 30%)
    int associativity = 8; // значение по умолчанию
    for (int i = 2; i < MAX_WAYS; i++) {
        if (times[i] > times[i-1] * 1.3) {
            associativity = i;
            break;
        }
    }

    std::cout << "==> Определенная ассоциативность: " << associativity << "-way" << std::endl;
    return associativity;
}

// Функция для проверки, является ли число степенью двойки
bool is_power_of_two(int n) {
    return (n > 0) && ((n & (n - 1)) == 0);
}

// Функция проверки согласованности параметров кэша
void verify_cache_consistency(int cache_size, int cache_line_size, int associativity) {
    std::cout << std::endl << "ПРОВЕРКА СОГЛАСОВАННОСТИ ПАРАМЕТРОВ КЭША" << std::endl;
    std::cout << "==========================================" << std::endl;

    // Проверяем основные предположения
    std::cout << "1. Проверка основных свойств:" << std::endl;

    bool size_ok = is_power_of_two(cache_size);
    bool line_size_ok = is_power_of_two(cache_line_size);
    bool assoc_ok = is_power_of_two(associativity) || (associativity == 1);

    std::cout << "   - Размер кэша " << cache_size << " байт - "
              << (size_ok ? "степень двойки True" : "НЕ степень двойки False") << std::endl;
    std::cout << "   - Размер линии " << cache_line_size << " байт - "
              << (line_size_ok ? "степень двойки True" : "НЕ степень двойки False") << std::endl;
    std::cout << "   - Ассоциативность " << associativity << "-way - "
              << (assoc_ok ? "корректная True" : "проблема False") << std::endl;

    // Основная формула проверки
    std::cout << std::endl << "2. Проверка по формуле из Hennessy & Patterson:" << std::endl;
    std::cout << "   2^index = Cache_size / (Block_size × Associativity)" << std::endl;

    double sets = static_cast<double>(cache_size) / (cache_line_size * associativity);
    std::cout << "   Вычислено: " << cache_size << " / (" << cache_line_size
              << " × " << associativity << ") = " << sets << " наборов" << std::endl;

    // Проверяем, является ли количество наборов степенью двойки
    bool sets_ok = false;
    int index_bits = -1;
    int actual_sets = static_cast<int>(sets + 0.5); // Округляем

    if (std::abs(sets - actual_sets) < 0.01) { // Проверяем целочисленность
        sets_ok = is_power_of_two(actual_sets);
        if (sets_ok) {
            index_bits = static_cast<int>(std::log2(actual_sets));
        }
    }

    if (sets_ok) {
        std::cout << "==> Количество наборов (" << actual_sets << ") = 2^"
                  << index_bits << " - СООТВЕТСТВУЕТ ФОРМУЛЕ" << std::endl;

        // Дополнительная информация о структуре кэша
        std::cout << std::endl << "3. Структура кэша:" << std::endl;
        std::cout << "   - Всего наборов: " << actual_sets << std::endl;
        std::cout << "   - Битов индекса: " << index_bits << std::endl;
        std::cout << "   - Битов смещения: " << static_cast<int>(std::log2(cache_line_size)) << std::endl;

        // Вычисляем биты тега для 32-битной архитектуры
        int offset_bits = static_cast<int>(std::log2(cache_line_size));
        int tag_bits = 32 - index_bits - offset_bits;
        if (tag_bits > 0) {
            std::cout << "   - Битов тега (для 32-бит): " << tag_bits << std::endl;
        }

    } else {
        std::cout << "==> ПРОБЛЕМА: количество наборов (" << sets
                  << ") не является степенью двойки" << std::endl;
        std::cout << "   Возможные причины:" << std::endl;
        std::cout << "   - Погрешности измерений" << std::endl;
        std::cout << "   - Нестандартная архитектура кэша" << std::endl;
        std::cout << "   - Ошибка в определении одного из параметров" << std::endl;

        // Находим ближайшую степень двойки для диагностики
        int suggested_sets = 1;
        while (suggested_sets * 2 <= actual_sets) {
            suggested_sets *= 2;
        }

        std::cout << "   Ближайшая степень двойки: " << suggested_sets << std::endl;
        std::cout << "   Рекомендуемые корректировки:" << std::endl;
        std::cout << "   - Размер кэша: " << suggested_sets * cache_line_size * associativity << " байт" << std::endl;
        std::cout << "   - Ассоциативность: " << cache_size / (cache_line_size * suggested_sets) << "-way" << std::endl;
    }

    // Проверка общей формулы
    std::cout << std::endl << "4. Проверка общей формулы Cache_size = Sets × Line_size × Associativity:" << std::endl;
    int calculated_size = actual_sets * cache_line_size * associativity;
    if (calculated_size == cache_size) {
        std::cout << actual_sets << " × " << cache_line_size << " × "
                  << associativity << " = " << calculated_size << " - ВЕРНО" << std::endl;
    } else {
        std::cout << actual_sets << " × " << cache_line_size << " × "
                  << associativity << " = " << calculated_size << " ≠ " << cache_size << std::endl;
    }
}

int main() {
    std::cout << "АНАЛИЗАТОР ХАРАКТЕРИСТИК КЭША L1" << std::endl;

    MemoryAllocator allocator;

    // Используем улучшенные методы
    int cache_line_size = find_cache_line_size(allocator);
    int cache_size = find_cache_size(allocator);
    int associativity = find_associativity(allocator, cache_size, cache_line_size);

    // Вывод результатов
    std::cout << std::endl << "ИТОГОВЫЕ РЕЗУЛЬТАТЫ:" << std::endl;
    std::cout << "Размер кэш-линии: " << cache_line_size << " байт" << std::endl;
    std::cout << "Ассоциативность: " << associativity << "-way" << std::endl;
    std::cout << "Объем L1 кэша: " << cache_size / 1024 << " KB" << std::endl;

    // Проверка результатов
    verify_cache_consistency(cache_size, cache_line_size, associativity);

    return 0;
}
