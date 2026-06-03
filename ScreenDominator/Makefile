
CC = gcc
CFLAGS = -Wall -O2 -m32 -D_WIN32_WINNT=0x0601
LIBS = -lpsapi -luser32 -lkernel32 -lgdi32
TARGET = DeAntiCapture
SOURCES = src/main.c
OBJECTS = $(SOURCES:.c=.o)

# Default target
all: $(TARGET).exe

# Link the executable
$(TARGET).exe: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Compile source files
src/%.o: src/%.c
	@if not exist src\$(null) mkdir src
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	-del /Q src\main.o 2>nul
	-del /Q $(TARGET).exe 2>nul

# Rebuild target
rebuild: clean all

.PHONY: all clean rebuild