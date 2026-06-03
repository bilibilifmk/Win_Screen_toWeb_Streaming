@echo off
cd /d e:\反反截屏测试
C:\msys64\mingw32\bin\gcc.exe -Wall -O2 -D_WIN32_WINNT=0x0601 -o DeAntiCapture_new.exe src\main.c -lpsapi -luser32 -lkernel32 -lgdi32 2>&1
echo EXIT_CODE=%ERRORLEVEL%