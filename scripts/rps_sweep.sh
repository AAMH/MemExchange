#!/usr/bin/env bash
set -euo pipefail

# -----------------------------
# Configuration
# -----------------------------
RPS_LIST=(10000 20000 30000 40000 50000 60000 70000 80000 90000 100000)
RUN_SECS=120

OUT_BASE="$HOME"
OUT_PREFIX="cloudsuite_10.10.1.1_11212"

# CloudSuite command template
CMD_BASE="./loader \
  -a ../twitter_dataset/twitter_dataset_unscaled \
  -s docker_servers.txt \
  -g 1 \
  -T 1 \
  -c 100 \
  -w 4 \
  -f 650 \
  -k 1230000 \
  -b 12345"

# -----------------------------
# Helper
# -----------------------------
timestamp() {
  date +"%Y%m%d_%H%M%S"
}

# -----------------------------
# Sweep loop
# -----------------------------
for RPS in "${RPS_LIST[@]}"; do
  echo "========================================"
  echo "Starting CloudSuite run: RPS=${RPS}"
  echo "========================================"

  OUT_FILE="${OUT_BASE}/${OUT_PREFIX}.csv"

  # Remove any stale output
  rm -f "${OUT_FILE}"

  # Start CloudSuite
  ${CMD_BASE} -r "${RPS}" &
  LOADER_PID=$!

  echo "CloudSuite PID: ${LOADER_PID}"
  echo "Running for ${RUN_SECS} seconds..."
  sleep "${RUN_SECS}"

  echo "Stopping CloudSuite..."
  kill -TERM "${LOADER_PID}" || true

  # Give it a moment to flush
  sleep 5

  # Safety: ensure it's dead
  if kill -0 "${LOADER_PID}" 2>/dev/null; then
    echo "CloudSuite still running, killing forcefully"
    kill -KILL "${LOADER_PID}" || true
  fi

  # Rename output
  if [[ -f "${OUT_FILE}" ]]; then
    TS="$(timestamp)"
    NEW_NAME="${OUT_BASE}/${OUT_PREFIX}_rps${RPS}_${TS}.csv"
    mv "${OUT_FILE}" "${NEW_NAME}"
    echo "Saved results to: ${NEW_NAME}"
  else
    echo "WARNING: Expected output file not found for RPS=${RPS}"
  fi

  echo "Cooling down before next run..."
  sleep 10
done

echo "========================================"
echo "RPS sweep complete."
echo "========================================"
