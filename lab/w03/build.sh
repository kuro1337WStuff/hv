#!/usr/bin/env bash
# Build only the fixed EDK II guest image. No Windows source, media, or VM input.
set -eo pipefail
export LC_ALL=C TZ=UTC
umask 022
repo=$PWD
recipe=$(git -c "safe.directory=$repo" -C "$repo" rev-parse --verify HEAD)
[[ $recipe =~ ^[0-9a-f]{40}$ && $recipe == "${GITHUB_SHA:-$recipe}" ]]
out=$repo/build/w03
src=/tmp/w03-edk2
pin=556dcfb466e9f05affeaff881d738807f8156d3c
tag=edk2-stable202611
[[ ! -e "$src" && ! -e "$out" ]]
mkdir -p "$out"
git clone --depth 1 --branch "$tag" https://github.com/tianocore/edk2.git "$src"
cd "$src"
[[ $(git rev-parse HEAD) == "$pin" && $(git rev-parse "$tag^{commit}") == "$pin" ]]
git submodule update --init --depth 1 --jobs 4
git submodule status | tee "$out/submodules.txt"
if grep -E '^[-+U]' "$out/submodules.txt"; then exit 1; fi
export SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
python3 "$repo/lab/w03/profile.py"
git diff --binary > "$out/edk2-profile.patch"
cp -r OvmfPkg/W02 "$out/board-source"
printf 'repo=%s\nedk2=%s\nepoch=%s\n' "$recipe" "$pin" "$SOURCE_DATE_EPOCH" > "$out/source.txt"
source edksetup.sh
make -C BaseTools/Source/C -j2
build -p OvmfPkg/OvmfPkgX64.dsc -a X64 -b RELEASE -t GCC -n 2 -D FD_SIZE_IN_KB=4096
fv=Build/OvmfX64/RELEASE_GCC/FV
cp "$fv/OVMF_CODE.fd" "$out/W03CODE.FD"
cp "$fv/OVMF_VARS.fd" "$out/W03VARS.FD"
for vol in secfv peifv dxefv; do
  mapfile -d '' -t maps < <(find "$fv" -maxdepth 1 -type f -iname "$vol.fv.map" -print0)
  [[ ${#maps[@]} -eq 1 && -s ${maps[0]} ]]
  cp "${maps[0]}" "$out/${vol^^}.Fv.map"
done
cd "$out"
[[ $(stat -c %s W03CODE.FD) == $((0x37c000)) && $(stat -c %s W03VARS.FD) == $((0x84000)) ]]
[[ $(tail -c 4096 W03CODE.FD | sha256sum | cut -d' ' -f1) == 2ed89d453f0312b6fd3576afc024109ca64740718511237d4b928da4fe70cd61 ]]
sha256sum W03CODE.FD W03VARS.FD *.Fv.map edk2-profile.patch board-source/* source.txt submodules.txt | tee SHA256SUMS
