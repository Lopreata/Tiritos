@echo off
rem Teste do combate sem jogar: gera tools\sim_test\resultado.txt (log) e imagens .bmp de momentos importantes.
cd /d "%~dp0\.."
if not exist tools\sim_test mkdir tools\sim_test
start /wait "" Tiritos.exe --sim-test tools\sim_test\resultado.txt
type tools\sim_test\resultado.txt
