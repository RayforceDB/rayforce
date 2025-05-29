@echo off
SETLOCAL

:: Set these variables to match your environment
SET TARGET=rayforce
SET DEF_NAME=%TARGET%.def
SET LIB_NAME=%TARGET%.lib
SET VS_DIR=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\

:: Setup Visual Studio environment variables
CALL "%VS_DIR%\VsDevCmd.bat"

:: Generate the .lib file
lib /def:%DEF_NAME% /out:%LIB_NAME% /machine:x64

cl /LD example.c rayforce.lib /I"../core/" /link /out:example.dll

ENDLOCAL
