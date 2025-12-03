all: build
	./cache_analyzer
build:
	g++ -O3 -march=native -std=c++20 cache_analyzer.cpp -pthread -o cache_analyzer
clean:
	$(RM) cache_analyzer
