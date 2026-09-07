#!/usr/bin/env bash
# Compile every permutation of TexGen's compute shader with fxc.
#
# TexGen's shaders are built at runtime through Util::CompileShader, so they are not in
# .github/configs/shader-validation.yaml and hlslkit never sees them. Without this, a syntax error
# in a permutation only shows up as a failed compile line in CommunityShaders.log after launching
# the game.
#
# Usage: tools/compile-texgen-shaders.sh
# Exits non-zero if any permutation fails.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
shader="$repo_root/features/TexGen/Shaders/TexGen/GenerateCacheMaps.hlsl"
includes="$repo_root/package/Shaders"

permutations=(CARDINALS SWEEP SWEEP_FINALIZE HEIGHT_SMOOTH NORMALS STATIC_RASTER)

fxc="$(find "/c/Program Files (x86)/Windows Kits/10/bin" -name fxc.exe -path '*x64*' 2>/dev/null | sort -V | tail -1)"
if [[ -z "$fxc" ]]; then
	echo "fxc.exe not found under the Windows SDK; install the SDK or compile in-game instead." >&2
	exit 1
fi

if [[ ! -f "$shader" ]]; then
	echo "shader not found: $shader" >&2
	exit 1
fi

out_dir="$(mktemp -d)"
trap 'rm -rf "$out_dir"' EXIT

failed=0
for define in "${permutations[@]}"; do
	out="$out_dir/$define.cso"
	if output=$("$fxc" -nologo -T cs_5_0 -E main -D "$define=1" \
		-I "$(cygpath -w "$includes")" \
		-Fo "$(cygpath -w "$out")" \
		"$(cygpath -w "$shader")" 2>&1) && [[ -f "$out" ]]; then
		size=$(stat -c %s "$out")
		echo "ok    $define ($size bytes)"
		# Warnings still matter even when the compile succeeds.
		echo "$output" | grep -i "warning" | sed 's/^/      /' || true
	else
		echo "FAIL  $define"
		echo "$output" | sed 's/^/      /'
		failed=1
	fi
done

exit $failed
