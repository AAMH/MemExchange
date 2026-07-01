#!/usr/bin/env bash
set -euo pipefail

SESSION="CLOUDSUITE"
ROOT="/users/AMH/mydata"
NUM_TENANTS=2

# -----------------------------
# Helpers
# -----------------------------
get_1010_ip() {
  # Prefer iproute2 output, fall back to hostname -I
  local ip
  ip="$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | grep -E '^10\.10\.1\.' | head -n1 || true)"
  if [[ -z "${ip}" ]]; then
    ip="$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^10\.10\.1\.' | head -n1 || true)"
  fi
  [[ -n "${ip}" ]] || { echo "ERROR: Could not find a 10.10.1.X IP on this node." >&2; exit 1; }
  echo "${ip}"
}

screen_new_window() {
  local name="$1"
  screen -S "$SESSION" -X screen -t "$name"
}

screen_send() {
  local win="$1"
  shift
  local cmd="$*"
  # Send cmd + Enter
  screen -S "$SESSION" -p "$win" -X stuff "$cmd$(printf '\r')"
}

# -----------------------------
# Cleanup
# -----------------------------
sudo pkill loader      || true

# -----------------------------
# Start screen session
# -----------------------------
screen -S "$SESSION" -X quit || true
# screen -wipe >/dev/null 2>&1 || true
screen -dmS "$SESSION"

########################################################################################################
# BENCHMARKS
########################################################################################################
for i in $(seq 1 "$NUM_TENANTS"); do
  win="bench_win$i"
  screen_new_window "$win"

  bench_dir="$ROOT/benchmarks/Cloudsuite$i"

  # NOTE: Avoid 'bash;' in the middle; it prevents later commands from running.
  screen_send "$win" "cd '$bench_dir' && make && \
./loader -a ../twitter_dataset/twitter_dataset_unscaled \
  -o ../twitter_dataset/twitter_dataset_1g_$i \
  -s docker_servers.txt -w 2 -S 2.7 -D 1024 -j -T 1 -r 50000 && \
./loader -a ../twitter_dataset/twitter_dataset_1g_$i \
  -s docker_servers.txt -g 0.9 -T 1 -c 25 -w 1 -r 25000"

  echo "Benchmark $i is running."
done

# Kill the default window (optional). Keeps the session alive with your named windows.
screen -S "$SESSION" -p 0 -X kill || true
