@echo off
rem Gera a versao para distribuir em dist\:
rem   Tiritos_Setup.exe       instalador (um arquivo so, com o jogo dentro)
rem   Tiritos_portatil.zip    o jogo pronto para extrair e jogar, sem instalar
rem   Tiritos\                a mesma pasta do zip
cd /d "%~dp0\.."

rem caminhos com .\ : funciona mesmo quando o cmd nao procura programas na pasta atual
call .\build.bat
if errorlevel 1 exit /b 1

echo.
echo Compilando o instalador...
windres tools\setup\setup.rc -O coff -o tools\setup\setup_res.o
if errorlevel 1 goto erro
g++ -O2 -std=c++11 -municode -mwindows -static tools\setup\setup.cpp tools\setup\setup_res.o -o tools\setup\setup_stub.exe -lcabinet -lole32 -luuid -lshell32 -ladvapi32
if errorlevel 1 goto erro
g++ -O2 -std=c++11 -static tools\setup\pack.cpp -o tools\setup\pack.exe -lcabinet
if errorlevel 1 goto erro
del tools\setup\setup_res.o

echo.
echo Montando a pasta dist\Tiritos...
if exist dist rmdir /s /q dist
mkdir dist\Tiritos\assets
copy /y Tiritos.exe dist\Tiritos\ >nul
copy /y assets\male.mdl dist\Tiritos\assets\ >nul
copy /y assets\female.mdl dist\Tiritos\assets\ >nul
copy /y tools\setup\LEIA-ME.txt dist\Tiritos\ >nul
copy /y assets\raw\License_Standard.txt dist\Tiritos\licenca-modelos.txt >nul

echo.
echo Gerando o instalador...
.\tools\setup\pack.exe tools\setup\setup_stub.exe dist\Tiritos_Setup.exe ^
  dist\Tiritos\Tiritos.exe Tiritos.exe ^
  dist\Tiritos\assets\male.mdl assets\male.mdl ^
  dist\Tiritos\assets\female.mdl assets\female.mdl ^
  dist\Tiritos\LEIA-ME.txt LEIA-ME.txt ^
  dist\Tiritos\licenca-modelos.txt licenca-modelos.txt
if errorlevel 1 goto erro

echo.
echo Gerando a versao portatil (zip)...
powershell -NoProfile -Command "Compress-Archive -Path 'dist\Tiritos' -DestinationPath 'dist\Tiritos_portatil.zip' -Force"
if errorlevel 1 goto erro

echo.
echo Pronto. Arquivos em dist\:
dir /b dist
exit /b 0

:erro
echo ERRO: nao foi possivel gerar a versao para distribuir.
exit /b 1
