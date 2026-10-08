#!/usr/bin/env bash
# Downloads a BirdNET model and prepares it for bsp-server:
#   <dest>/birdnet-<version>/model.onnx
#   <dest>/birdnet-<version>/labels.txt   ("Scientific name_Common name" per line)
#   <dest>/birdnet-geo/                   BirdNET+ geo model (expected species by location)
#
#   v3.0  BirdNET+ V3.0 developer preview (default). Published as ONNX; no conversion.
#   v2.4  BirdNET v2.4. Published for TensorFlow; converted to ONNX by
#         convert_birdnet_v24.py in a throwaway Python virtualenv (downloads
#         TensorFlow, ~1 GB, each run) and verified against TensorFlow.
#
# Usage: scripts/fetch-birdnet.sh [v3.0|v2.4] [--fp32] [--dest DIR]
set -euo pipefail

VERSION="v3.0"
PRECISION="fp16"
DEST="models"
while [[ $# -gt 0 ]]; do
  case "$1" in
    v3.0|v3) VERSION="v3.0" ;;
    v2.4) VERSION="v2.4" ;;
    --fp32) PRECISION="fp32" ;;
    --dest) DEST="$2"; shift ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

for tool in curl python3 md5sum sha256sum; do
  command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done

OUT="$DEST/birdnet-$VERSION"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

# zenodo_fetch RECORD FILENAME TARGET: download a file from a Zenodo record and
# verify it against the MD5 checksum published by Zenodo's API.
zenodo_fetch() {
  local record="$1" name="$2" target="$3"
  local expected
  expected="$(curl -fsSL "https://zenodo.org/api/records/$record" | python3 -I -c '
import json, sys
name = sys.argv[1]
for f in json.load(sys.stdin)["files"]:
    if f["key"] == name:
        print(f["checksum"].removeprefix("md5:"))
        break
else:
    sys.exit(f"{name} not found in Zenodo record")
' "$name")"
  if [[ -f "$target" ]] && [[ "$(md5sum "$target" | cut -d' ' -f1)" == "$expected" ]]; then
    echo "already downloaded: $target"
    return
  fi
  echo "downloading $name ..."
  local progress=(--silent --show-error)
  [[ -t 2 ]] && progress=(--progress-bar)
  curl -fL "${progress[@]}" -o "$target.part" "https://zenodo.org/records/$record/files/$name?download=1"
  local actual
  actual="$(md5sum "$target.part" | cut -d' ' -f1)"
  if [[ "$actual" != "$expected" ]]; then
    rm -f "$target.part"
    echo "checksum mismatch for $name (expected $expected, got $actual)" >&2
    exit 1
  fi
  mv "$target.part" "$target"
}

# github_fetch URL SHA256 TARGET: download a file and verify its SHA-256.
github_fetch() {
  local url="$1" expected="$2" target="$3"
  if [[ -f "$target" ]] && [[ "$(sha256sum "$target" | cut -d' ' -f1)" == "$expected" ]]; then
    echo "already downloaded: $target"
    return
  fi
  echo "downloading ${url##*/} ..."
  local progress=(--silent --show-error)
  [[ -t 2 ]] && progress=(--progress-bar)
  curl -fL "${progress[@]}" -o "$target.part" "$url"
  local actual
  actual="$(sha256sum "$target.part" | cut -d' ' -f1)"
  if [[ "$actual" != "$expected" ]]; then
    rm -f "$target.part"
    echo "checksum mismatch for ${url##*/} (expected $expected, got $actual)" >&2
    exit 1
  fi
  mv "$target.part" "$target"
}

# The geo model is shared by both acoustic models; species are matched by name.
# Checksums are the ones pinned by the official `birdnet` Python package.
fetch_geo() {
  local base="https://github.com/birdnet-team/geomodel/releases/download/v3.0.4"
  local geo="$DEST/birdnet-geo"
  mkdir -p "$geo"
  github_fetch "$base/BirdNET+_Geomodel_V3.0.4_Global_14K_FP32.onnx" \
    0de81d222c23dcb6fa428e958b4dac978783191357e01b7268a103fc6f08e61a "$geo/model.onnx"
  github_fetch "$base/BirdNET+_Geomodel_V3.0.4_Global_14K_Labels.txt" \
    8250b457e45d43fc3e77b5cbd06a1d311baf585ab9c51ed8d42e011d98534835 "$geo/labels.txt"
  curl -fsSL -o "$geo/LICENSE-MODELS.md" "$base/LICENSE-MODELS.md"
}

