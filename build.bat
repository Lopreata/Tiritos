@echo off
rem Compila o Tiritos (main.cpp + icone/versao do game.rc) com MinGW (g++) ou, se nao houver, com o MSVC (cl).
rem Resultado: Tiritos.exe nesta pasta.
cd /d "%~dp0"

where g++ >nul 2>nul
if %errorlevel%==0 goto mingw

rem o MinGW instalado pelo winget nem sempre esta no PATH de um terminal ja aberto
for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs*") do (
    if exist "%%d\mingw64\bin\g++.exe" set "PATH=%%d\mingw64\bin;%PATH%"
)
where g++ >nul 2>nul
if %errorlevel%==0 goto mingw

where cl >nul 2>nul
if %errorlevel%==0 (
    rc /nologo /fo game.res game.rc && cl /nologo /O2 /EHsc main.cpp game.res /Fe:Tiritos.exe /link /SUBSYSTEM:WINDOWS
    goto done
)

echo Nenhum compilador C++ encontrado (g++ ou cl).
exit /b 1

:mingw
windres game.rc -O coff -o game_res.o
if errorlevel 1 goto done
rem -static: o Tiritos.exe nao depende das DLLs do MinGW e roda em qualquer Windows
g++ -O2 -std=c++11 -mwindows -static main.cpp game_res.o -o Tiritos.exe -lopengl32 -lgdi32 -luser32 -lws2_32

:done
if errorlevel 1 (
    echo ERRO: a compilacao falhou.
    exit /b 1
)
if exist game_res.o del game_res.o
echo OK: execute Tiritos.exe
