#!/usr/bin/env bash
set -euo pipefail

host=${SGLANG_HOST_IP:-141.111.65.132}
port=${PORT:-30002}
model_path=${MODEL_PATH:-/home/models/GLM-5-FP8}
log_dir=${LOG_DIR:-/home/wq/logs/0520}
python_bin=${PYTHON_BIN:-python}
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ucm_config_file=${UCM_CONFIG_FILE:-"$script_dir/ucm_asu.yaml"}

if [[ ! -f "$ucm_config_file" || ! -r "$ucm_config_file" ]]; then
  printf 'Cannot read UCM config: %s\n' "$ucm_config_file" >&2
  exit 1
fi

export SGLANG_HOST_IP="$host"
export CUDA_VISIBLE_DEVICES=${CUDA_VISIBLE_DEVICES:-0,1,2,3,4,5,6,7}
export ENABLE_UCM_PATCH=1
export SGLANG_DSA_PREFILL_DENSE_ATTN_KV_LEN_THRESHOLD=0
export SGLANG_HEALTH_CHECK_TIMEOUT=600
export SGLANG_TIMEOUT_KEEP_ALIVE=600

export SGLANG_DG_CACHE_DIR=${SGLANG_DG_CACHE_DIR:-/home/cache/deep_gemm}
export TRITON_CACHE_DIR=${TRITON_CACHE_DIR:-/home/cache/triton}
export TORCHINDUCTOR_CACHE_DIR=${TORCHINDUCTOR_CACHE_DIR:-/home/cache/inductor}
export SGLANG_JIT_DEEPGEMM_FAST_WARMUP=1
export SGL_DG_USE_NVRTC=1
export SGLANG_ENABLE_JIT_DEEPGEMM=true
export SGLANG_USE_DEEPGEMM_BMM=true

mkdir -p "$log_dir" "$SGLANG_DG_CACHE_DIR" "$TRITON_CACHE_DIR" "$TORCHINDUCTOR_CACHE_DIR"
linker_config=$("$python_bin" -c '
import json
import os
import sys

print(json.dumps({
    "linker_module_path": "ucm.integration.sglang.ucm_direct_linker",
    "linker": "UcmLayerWiseLinker",
    "linker_extra_config": {"UCM_CONFIG_FILE": os.path.abspath(sys.argv[1])},
}))
' "$ucm_config_file")

"$python_bin" -m sglang.launch_server \
  --model-path "$model_path" \
  --host "$host" \
  --port "$port" \
  --tool-call-parser glm47 \
  --reasoning-parser glm45 \
  --tensor-parallel-size 8 \
  --moe-dense-tp-size 1 \
  --attn-cp-size 8 \
  --enable-prefill-cp \
  --cp-strategy interleave \
  --mem-fraction-static 0.85 \
  --moe-a2a-backend none \
  --deepep-mode normal \
  --enable-metrics-for-all-schedulers \
  --enable-metrics \
  --enable-cache-report \
  --trust-remote-code \
  --log-level info \
  --watchdog-timeout 3600 \
  --page-size 64 \
  --chunked-prefill-size 16384 \
  --disaggregation-mode null \
  --enable-unified-cache-external-linker \
  --unified-cache-external-linker-backend dynamic \
  --unified-cache-external-linker-config "$linker_config" \
  2>&1 | tee -i "$log_dir/mix_asu_$(date +%Y%m%d_%H%M%S).log"