fetch_v3() {
  local record=20703646
  local base="BirdNET+_V3.0-preview3.1_Global_11K"
  local model="${base}_FP16.onnx"
  [[ "$PRECISION" == "fp32" ]] && model="${base}_FP32.onnx"

  zenodo_fetch "$record" "$model" "$OUT/model.onnx"
  zenodo_fetch "$record" "${base}_Labels.csv" "$WORK/labels.csv"
  zenodo_fetch "$record" "TERMS_OF_USE.txt" "$OUT/TERMS_OF_USE.txt"

  # Semicolon-separated CSV with sci_name/com_name columns, one row per model
  # output in order. Every row is kept so label indices match model outputs.
  python3 -I - "$WORK/labels.csv" "$OUT/labels.txt" <<'EOF'
import csv, sys
src, dst = sys.argv[1], sys.argv[2]
with open(src, encoding="utf-8", newline="") as f:
    rows = list(csv.DictReader(f, delimiter=";"))
with open(dst, "w", encoding="utf-8") as f:
    for row in rows:
        sci = row.get("sci_name", "").strip()
        com = row.get("com_name", "").strip() or sci
        f.write(f"{sci}_{com}\n")
print(f"wrote {len(rows)} labels")
EOF
}

fetch_v2_4() {
  local record=15050749
  # The TensorFlow SavedModel converts more faithfully than the TFLite file.
  zenodo_fetch "$record" "BirdNET_v2.4_protobuf.zip" "$WORK/protobuf.zip"
  python3 -I -m zipfile -e "$WORK/protobuf.zip" "$WORK/protobuf"
  local saved_model labels
  saved_model="$(find "$WORK/protobuf" -type d -name 'audio-model' | head -1)"
  labels="$(find "$WORK/protobuf" -path '*labels*' -name 'en_us.txt' | head -1)"
  [[ -n "$saved_model" && -n "$labels" ]] || { echo "unexpected archive layout:" >&2; find "$WORK/protobuf" -maxdepth 3 >&2; exit 1; }

  # TensorFlow does not support every Python release yet; prefer an older one.
  local py=""
  for candidate in python3.12 python3.11 python3.10 python3; do
    if command -v "$candidate" >/dev/null; then py="$candidate"; break; fi
  done
  echo "creating conversion environment with $py (TensorFlow, tf2onnx, onnxruntime; ~1 GB) ..."
  "$py" -m venv "$WORK/venv"
  "$WORK/venv/bin/pip" install --quiet --upgrade pip
  "$WORK/venv/bin/pip" install --quiet tensorflow-cpu tf2onnx onnxruntime
  echo "converting SavedModel -> ONNX ..."
  # TensorFlow's chatter goes to a log that is shown only on failure.
  if ! TF_CPP_MIN_LOG_LEVEL=3 "$WORK/venv/bin/python" -I "$(dirname "$0")/convert_birdnet_v24.py" \
    "$saved_model" "$WORK/model.onnx" 2>"$WORK/convert.log"; then
    tail -n 30 "$WORK/convert.log" >&2
    echo "conversion failed" >&2
    exit 1
  fi
  mv "$WORK/model.onnx" "$OUT/model.onnx"
  cp "$labels" "$OUT/labels.txt"
  echo "wrote $(grep -c '' "$OUT/labels.txt") labels"
}

case "$VERSION" in
  v3.0) fetch_v3 ;;
  v2.4) fetch_v2_4 ;;
esac
fetch_geo

cat <<EOF

BirdNET $VERSION is ready in $OUT/. To use it, put this in your server config:

[identifier]
kind = "birdnet"

[identifier.birdnet]
version = "$VERSION"
model_path = "$OUT/model.onnx"
labels_path = "$OUT/labels.txt"

The geo model for flagging unexpected species is in $DEST/birdnet-geo/ (the default
location). Set the site's latitude/longitude on the web UI's Config page to enable it.

EOF
case "$VERSION" in
  v3.0) echo "License: CC BY-SA 4.0 plus terms of use (no poaching or military use; attribution required), see $OUT/TERMS_OF_USE.txt." ;;
  v2.4) echo "License: CC BY-NC 4.0 (non-commercial; attribution required)." ;;
esac
echo "Geo model license: Apache 2.0, see $DEST/birdnet-geo/LICENSE-MODELS.md. See the README's attribution section."
