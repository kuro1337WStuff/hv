@echo off
setlocal
pushd "%~dp0.." || exit /b 1
set "SRC=boot_probe"
set "EFI=BOOTX64"
set "ISO=hv-probe"
set "CASE_FLAGS="
set "ASM_OBJ="
if /i "%~1"=="l2" (
  set "SRC=l2_smoke"
  set "EFI=L2SMOKE"
  set "ISO=hv-l2-smoke"
) else if /i "%~1"=="cpuid" (
  set "SRC=l2_smoke"
  set "EFI=CPUIDRES"
  set "ISO=hv-cpuid-resume"
  set "CASE_FLAGS=/DHV_CPUID_RESUME"
  set "ASM_OBJ=build\cpuid_resume.obj"
) else if /i "%~1"=="npt-write" (
  set "SRC=l2_smoke"
  set "EFI=NPTWRITE"
  set "ISO=hv-npt-write"
  set "CASE_FLAGS=/DHV_NPT_WRITE"
) else if /i "%~1"=="npt-remap" (
  set "SRC=l2_smoke"
  set "EFI=NPTREMAP"
  set "ISO=hv-npt-remap"
  set "CASE_FLAGS=/DHV_NPT_REMAP"
) else if /i "%~1"=="npt-cross" (
  set "SRC=l2_smoke"
  set "EFI=NPTCROSS"
  set "ISO=hv-npt-cross"
  set "CASE_FLAGS=/DHV_NPT_CROSS"
) else if not "%~1"=="" (echo Usage: build.cmd [l2 or cpuid or npt-write or npt-remap or npt-cross] & exit /b 1)
set "TOOLS=build\tools"
set "INC_EDK=%TOOLS%\edk2\MdePkg\Include"
set "PACK=%TOOLS%\packenv\Scripts\python.exe"
if not exist "%INC_EDK%\Uefi.h" (
  git clone --depth 1 --branch edk2-stable202502 --filter=blob:none --sparse https://github.com/tianocore/edk2.git "%TOOLS%\edk2" || exit /b 1
  git -C "%TOOLS%\edk2" sparse-checkout set MdePkg/Include || exit /b 1
)
if not exist "%PACK%" (
  python -m venv "%TOOLS%\packenv" || exit /b 1
)
"%PACK%" -m pip --disable-pip-version-check install -r firmware\pack-requirements.txt || exit /b 1
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%v\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (echo x64 MSVC build tools not found. & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
if defined ASM_OBJ (
  ml64 /nologo /c /Fo"%ASM_OBJ%" firmware\cpuid_resume.asm || exit /b 1
)
cl /nologo /c /TC /std:c11 /O1 /GS- /Zl /W4 /WX %CASE_FLAGS% /I"%INC_EDK%\X64" /I"%INC_EDK%" /Fobuild\%EFI%.obj firmware\%SRC%.c || exit /b 1
link /nologo /nodefaultlib /incremental:no /machine:x64 /subsystem:efi_application /entry:EfiMain /out:build\%EFI%.EFI build\%EFI%.obj %ASM_OBJ% || exit /b 1
"%PACK%" firmware\pack_iso.py build\%EFI%.EFI build\%ISO%.iso || exit /b 1
echo Built build\%ISO%.iso
popd
