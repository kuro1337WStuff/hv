@echo off
setlocal
pushd "%~dp0.." || exit /b 1
set "SRC=boot_probe"
set "EFI=BOOTX64"
set "ISO=hv-probe"
set "CASE_FLAGS="
set "ASM_OBJ="
set "ASM_SRC="
set "CROSS_PROFILE="
if /i "%~1"=="l2" (
  set "SRC=l2_smoke"
  set "EFI=L2SMOKE"
  set "ISO=hv-l2-smoke"
) else if /i "%~1"=="l1-handoff" (
  set "SRC=l1_handoff"
  set "EFI=L1HANDOFF"
  set "ISO=hv-l1-handoff"
  set "ASM_OBJ=build\l1_handoff_asm.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
) else if /i "%~1"=="cpuid" (
  set "SRC=l2_smoke"
  set "EFI=CPUIDRES"
  set "ISO=hv-cpuid-resume"
  set "CASE_FLAGS=/DHV_CPUID_RESUME"
  set "ASM_OBJ=build\cpuid_resume.obj"
  set "ASM_SRC=firmware\cpuid_resume.asm"
) else if /i "%~1"=="vcpu-gpr" (
  set "SRC=l2_smoke"
  set "EFI=VCPUGPR"
  set "ISO=hv-vcpu-gpr"
  set "CASE_FLAGS=/DHV_VCPU_GPR"
  set "ASM_OBJ=build\vcpu_gpr.obj"
  set "ASM_SRC=firmware\vcpu_gpr.asm"
) else if /i "%~1"=="vcpu-xmm" (
  set "SRC=l2_smoke"
  set "EFI=VCPUXMM"
  set "ISO=hv-vcpu-xmm"
  set "CASE_FLAGS=/DHV_VCPU_XMM"
  set "ASM_OBJ=build\vcpu_xmm.obj"
  set "ASM_SRC=firmware\vcpu_xmm.asm"
) else if /i "%~1"=="vcpu-fpstate" (
  set "SRC=l2_smoke"
  set "EFI=VCPUFP"
  set "ISO=hv-vcpu-fpstate"
  set "CASE_FLAGS=/DHV_VCPU_FPSTATE"
  set "ASM_OBJ=build\vcpu_fpstate.obj"
  set "ASM_SRC=firmware\vcpu_fpstate.asm"
) else if /i "%~1"=="vcpu-mxcsr" (
  set "SRC=l2_smoke"
  set "EFI=VCPUMXCS"
  set "ISO=hv-vcpu-mxcsr"
  set "CASE_FLAGS=/DHV_VCPU_MXCSR"
  set "ASM_OBJ=build\vcpu_mxcsr.obj"
  set "ASM_SRC=firmware\vcpu_mxcsr.asm"
) else if /i "%~1"=="exception-ud" (
  set "SRC=l2_smoke"
  set "EFI=EXCEPTUD"
  set "ISO=hv-exception-ud"
  set "CASE_FLAGS=/DHV_EXCEPTION_UD"
  set "ASM_OBJ=build\exception_ud.obj"
  set "ASM_SRC=firmware\exception_ud.asm"
) else if /i "%~1"=="irq-priority" (
  set "SRC=l2_smoke"
  set "EFI=IRQPRIORITY"
  set "ISO=hv-irq-priority"
  set "CASE_FLAGS=/DHV_IRQ_WAKE /DHV_IRQ_PRIORITY"
  set "ASM_OBJ=build\irq_priority.obj"
  set "ASM_SRC=firmware\irq_priority.asm"
) else if /i "%~1"=="irq-shadow" (
  set "SRC=l2_smoke"
  set "EFI=IRQSHADOW"
  set "ISO=hv-irq-shadow"
  set "CASE_FLAGS=/DHV_IRQ_WAKE /DHV_IRQ_SHADOW"
  set "ASM_OBJ=build\irq_shadow.obj"
  set "ASM_SRC=firmware\irq_shadow.asm"
) else if /i "%~1"=="irq-wake" (
  set "SRC=l2_smoke"
  set "EFI=IRQWAKE"
  set "ISO=hv-irq-wake"
  set "CASE_FLAGS=/DHV_IRQ_WAKE"
  set "ASM_OBJ=build\irq_wake.obj"
  set "ASM_SRC=firmware\irq_wake.asm"
) else if /i "%~1"=="npt-write" (
  set "SRC=l2_smoke"
  set "EFI=NPTWRITE"
  set "ISO=hv-npt-write"
  set "CASE_FLAGS=/DHV_NPT_WRITE"
) else if /i "%~1"=="forensic-write" (
  set "SRC=l2_smoke"
  set "EFI=FORENSIC"
  set "ISO=hv-forensic-write"
  set "CASE_FLAGS=/DHV_NPT_WRITE /DHV_FORENSIC_WRITE"
) else if /i "%~1"=="npt-remap" (
  set "SRC=l2_smoke"
  set "EFI=NPTREMAP"
  set "ISO=hv-npt-remap"
  set "CASE_FLAGS=/DHV_NPT_REMAP"
) else if /i "%~1"=="npt-cross" (
  rem The cross profile uses Limine and a freestanding ELF, not EDK II.
  set "ISO=hv-npt-cross"
  set "CROSS_PROFILE=npt-cross"
) else if /i "%~1"=="npt-broadcast" (
  set "ISO=hv-npt-broadcast"
  set "CROSS_PROFILE=npt-broadcast"
) else if not "%~1"=="" (echo Usage: build.cmd [l1-handoff or l2 or cpuid or vcpu-gpr or vcpu-xmm or vcpu-mxcsr or vcpu-fpstate or exception-ud or irq-wake or irq-shadow or irq-priority or npt-write or npt-remap or npt-cross or npt-broadcast or forensic-write] & exit /b 1)
set "TOOLS=build\tools"
set "INC_EDK=%TOOLS%\edk2\MdePkg\Include"
set "PACK=%TOOLS%\packenv\Scripts\python.exe"
if defined CROSS_PROFILE goto :packenv
if not exist "%INC_EDK%\Uefi.h" (
  git clone --depth 1 --branch edk2-stable202502 --filter=blob:none --sparse https://github.com/tianocore/edk2.git "%TOOLS%\edk2" || exit /b 1
  git -C "%TOOLS%\edk2" sparse-checkout set MdePkg/Include || exit /b 1
)
:packenv
if not exist "%PACK%" (
  python -m venv "%TOOLS%\packenv" || exit /b 1
)
"%PACK%" -m pip --disable-pip-version-check install -r firmware\pack-requirements.txt || exit /b 1
if defined CROSS_PROFILE goto :cross
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%v in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%v\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (echo x64 MSVC build tools not found. & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
if defined ASM_OBJ (
  ml64 /nologo /c /I"%~dp0." /Fo"%ASM_OBJ%" "%ASM_SRC%" || exit /b 1
)
cl /nologo /c /TC /std:c11 /O1 /GS- /Zl /W4 /WX %CASE_FLAGS% /I"%INC_EDK%\X64" /I"%INC_EDK%" /Fobuild\%EFI%.obj firmware\%SRC%.c || exit /b 1
link /nologo /nodefaultlib /incremental:no /machine:x64 /subsystem:efi_application /entry:EfiMain /out:build\%EFI%.EFI build\%EFI%.obj %ASM_OBJ% || exit /b 1
"%PACK%" firmware\pack_iso.py build\%EFI%.EFI build\%ISO%.iso || exit /b 1
goto :done
:cross
"%PACK%" firmware\cross\build.py %CROSS_PROFILE% || exit /b 1
:done
echo Built build\%ISO%.iso
popd
