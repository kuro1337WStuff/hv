@echo off
setlocal
pushd "%~dp0.." || exit /b 1
set "SRC=boot_probe"
set "EFI=BOOTX64"
set "ISO=hv-probe"
set "CASE_FLAGS="
set "ASM_OBJ="
set "ASM_SRC="
set "CPUID_OBJ="
set "CROSS_PROFILE="
set "EXTRA_ASM="
set "EXTRA_OBJS="
set "EXTRA_ASM_FLAGS="
set "LINUX_MEDIA="
set "OVMF_PROFILE="
if /i "%~1"=="l1-linux-admission" set "LINUX_MEDIA=1"
if /i "%~1"=="l1-linux-inputs" set "LINUX_MEDIA=1"
if /i "%~1"=="l1-entry-guard" set "LINUX_MEDIA=1"
if /i "%~1"=="l1-acpi-map" set "LINUX_MEDIA=1"
if /i "%~1"=="l1-range-audit" set "LINUX_MEDIA=1"
set "SELECTED=%~1"
if /i "%~1"=="l1-w04-return" set "SELECTED=l1-w04-loader"
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
) else if /i "%~1"=="l1-l2" (
  set "SRC=l1_handoff"
  set "EFI=L1L2"
  set "ISO=hv-l1-l2"
  set "CASE_FLAGS=/DHV_STANDALONE_L2"
  set "ASM_OBJ=build\l1_l2_asm.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
) else if /i "%~1"=="l1-w01" (
  set "SRC=l1_handoff"
  set "EFI=L1W01"
  set "ISO=hv-l1-w01"
  set "OVMF_PROFILE=w01"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_W01"
  set "ASM_OBJ=build\w01_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr w01_switch"
  if not exist build\w01_pin.h (echo Missing prepared W01 pin. & exit /b 1)
) else if /i "%~1"=="l1-w02" (
  set "SRC=l1_handoff"
  set "EFI=L1W02"
  set "ISO=hv-l1-w02"
  set "OVMF_PROFILE=w02"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_W02"
  set "ASM_OBJ=build\w02_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr w01_switch"
  if not exist build\w02_pin.h (echo Missing prepared W02 pin. & exit /b 1)
) else if /i "%~1"=="l1-w03" (
  set "SRC=l1_handoff"
  set "EFI=L1W03"
  set "ISO=hv-l1-w03"
  set "OVMF_PROFILE=w03"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_W02 /DHV_W03"
  set "ASM_OBJ=build\w03_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr w01_switch"
  if not exist build\w03_pin.h (echo Missing prepared W03 pin. & exit /b 1)
) else if /i "%SELECTED%"=="l1-w04-loader" (
  rem W04 changes the L1 observation only. Reuse the exact W03 guest inputs.
  set "SRC=l1_handoff"
  set "EFI=L1W04"
  set "ISO=hv-l1-w04-loader"
  set "OVMF_PROFILE=w03"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_W02 /DHV_W03 /DHV_W04"
  set "ASM_OBJ=build\w04_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr w01_switch"
  if not exist build\w03_pin.h (echo Missing prepared W03 pin. & exit /b 1)
  if /i "%~1"=="l1-w04-return" (
    set "EFI=L1W04RET"
    set "ISO=hv-l1-w04-return"
    set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_W02 /DHV_W03 /DHV_W04 /DHV_W04_RETURN"
  )
) else if /i "%~1"=="l1-cpuid" (
  set "SRC=l1_handoff"
  set "EFI=L1CPUID"
  set "ISO=hv-l1-cpuid"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_CPUID_RESUME"
  set "ASM_OBJ=build\l1_cpuid_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "CPUID_OBJ=build\l1_cpuid_guest.obj"
) else if /i "%~1"=="l1-npt-write" (
  set "SRC=l1_handoff"
  set "EFI=L1NPTWRITE"
  set "ISO=hv-l1-npt-write"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_NPT_WRITE"
  set "ASM_OBJ=build\l1_npt_write_asm.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
) else if /i "%~1"=="l1-npt-remap" (
  set "SRC=l1_handoff"
  set "EFI=L1NPTREMAP"
  set "ISO=hv-l1-npt-remap"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_NPT_REMAP"
  set "ASM_OBJ=build\l1_npt_remap_asm.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
) else if /i "%~1"=="l1-package" (
  set "SRC=l1_handoff"
  set "EFI=L1PACKAGE"
  set "ISO=hv-l1-package"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE"
  set "ASM_OBJ=build\l1_package_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_fpstate cpuid_resume exception_ud vcpu_gpr pkg_devices"
) else if /i "%~1"=="l1-cpu-contract" (
  set "SRC=l1_handoff"
  set "EFI=L1CPUCONTRACT"
  set "ISO=hv-l1-cpu-contract"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_CPU_CONTRACT"
  set "ASM_OBJ=build\l1_cpu_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_fpstate cpu_contract"
  set "EXTRA_ASM_FLAGS=/DHV_CPU_CONTRACT"
) else if /i "%~1"=="l1-cpu-floor" (
  set "SRC=l1_handoff"
  set "EFI=L1CPUFLOOR"
  set "ISO=hv-l1-cpu-floor"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_CPU_CONTRACT /DHV_FLOOR_CONTRACT"
  set "ASM_OBJ=build\l1_floor_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_fpstate floor_guest"
  set "EXTRA_ASM_FLAGS=/DHV_CPU_CONTRACT"
) else if /i "%~1"=="l1-page-pair" (
  set "SRC=l1_handoff"
  set "EFI=L1PAGEPAIR"
  set "ISO=hv-l1-page-pair"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_PAGE_PAIR"
  set "ASM_OBJ=build\l1_page_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr page_pair"
  set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
) else if /i "%~1"=="l1-page-transition" (
  set "SRC=l1_handoff"
  set "EFI=L1PAGETRANSITION"
  set "ISO=hv-l1-page-transition"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_PAGE_TRANSITION"
  set "ASM_OBJ=build\l1_page_transition_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr page_transition"
  set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
) else if /i "%~1"=="l1-cr4-guard" (
  set "SRC=l1_handoff"
  set "EFI=L1CR4GUARD"
  set "ISO=hv-l1-cr4-guard"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_CR4_GUARD"
  set "ASM_OBJ=build\l1_cr4_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr cr4_guard"
  set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
) else if /i "%~1"=="l1-cr4-gp" (
  set "SRC=l1_handoff"
  set "EFI=L1CR4GP"
  set "ISO=hv-l1-cr4-gp"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_CR4_GP"
  set "ASM_OBJ=build\l1_gp_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr cr4_gp"
  set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
) else if /i "%~1"=="l1-serial-8250" (
  set "SRC=l1_handoff"
  set "EFI=L1SERIAL8250"
  set "ISO=hv-l1-serial-8250"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_SERIAL_CONTRACT"
  set "ASM_OBJ=build\l1_serial_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr uart8250_guest"
) else if /i "%~1"=="l1-pit-irq0" (
  set "SRC=l1_handoff"
  set "EFI=L1PITIRQ0"
  set "ISO=hv-l1-pit-irq0"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_TICK_CONTRACT"
  set "ASM_OBJ=build\l1_pit_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr tick_guest"
) else if /i "%~1"=="l1-console-tx" (
  set "SRC=l1_handoff"
  set "EFI=L1CONSOLETX"
  set "ISO=hv-l1-console-tx"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_TX_CONTRACT"
  set "ASM_OBJ=build\l1_tx_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr tx_guest"
) else if /i "%~1"=="l1-power-s5" (
  set "SRC=l1_handoff"
  set "EFI=L1POWERS5"
  set "ISO=hv-l1-power-s5"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_PACKAGE /DHV_POWER_CONTRACT"
  set "ASM_OBJ=build\l1_power_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr power_guest"
) else if defined LINUX_MEDIA (
  set "SRC=l1_handoff"
  set "EFI=L1LXADMIT"
  set "ISO=hv-l1-linux-admission"
  set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_LINUX_ADMISSION"
  set "ASM_OBJ=build\linux_handoff.obj"
  set "ASM_SRC=firmware\l1_handoff.asm"
  set "EXTRA_ASM=vcpu_gpr"
  if /i "%~1"=="l1-linux-inputs" (
    set "EFI=L1LXINPUT"
    set "ISO=hv-l1-linux-inputs"
    set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_LINUX_ADMISSION /DHV_INPUT_TEST"
    set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
  ) else if /i "%~1"=="l1-entry-guard" (
    set "EFI=L1ENTRYGUARD"
    set "ISO=hv-l1-entry-guard"
    set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_LINUX_ADMISSION /DHV_ENTRY_TEST"
    set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
  ) else if /i "%~1"=="l1-range-audit" (
    set "EFI=L1RANGEAUDIT"
    set "ISO=hv-l1-range-audit"
    set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_LINUX_ADMISSION /DHV_RANGE_AUDIT"
    set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
  ) else if /i "%~1"=="l1-acpi-map" (
    set "EFI=L1ACPIMAP"
    set "ISO=hv-l1-acpi-map"
    set "CASE_FLAGS=/DHV_STANDALONE_L2 /DHV_LINUX_ADMISSION /DHV_ACPI_MAP"
    set "EXTRA_ASM_FLAGS=/DHV_COUNT_ENTRIES"
  )
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
) else if not "%~1"=="" (echo Unknown firmware profile: %~1 & exit /b 1)
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
if defined CPUID_OBJ (
  ml64 /nologo /c /Fo"%CPUID_OBJ%" firmware\cpuid_resume.asm || exit /b 1
)
for %%s in (%EXTRA_ASM%) do call :extra_asm %%s || exit /b 1
cl /nologo /c /TC /std:c11 /O1 /GS- /Zl /W4 /WX %CASE_FLAGS% /I"%INC_EDK%\X64" /I"%INC_EDK%" /Fobuild\%EFI%.obj firmware\%SRC%.c || exit /b 1
link /nologo /nodefaultlib /incremental:no /machine:x64 /subsystem:efi_application /entry:EfiMain /out:build\%EFI%.EFI build\%EFI%.obj %ASM_OBJ% %CPUID_OBJ% %EXTRA_OBJS% || exit /b 1
if defined OVMF_PROFILE (
  "%PACK%" firmware\%OVMF_PROFILE%_media.py pack build\%EFI%.EFI build\%ISO%.iso --inputs build\%OVMF_PROFILE% || exit /b 1
) else if defined LINUX_MEDIA (
  "%PACK%" firmware\linux_media.py build\%EFI%.EFI build\%ISO%.iso || exit /b 1
) else (
  "%PACK%" firmware\pack_iso.py build\%EFI%.EFI build\%ISO%.iso || exit /b 1
)
goto :done
:cross
"%PACK%" firmware\cross\build.py %CROSS_PROFILE% || exit /b 1
:done
echo Built build\%ISO%.iso
popd
exit /b 0
:extra_asm
ml64 /nologo /c %EXTRA_ASM_FLAGS% /I"%~dp0." /Fo"build\pkg_%1.obj" "firmware\%1.asm" || exit /b 1
set "EXTRA_OBJS=%EXTRA_OBJS% build\pkg_%1.obj"
exit /b 0
