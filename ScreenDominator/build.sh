#!/bin/bash
/mingw32/bin/gcc.exe -Wall -O2 -D_WIN32_WINNT=0x0601 -E src/main.c -o /tmp/preprocessed.i 2>&1
echo "PREPROCESS_EXIT=$?"
/mingw32/bin/gcc.exe -Wall -O2 -D_WIN32_WINNT=0x0601 -c src/main.c -o /tmp/main.o 2>&1
echo "COMPILE_EXIT=$?"