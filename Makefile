all: build
	./cache_analyzer
build:
	g++ -O3 -o cache_analyzer cache_analyzer.cpp
clean:
	$(RM) cache_analyzer
