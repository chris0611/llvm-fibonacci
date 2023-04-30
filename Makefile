CC = clang
CFLAGS = -g -O3 -Wall -Wextra -Wpedantic `llvm-config --cflags --ldflags --libs --system-libs orcjit core native`

all: fibonacci

%: %.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f fibonacci fib.ll fib.s