CC=gcc
CFLAGS=-g -Wall
LDLIBS=-lncurses
OBJS=main.o
TARGET=codein

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
