#!/usr/bin/env bash
set -euo pipefail

SESSION="MUTILATE"
ROOT="/users/AMH/mydata"
MEMCACHED_DIR="$ROOT/memcached"
MUTILATE_DIR="$ROOT/benchmarks/mutilate"
MTC_MODE="MTC_ON"

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
sudo pkill gdb         || true
sudo pkill memcached   || true
sudo pkill mutilate    || true
sudo pkill screen      || true

# -----------------------------
# Build + init shared state
# -----------------------------
echo "[U] Building Mutilate client..."
cd "${MUTILATE_DIR}"
rm -rf .sconf_temp .sconsign.dblite .sconsign .sconsign.dblite .sconsign.dblite.dblite
scons || true

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
  # screen_send "$win" "gdb -ex=r --args memcached -v -p $port -t 4 -m 4096 -G" 
  echo "Tenant $i is running on port $port."
done

########################################################################################################
# BENCHMARKS
########################################################################################################
LOG_DIR="/users/AMH/mutilate_results"
mkdir -p "$LOG_DIR"

port="$BASE_PORT"
for i in $(seq 1 "$NUM_TENANTS"); do
  port=$((port + 1))
  win="bench_win$i"
  screen_new_window "$win"

  out_file="$LOG_DIR/mutilate_${NODE_IP}_${port}"

  screen_send "$win" "bash -c \"cd '$MUTILATE_DIR' && \
  ./mutilate -s localhost:$port \
  -t 5000 \
  --keysize=fb_key --valuesize=fb_value \
  --iadist=fb_ia --records=3000000 \
  --update=0.05 \
  -q 25000 -c 25 \
    > '${out_file}.out' 2>&1\""

  echo "Benchmark $i running. Output -> $out_file"
done

# Kill the default window (optional). Keeps the session alive with your named windows.
screen -S "$SESSION" -p 0 -X kill || true
