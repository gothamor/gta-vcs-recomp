@echo off
rem Sets up an MSVC x64 developer environment and runs the given command.
rem Usage: tools\devenv.cmd <command...>
rem Picks the newest Visual Studio that has the C++ toolset.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo error: no Visual Studio with the C++ x64 toolset found
  exit /b 1
)
echo [devenv] %VSROOT%
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
rem Prefer standalone CMake/Ninja/LLVM/JDK from scoop when present.
if exist "%USERPROFILE%\scoop\shims" set "PATH=%USERPROFILE%\scoop\shims;%PATH%"
if exist "%USERPROFILE%\scoop\apps\llvm\current\bin" set "PATH=%USERPROFILE%\scoop\apps\llvm\current\bin;%PATH%"
if exist "%USERPROFILE%\scoop\apps\temurin21-jdk\current" (
  set "JAVA_HOME=%USERPROFILE%\scoop\apps\temurin21-jdk\current"
  set "PATH=%USERPROFILE%\scoop\apps\temurin21-jdk\current\bin;%PATH%"
)
if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin" set "PATH=%PATH%;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if exist "%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja" set "PATH=%PATH%;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
%*
