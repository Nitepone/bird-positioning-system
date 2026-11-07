#!/usr/bin/env bash
# Downloads BirdNET-Tiny N6 GLOBAL 1K v2.0 Hybrid (Apache-2.0) into
# trigger-models/birdnet-tiny-2.0-hybrid/model.tflite. It is 2 MB, so it is
# not kept in git; the DrongoNet models are small enough to be.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out="$here/birdnet-tiny-2.0-hybrid"
url=https://github.com/birdnet-team/birdnet-stm32/releases/download/v2.0/BirdNET_Tiny_N6_GLOBAL_1K_V2.0_Hybrid.zip
zip_sha=134532c40966ea494c92bb3ec17a0e826411a7cc1ceb5efa52f97633d98762c6
model_sha=3f704a1d08298f1d31755105f21cd2b65de5ade6a8fe61a91a1b97e05a0e8ce6

if [[ -f "$out/model.tflite" ]] && echo "$model_sha  $out/model.tflite" | sha256sum -c --quiet 2>/dev/null; then
    echo "already there: $out/model.tflite"
    exit 0
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL --retry 3 -o "$work/bundle.zip" "$url"
echo "$zip_sha  $work/bundle.zip" | sha256sum -c --quiet
python3 - "$work/bundle.zip" "$out/model.tflite" <<'EOF'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    open(sys.argv[2], "wb").write(z.read("BirdNET_Tiny_N6_GLOBAL_1K_V2.0_Hybrid_INT8.tflite"))
EOF
echo "$model_sha  $out/model.tflite" | sha256sum -c --quiet
echo "wrote $out/model.tflite"
