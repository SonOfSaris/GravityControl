@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist ..\out mkdir ..\out
cl /nologo /O2 /W3 /EHsc /std:c++17 /MT chordtest.cpp /Fo:..\out\ /Fe:..\out\chordtest.exe
exit /b %errorlevel%
