.PHONY: all build start stop restart status run logs test clean

all: build

build:
	mkdir -p build
	cmake -B build -S .
	cmake --build build -j$$(nproc)

start:
	./bin/morfo start

stop:
	./bin/morfo stop

restart:
	./bin/morfo restart

status:
	./bin/morfo status

run:
	./bin/morfo run

logs:
	./bin/morfo logs

test:
	ctest --test-dir build --output-on-failure

clean:
	rm -rf build
