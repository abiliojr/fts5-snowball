SQLITE_FLAGS=`pkg-config --cflags --silence-errors sqlite3`

all: libstemmer fts5stemmer.o
	cc -shared -o fts5stemmer.so fts5stemmer.o snowball/libstemmer.o
	@[ "`uname -s`" = "Darwin" ] && mv fts5stemmer.so fts5stemmer.dylib || :

libstemmer:
	@if [ ! -f snowball/GNUmakefile ];then \
		echo -e "\n\nError: snowball is missing! Please put it's source code in a directory called 'snowball'"; \
		echo -e "Head to https://github.com/snowballstem/snowball to fetch it\n"; \
		exit 2; \
	fi

	$(MAKE) -C snowball

fts5stemmer.o: src/fts5stemmer.c
	cc -Wall -c $(SQLITE_FLAGS) -Isnowball/include -Isqlite -O3 src/fts5stemmer.c

clean:
	@rm *.o
	$(MAKE) -C snowball clean
