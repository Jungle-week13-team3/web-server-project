CC      = gcc
CFLAGS  = -Wall -Wextra -g
TARGET  = server
SRCS    = $(wildcard src/*.c)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

.PHONY: clean