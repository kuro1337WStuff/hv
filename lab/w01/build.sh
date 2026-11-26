#!/usr/bin/env bash
# Run from a clean project checkout in the workflow's pinned amd64 EDK II container.
# Produces only flash inputs and the independent upstream GenFv map; never runs a VM.
set -eo pipefail
export LC_ALL=C TZ=UTC
umask 022
out="$PWD/build/w01"
src=/tmp/w01-edk2
pin=556dcfb466e9f05affeaff881d738807f8156d3c
tag=edk2-stable202611
[[ ! -e "$src" && ! -e "$out" ]] || { echo 'Expected a fresh build workspace' >&2; exit 1; }
git clone --depth 1 --branch "$tag" https://github.com/tianocore/edk2.git "$src"
cd "$src"
[[ "$(git rev-parse HEAD)" == "$pin" && "$(git rev-parse "$tag^{commit}")" == "$pin" ]]
git submodule update --init --depth 1 --jobs 4
# No nested modules are used by this tree. Keep the exact gitlinks in the job log.
git submodule status
if git submodule status | grep -E '^[-+U]'; then exit 1; fi
export SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
source edksetup.sh
make -C BaseTools/Source/C -j2
build -p OvmfPkg/OvmfPkgX64.dsc -a X64 -b RELEASE -t GCC -n 2 -D FD_SIZE_IN_KB=4096
fv=Build/OvmfX64/RELEASE_GCC/FV
shopt -s nullglob nocaseglob
map=("$fv"/secfv.fv.map)
[[ ${#map[@]} == 1 && -s "${map[0]}" ]]
[[ $(stat -c %s "$fv/OVMF_CODE.fd") == $((0x37c000)) && $(stat -c %s "$fv/OVMF_VARS.fd") == $((0x84000)) ]]
mkdir -p "$out"
cp "$fv/OVMF_CODE.fd" "$out/W01CODE.FD"
cp "$fv/OVMF_VARS.fd" "$out/W01VARS.FD"
cp "${map[0]}" "$out/SECFV.Fv.map"
cd "$out"
printf 'EDK II source: %s (%s), epoch %s\n' "$tag" "$pin" "$SOURCE_DATE_EPOCH"
sha256sum W01CODE.FD W01VARS.FD SECFV.Fv.map
