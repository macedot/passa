CC = gcc
CFLAGS = -O3 -march=native -mtune=native -flto \
	 -fipa-pta -fdevirtualize-at-ltrans -fuse-linker-plugin \
	 -fno-plt -fno-semantic-interposition -fomit-frame-pointer \
	 -ffunction-sections -fdata-sections \
	 -D_GNU_SOURCE -Wall -Wextra
LDFLAGS = -luring -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed

TARGET = passa
SRCS = main.c simd.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
