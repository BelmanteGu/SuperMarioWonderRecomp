#!/usr/bin/env python3
"""Pack a ready-to-run Wonder for another Windows PC of the owner's (docs/nvn/WINDOWS-PLAN.md, "Another PC").

The zip holds the built game and its data, nothing to install or build there:

    WonderPC/wonder.exe                      the build (private/wonder/build)
    WonderPC/msvcp140.dll, vcruntime140*.dll the C++ runtime, app-local (Visual Studio's redistributable files)
    WonderPC/Jogar.bat                       sets NX_DATA_ROOT to data\\ and starts the game
    WonderPC/LEIA-ME.txt                     requirements and switches
    WonderPC/data/analysis, extract/romfs, spirv (with results.tsv), fonts, user/save (unless --no-save)

The data comes from the owner's own cartridge: for the owner's machines only, never to be shared.

Usage: pack_pc_portable.py [--root private/wonder] [--out private/wonder/pc/WonderPC-portable.zip] [--no-save]
"""
import argparse
import glob
import os
import sys
import zipfile

TOP = "WonderPC/"
REDIST = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"

JOGAR = r"""@echo off
rem Super Mario Bros. Wonder (Switch recomp) - starts the game with the data next to this file.
cd /d "%~dp0"
set NX_DATA_ROOT=%~dp0data
set NX_LOG=%~dp0data\nx.log
if not defined NX_MEM_LIMIT_GB set NX_MEM_LIMIT_GB=8
if not defined NX_GPU set NX_GPU=discrete
rem The game's language: data\language.txt, written by Language.bat (see README, NX_LANGUAGE).
if exist "%~dp0data\language.txt" set /p NX_LANGUAGE=<"%~dp0data\language.txt"
start "" "%~dp0wonder.exe"
"""

# The language selector: writes data\language.txt, which Jogar.bat applies as NX_LANGUAGE. The codes are the
# console's own (runtime/src/language.h): most are two letters ("es", "ja", ...), not "es-ES".
LANGUAGE = r"""@echo off
rem Super Mario Bros. Wonder (Switch recomp) - choose the game's language, saved to data\language.txt.
chcp 65001 >nul
setlocal
title Super Mario Bros. Wonder - Language
cd /d "%~dp0"
if not exist "%~dp0data" mkdir "%~dp0data"

:menu
cls
echo ====================================================
echo    Super Mario Bros. Wonder  -  Language
echo ====================================================
echo.
echo    Choose the language of the text and the voices:
echo.
echo     1  Spanish (Spain)            es
echo     2  Spanish (Latin America)    es-419
echo     3  English (US)               en-US
echo     4  English (UK)               en-GB
echo     5  French                     fr
echo     6  French (Canada)            fr-CA
echo     7  German                     de
echo     8  Italian                    it
echo     9  Japanese                   ja
echo    10  Portuguese (Portugal)      pt
echo    11  Portuguese (Brazil)        pt-BR
echo    12  Dutch                      nl
echo    13  Russian                    ru
echo    14  Korean                     ko
echo    15  Chinese (Simplified)       zh-Hans
echo    16  Chinese (Traditional)      zh-Hant
echo     0  Exit without changes
echo.
set "opt="
set /p "opt=  Choice [1]: "
if "%opt%"=="" set "opt=1"
if "%opt%"=="0" exit /b 0
if "%opt%"=="1"  set "code=es" & goto setlang
if "%opt%"=="2"  set "code=es-419" & goto setlang
if "%opt%"=="3"  set "code=en-US" & goto setlang
if "%opt%"=="4"  set "code=en-GB" & goto setlang
if "%opt%"=="5"  set "code=fr" & goto setlang
if "%opt%"=="6"  set "code=fr-CA" & goto setlang
if "%opt%"=="7"  set "code=de" & goto setlang
if "%opt%"=="8"  set "code=it" & goto setlang
if "%opt%"=="9"  set "code=ja" & goto setlang
if "%opt%"=="10" set "code=pt" & goto setlang
if "%opt%"=="11" set "code=pt-BR" & goto setlang
if "%opt%"=="12" set "code=nl" & goto setlang
if "%opt%"=="13" set "code=ru" & goto setlang
if "%opt%"=="14" set "code=ko" & goto setlang
if "%opt%"=="15" set "code=zh-Hans" & goto setlang
if "%opt%"=="16" set "code=zh-Hant" & goto setlang
goto menu

:setlang
> "%~dp0data\language.txt" echo %code%
echo.
echo   Saved: %code%   (data\language.txt)
echo.
set "go="
set /p "go=  Start the game now? [Y/n]: "
if /i "%go%"=="n" exit /b 0
start "Wonder" cmd /c ""%~dp0Jogar.bat""
exit /b 0
"""

