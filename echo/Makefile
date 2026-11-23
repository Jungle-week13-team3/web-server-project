CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2
LDFLAGS =

PROGS = hostinfo echoclient echoserveri

.PHONY: all asan test clean

all: $(PROGS)

hostinfo: hostinfo.o csapp.o
	$(CC) $(LDFLAGS) -o $@ $^

echoclient: echoclient.o csapp.o
	$(CC) $(LDFLAGS) -o $@ $^

echoserveri: echoserveri.o echo.o csapp.o
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c csapp.h
	$(CC) $(CFLAGS) -c $<

# AddressSanitizer·UndefinedBehaviorSanitizer 를 붙여 다시 빌드한다.
# 메모리 오류 검사: make asan test
asan:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" \
	            LDFLAGS="-fsanitize=address,undefined"

test: all
	bash ./test_echo.sh

clean:
	rm -f $(PROGS) *.o
