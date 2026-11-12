@echo off
rem Teste do ciclo de andar/correr: gera tools\anim_test\resultado.txt (metricas) e resultado.bmp (quadros de lado).
cd /d "%~dp0\.."
if not exist tools\anim_test mkdir tools\anim_test
Tiritos.exe --anim-test "%CD%\tools\anim_test\resultado"
type tools\anim_test\resultado.txt
