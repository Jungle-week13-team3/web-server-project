CC      = gcc
CFLAGS  = -Wall -Wextra -g
TARGET  = server
SRCS    = $(wildcard src/*.c)

HDRS    = $(wildcard src/*.h)

# 헤더(.h)가 바뀌어도 다시 빌드한다. 컴파일에는 .c만 넘긴다.
$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: clean