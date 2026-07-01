#!/usr/bin/env bash
set -euo pipefail

SESSION="CLOUDSUITE"
ROOT="/users/AMH/mydata"
MEMCACHED_DIR="$ROOT/memcached"
BENCH_DIR_1="$ROOT/benchmarks/Cloudsuite1"
BENCH_DIR_2="$ROOT/benchmarks/Cloudsuite2"
MTC_MODE="MTC_OFF"

NUM_TENANTS=2
BASE_PORT=11211

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
# Pick m, n1, n2 based on 10.10.1.Y
# -----------------------------
NODE_IP="$(get_1010_ip)"
OCTET="${NODE_IP##*.}"

share_m=""
share_n1=""
share_n2=""

case "$OCTET" in
  1|2|3)
    share_m="128"
    share_n1="0.5"
    share_n2="0.5"
    ;;
  4|5|6)
    share_m="10000"
    share_n1="0.5"
    share_n2="0.5"
    ;;
  7|8|9|10)
    share_m="5064"
    share_n1="0.01263823065"
    share_n2="0.98736176935"
    ;;
  *)
    echo "ERROR: IP $NODE_IP (last octet $OCTET) not in expected range 1..10" >&2
    exit 1
    ;;
esac

echo "Node IP: $NODE_IP  -> init_share $share_m $share_n1 $share_n2 $MTC_MODE"

# -----------------------------
# Cleanup
# -----------------------------
sudo pkill -u "$USER" gdb         || true
sudo pkill -u "$USER" memcached   || true
sudo pkill -u "$USER" base_memcached || true
sudo pkill -u "$USER" loader      || true
sudo pkill -u "$USER" screen      || true

# -----------------------------
# Build + init shared state
# -----------------------------
cd "$MEMCACHED_DIR"
make
gcc -g -o init_share init_share.c shm_malloc.c -lrt -pthread
./stop_share || true

# -----------------------------
# Start screen session
# -----------------------------
screen -S "$SESSION" -X quit || true
screen -wipe >/dev/null 2>&1 || true
screen -dmS "$SESSION"

# Tracker / init_share
screen_new_window "tracker_win"
screen_send "tracker_win" "./init_share $share_m $share_n1 $share_n2 $MTC_MODE"

########################################################################################################
# TENANTS
########################################################################################################
port="$BASE_PORT"
for i in $(seq 1 "$NUM_TENANTS"); do
  port=$((port + 1))
  win="tenant_win$i"
  screen_new_window "$win"
  screen_send "$win" "./memcached -p $port -t 4 -m 4096 -G"
  echo "Tenant $i is running on port $port."
done

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
  -s docker_servers.txt -w 4 -S 2.7 -D 1024 -j -T 1 -r 100000 && \
./loader -a ../twitter_dataset/twitter_dataset_1g_$i \
  -s docker_servers.txt -g 0.9 -T 1 -c 25 -w 1 -r 25000"

  echo "Benchmark $i is running."
done

# Kill the default window (optional). Keeps the session alive with your named windows.
screen -S "$SESSION" -p 0 -X kill || true
