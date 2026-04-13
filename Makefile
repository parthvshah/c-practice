CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -g

TARGET  = main
SRCS    = main.c ring_buffer.c parse_mem.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

main.o: main.c ring_buffer.h parse_mem.h
	$(CC) $(CFLAGS) -c -o $@ $<

ring_buffer.o: ring_buffer.c ring_buffer.h
	$(CC) $(CFLAGS) -c -o $@ $<

parse_mem.o: parse_mem.c parse_mem.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
