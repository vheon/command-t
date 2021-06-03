CC=gcc
CFLAGS=-Wall -Werror -fpic
COVER=#--coverage

all: build/commandt.so

build/commandt.so: src/commandt.c
	mkdir -pv build
	$(CC) -O3 $(CFLAGS) -shared src/commandt.c -o build/commandt.so

.PHONY: clean

clean:
	rm -rf build
