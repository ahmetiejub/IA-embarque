@echo off
setlocal enabledelayedexpansion
REM ===================================================================
REM  Projet AMR & EJUB - Ecrit le FSBL et l'application dans la flash
REM  externe de la NUCLEO-N657X0-Q pour un demarrage autonome.
REM  Prerequis : carte en mode DEVELOPPEMENT (BOOT1 en position 2-3),
REM  FSBL et Appli compiles dans CubeIDE, aucune session de debug ouverte.
REM ===================================================================
cd /d "%~dp0"

set FSBL_BIN=..\FSBL\Debug\amr-ejub_FSBL.bin
set APPLI_BIN=..\Appli\Debug\amr-ejub_Appli.bin

if not exist "%FSBL_BIN%"  ( echo [ERREUR] %FSBL_BIN% introuvable : compile amr-ejub_FSBL dans CubeIDE. & goto fin )
if not exist "%APPLI_BIN%" ( echo [ERREUR] %APPLI_BIN% introuvable : compile amr-ejub_Appli dans CubeIDE. & goto fin )

REM --- Recherche des outils ST (STM32CubeProgrammer, ou celui integre a CubeIDE) ---
set TOOLS=
for %%D in ("C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin" "C:\Program Files (x86)\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin") do (
  if exist "%%~D\STM32_SigningTool_CLI.exe" if not defined TOOLS set "TOOLS=%%~D"
)
if not defined TOOLS (
  for /d %%I in ("C:\ST\STM32CubeIDE_*") do for /d %%P in ("%%I\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*") do (
    if exist "%%P\tools\bin\STM32_SigningTool_CLI.exe" if not defined TOOLS set "TOOLS=%%P\tools\bin"
  )
)
if not defined TOOLS (
  echo [ERREUR] Outils ST introuvables. Installe STM32CubeProgrammer ^(gratuit, site st.com^),
  echo          ou indique le dossier "bin" a la main dans ce fichier ^(variable TOOLS^).
  goto fin
)
echo Outils ST : %TOOLS%
set SIGN="%TOOLS%\STM32_SigningTool_CLI.exe"
set PROG="%TOOLS%\STM32_Programmer_CLI.exe"
set LOADER=%TOOLS%\ExternalLoader\MX25UM51245G_STM32N6570-NUCLEO.stldr
if not exist "%LOADER%" ( echo [ERREUR] Chargeur de flash introuvable : %LOADER% & goto fin )

echo.
echo === 1/3 Signature (en-tete exige par la puce au demarrage) ===
if exist FSBL-trusted.bin del FSBL-trusted.bin
if exist Appli-trusted.bin del Appli-trusted.bin
%SIGN% -bin "%FSBL_BIN%" -nk -of 0x80000000 -t fsbl -o FSBL-trusted.bin -hv 2.3 -align
if errorlevel 1 %SIGN% -bin "%FSBL_BIN%" -nk -of 0x80000000 -t fsbl -o FSBL-trusted.bin -hv 2.3
if not exist FSBL-trusted.bin ( echo [ERREUR] Signature du FSBL & goto fin )
%SIGN% -bin "%APPLI_BIN%" -nk -of 0x80000000 -t fsbl -o Appli-trusted.bin -hv 2.3 -align
if errorlevel 1 %SIGN% -bin "%APPLI_BIN%" -nk -of 0x80000000 -t fsbl -o Appli-trusted.bin -hv 2.3
if not exist Appli-trusted.bin ( echo [ERREUR] Signature de l'application & goto fin )

echo.
echo === 2/3 Ecriture dans la flash externe ===
echo     FSBL  -^> 0x70000000      Application -^> 0x70100000
%PROG% -c port=SWD mode=HOTPLUG -el "%LOADER%" -hardRst -w FSBL-trusted.bin 0x70000000 -w Appli-trusted.bin 0x70100000
if errorlevel 1 ( echo [ERREUR] Ecriture : carte en mode developpement ? session de debug CubeIDE fermee ? & goto fin )

echo.
echo === 3/3 Termine ===
echo  1. Debranche la carte.
echo  2. Mets les interrupteurs en "demarrage sur flash" : BOOT0 en 1-2, BOOT1 en 1-2.
echo  3. Rebranche : la carte demarre toute seule.
echo  Pour revenir au mode CubeIDE : BOOT1 en 2-3.
:fin
echo.
pause
