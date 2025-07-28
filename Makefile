CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2
LIBS = -lX11 -lXext -lasound -lm -lcairo -lXrandr

TARGET = interval_timer
SOURCE = interval_timer.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/

uninstall:
	sudo rm -f /usr/local/bin/$(TARGET) 