@echo off
setlocal EnableExtensions
where cl.exe >nul 2>&1
if %errorlevel% neq 0 (
  call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  if %errorlevel% neq 0 call "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
)
if not exist bin mkdir bin
cl /nologo /TC /O2 /W4 src\FseSwitch.c /Fe:bin\FseSwitch.exe /Fo:bin\ /link windowsapp.lib onecore.lib wtsapi32.lib userenv.lib user32.lib
if %errorlevel% neq 0 exit /b %errorlevel%
echo Built bin\FseSwitch.exe (pure C)
bin\FseSwitch.exe status
