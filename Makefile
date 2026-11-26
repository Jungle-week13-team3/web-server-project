CC      = gcc
CFLAGS  = -Wall -Wextra -g
LDLIBS  = -pthread # 스레드(pthread) 라이브러리. CFLAGS를 바꿔 빌드해도 빠지지 않게 따로 둔다
TARGET  = server
SRCS    = $(wildcard src/*.c)
HDRS    = $(wildcard src/*.h)

# 서버가 실행하는 테스트용 CGI 프로그램
CGI_ADDER = cgi-bin/adder

# 단위 테스트: 소켓 없이 http.c·static.c·cgi.c 함수만 불러 본다
TEST_HTTP   = tests/unit/test_http
TEST_STATIC = tests/unit/test_static
TEST_CGI    = tests/unit/test_cgi
TEST_PROXY  = tests/unit/test_proxy
TEST_POOL   = tests/unit/test_pool

# make만 치면 서버와 CGI 프로그램을 같이 만든다
all: $(TARGET) $(CGI_ADDER)

# 헤더(.h)가 바뀌어도 다시 빌드한다. 컴파일에는 .c만 넘긴다.
$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(CGI_ADDER): cgi-bin/adder.c
	$(CC) $(CFLAGS) -o $@ $<

$(TEST_HTTP): tests/unit/test_http.c src/http.c src/http.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/unit/test_http.c src/http.c

# static.c가 http.c(응답 머리)와 net.c(write_all)를 부르므로 같이 링크한다
$(TEST_STATIC): tests/unit/test_static.c src/static.c src/static.h src/http.c src/net.c
	$(CC) $(CFLAGS) -Isrc -o $@ tests/unit/test_static.c src/static.c src/http.c src/net.c

$(TEST_CGI): tests/unit/test_cgi.c src/cgi.c src/cgi.h src/http.c src/net.c
	$(CC) $(CFLAGS) -Isrc -o $@ tests/unit/test_cgi.c src/cgi.c src/http.c src/net.c

$(TEST_PROXY): tests/unit/test_proxy.c src/proxy.c src/proxy.h src/http.c src/net.c
	$(CC) $(CFLAGS) -Isrc -o $@ tests/unit/test_proxy.c src/proxy.c src/http.c src/net.c $(LDLIBS)

$(TEST_POOL): tests/unit/test_pool.c src/pool.c src/pool.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/unit/test_pool.c src/pool.c $(LDLIBS)

test: $(TEST_HTTP) $(TEST_STATIC) $(TEST_CGI) $(TEST_PROXY) $(TEST_POOL)
	./$(TEST_HTTP)
	./$(TEST_STATIC)
	./$(TEST_CGI)
	./$(TEST_PROXY)
	./$(TEST_POOL)

clean:
	rm -f $(TARGET) $(CGI_ADDER) $(TEST_HTTP) $(TEST_STATIC) $(TEST_CGI) $(TEST_PROXY) $(TEST_POOL)

# 메모리 검사: AddressSanitizer(잘못된 메모리 접근·누수) + UBSan(정의되지 않은 동작)으로 다시 빌드해
# 단위·기능 테스트를 전부 돌리고, 서버 로그에 검사기 보고가 하나라도 있으면 실패한다.
# 끝나면 검사용 실행 파일을 지운다(다음 make는 일반 빌드).
ASAN_FLAGS = -Wall -Wextra -g -fno-omit-frame-pointer -fsanitize=address,undefined
SERVER_LOGS = /tmp/web-server-test.log /tmp/web-backend1.log /tmp/web-backend2.log /tmp/web-shutdown.log
asan:
	$(MAKE) clean
	$(MAKE) all $(TEST_HTTP) $(TEST_STATIC) $(TEST_CGI) $(TEST_PROXY) $(TEST_POOL) CFLAGS="$(ASAN_FLAGS)"
	./$(TEST_HTTP) && ./$(TEST_STATIC) && ./$(TEST_CGI) && ./$(TEST_PROXY) && ./$(TEST_POOL)
	bash tests/integration.sh
	@if grep -l 'Sanitizer\|runtime error' $(SERVER_LOGS); then \
		echo "ASan: 위 로그에 검사기 보고가 있다"; exit 1; \
	else echo "ASan: 메모리 오류·누수 보고 0건"; fi
	$(MAKE) clean

.PHONY: all clean test asan