LEIAME = """Super Mario Bros. Wonder - recompilado para Windows (uso pessoal; dados do seu proprio cartucho, nao compartilhe)

Como rodar: extraia num caminho CURTO (por exemplo C:\\ , criando C:\\WonderPC) e de dois cliques em Jogar.bat.
Pastas fundas (Downloads\\...\\...) passam do limite de 260 caracteres do Windows em alguns arquivos do jogo.
A primeira abertura demora alguns minutos (o antivirus le os ~31 mil arquivos pela primeira vez); depois e normal.

Requisitos:
- Windows 10 ou 11, 64 bits
- CPU com AVX2 (Intel 4a geracao / Haswell ou mais nova, AMD Ryzen)
- GPU com Vulkan 1.3 e driver atualizado (NVIDIA GTX 900 ou mais nova, AMD RX 400 ou mais nova, Intel Iris Xe / Arc / UHD 7xx)
- 16 GB de RAM recomendados (o jogo usa 3 a 7 GB)

Controles: controle Xbox (XInput, layout Nintendo: A e B trocados) ou teclado:
  WASD / setas mover, Espaco = A (pular), Shift = B (correr), J = X, K = Y, Q/E = L/R, Enter = +, Esc = -
  Tela de titulo: L+R (Q+E). F11 ou Alt+Enter: tela cheia.

Opcoes (edite Jogar.bat, antes da linha start):
  set NX_GPU=integrated     usar a GPU integrada (padrao: a dedicada)
  set NX_HANDHELD=1         modo portatil, 720p (mais leve)
  set NX_MUTE=1             sem som
  set NX_FULLSCREEN=1       comecar em tela cheia

Problemas: o log fica em data\\nx.log (mande junto com um video do erro).
O save fica em data\\user\\save.
"""


def redist_dlls():
    hits = sorted(glob.glob(os.path.join(REDIST, "*", "x64", "Microsoft.VC14*.CRT")))
    if not hits:
        sys.exit(f"no Visual C++ redistributable under {REDIST}")
    crt = hits[-1]
    for name in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        p = os.path.join(crt, name)
        if not os.path.isfile(p):
            sys.exit(f"missing {p}")
        yield p, name


def tree(src, dst, only=None):
    for d, _, files in os.walk(src):
        for f in sorted(files):
            if only and not only(f):
                continue
            p = os.path.join(d, f)
            yield p, dst + "/" + os.path.relpath(p, src).replace(os.sep, "/")


def entries(root, save):
    exe = os.path.join(root, "build", "wonder.exe")
    if not os.path.isfile(exe):
        sys.exit(f"missing {exe} (build it first)")
    yield exe, "wonder.exe"
    yield from redist_dlls()
    yield from tree(os.path.join(root, "analysis"), "data/analysis")
    yield from tree(os.path.join(root, "extract", "romfs"), "data/extract/romfs")
    spirv = os.path.join(root, "spirv")
    if not os.path.isfile(os.path.join(spirv, "results.tsv")):
        sys.exit("missing spirv/results.tsv (docs/nvn/TRANSLATOR.md)")
    yield from tree(spirv, "data/spirv", lambda f: f.endswith(".spv") or f == "results.tsv")
    yield from tree(os.path.join(root, "fonts"), "data/fonts")
    if save and os.path.isdir(os.path.join(root, "user", "save")):
        yield from tree(os.path.join(root, "user", "save"), "data/user/save")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="private/wonder")
    ap.add_argument("--out", default="private/wonder/pc/WonderPC-portable.zip")
    ap.add_argument("--no-save", action="store_true", help="leave the save out (a new game on the other PC)")
    a = ap.parse_args()
    files = list(entries(a.root, not a.no_save))
    total = sum(os.path.getsize(p) for p, _ in files)
    print(f"{len(files)} files, {total / 2**30:.2f} GB -> {a.out}")
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:
        z.writestr(TOP + "Jogar.bat", JOGAR.replace("\n", "\r\n"))
        z.writestr(TOP + "Language.bat", LANGUAGE.replace("\n", "\r\n"))
        z.writestr(TOP + "LEIA-ME.txt", LEIAME.replace("\n", "\r\n"))
        z.writestr(TOP + "data/language.txt", "en-US\r\n")
        for k, (p, name) in enumerate(files):
            z.write(p, TOP + name)
            if k % 2000 == 0:
                print(f"  {k}/{len(files)}")
    print(f"done: {os.path.getsize(a.out) / 2**30:.2f} GB")


if __name__ == "__main__":
    main()
