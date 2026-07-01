#!/usr/bin/env bash
set -euo pipefail

#############################################
# Controller-side configuration
#############################################

MANIFEST=${MANIFEST:-manifest.xml}     # CloudLab manifest file
SERVERS_LIST=${SERVERS_LIST:-servers-list}  # One hostname per line (msXXXX.utah.cloudlab.us)
ROLES_CFG=${ROLES_CFG:-roles.cfg}          # "index role" lines, role in {U,O,M}
REMOTE_USER=${REMOTE_USER:-AMH}            # SSH username on CloudLab nodes
REMOTE_ISROOT=${REMOTE_ISROOT:-/users/AMH/mydata/Infiniswap}
REMOTE_SETUP_DIR="$REMOTE_ISROOT/setup"
REMOTE_LOOP_IMG=${REMOTE_LOOP_IMG:-/mydata/is_backup.img}
REMOTE_IS_SIZE_GB=${REMOTE_IS_SIZE_GB:-12} # size of loopfile (GB)
MEMCACHED_VER=${MEMCACHED_VER:-1.6.23}
PREFIX=${PREFIX:-/usr/local}

#############################################
# Helpers: read servers, roles, manifest
#############################################

declare -a NODE_HOSTS NODE_IPS NODE_ROLES
declare -a DAEMON_IDX BD_IDX

require_tools() {
  if ! command -v xmlstarlet >/dev/null 2>&1; then
    echo "Installing xmlstarlet on controller..."
    brew update
    brew install xmlstarlet
  fi
}

load_nodes_and_ips() {
  mapfile -t NODE_HOSTS < "$SERVERS_LIST"
  local num_nodes=${#NODE_HOSTS[@]}
  echo "Loaded $num_nodes hosts from $SERVERS_LIST"

  # Build host->ip map from manifest
  declare -gA HOST_TO_IP
  while read -r host ip; do
    [[ -z "$host" || -z "$ip" ]] && continue
    HOST_TO_IP["$host"]="$ip"
  done < <(
    xmlstarlet sel \
      -N rs="http://www.geni.net/resources/rspec/3" \
      -t -m "//rs:node" \
      -v "rs:services/rs:login/@hostname" -o " " \
      -v "rs:interface/rs:ip/@address" -n \
      "$MANIFEST"
  )

  NODE_IPS=()
  for (( i=0; i<num_nodes; i++ )); do
    local h="${NODE_HOSTS[$i]}"
    local ip="${HOST_TO_IP[$h]:-}"
    if [[ -z "$ip" ]]; then
      echo "ERROR: No RDMA IP for host '$h' in manifest" >&2
      exit 1
    fi
    NODE_IPS[$i]="$ip"
  done

  echo "Host → RDMA IP mapping:"
  for (( i=0; i<num_nodes; i++ )); do
    echo "  [$i] ${NODE_HOSTS[$i]}  ${NODE_IPS[$i]}"
  done
}

load_roles() {
  local num_nodes=${#NODE_HOSTS[@]}
  NODE_ROLES=()

  while read -r idx role; do
    [[ -z "$idx" || "$idx" =~ ^# ]] && continue
    NODE_ROLES[idx]="$role"
  done < "$ROLES_CFG"

  echo "Roles from $ROLES_CFG:"
  for (( i=0; i<num_nodes; i++ )); do
    echo "  [$i] host=${NODE_HOSTS[$i]} role=${NODE_ROLES[$i]:-?}"
  done

  DAEMON_IDX=()
  BD_IDX=()

  for (( i=0; i<num_nodes; i++ )); do
    local r="${NODE_ROLES[$i]}"
    case "$r" in
      O|o)
        DAEMON_IDX+=("$i")
        ;;
      M|m)
        DAEMON_IDX+=("$i")
        BD_IDX+=("$i")
        ;;
      U|u)
        BD_IDX+=("$i")
        ;;
      *)
        echo "WARNING: Node $i (${NODE_HOSTS[$i]}) has unknown role '$r'; skipping."
        ;;
    esac
  done

  echo "Daemon nodes (offer remote memory): ${DAEMON_IDX[*]}"
  echo "BD nodes (swap clients):            ${BD_IDX[*]}"
}

#############################################
# SSH helpers
#############################################

ssh_node() {
  local idx="$1"; shift
  local h="${NODE_HOSTS[$idx]}"
  ssh -o StrictHostKeyChecking=no "${REMOTE_USER}@${h}" "$@"
}

scp_to_node() {
  local idx="$1"; shift
  local src="$1"; local dst="$2"
  local h="${NODE_HOSTS[$idx]}"
  scp -o StrictHostKeyChecking=no "$src" "${REMOTE_USER}@${h}:$dst"
}

#############################################
# Phase 1: enable cgroup memory+swap on all
#############################################

phase1_enable_cgroup_swap() {
  local num_nodes=${#NODE_HOSTS[@]}
  for (( i=0; i<num_nodes; i++ )); do
    local h="${NODE_HOSTS[$i]}"
    echo "=== [Phase1] Enabling cgroup memory+swap on $h ==="
    ssh_node "$i" "bash -s" << 'EOF'
set -euo pipefail

sudo cp /etc/default/grub /etc/default/grub.bak.$(date +%s)

# Ensure keys exist
grep -q '^GRUB_CMDLINE_LINUX=' /etc/default/grub || \
  echo 'GRUB_CMDLINE_LINUX=""' | sudo tee -a /etc/default/grub >/dev/null
grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' /etc/default/grub || \
  echo 'GRUB_CMDLINE_LINUX_DEFAULT=""' | sudo tee -a /etc/default/grub >/dev/null

# Append flags if not already present (both variables)
sudo sed -i -E '/^GRUB_CMDLINE_LINUX="/ {
  /cgroup_enable=memory/ ! s/"$/ cgroup_enable=memory swapaccount=1"/
}' /etc/default/grub

sudo sed -i -E '/^GRUB_CMDLINE_LINUX_DEFAULT="/ {
  /cgroup_enable=memory/ ! s/"$/ cgroup_enable=memory swapaccount=1"/
}' /etc/default/grub

sudo update-grub
EOF
    echo "Rebooting $h ..."
    ssh_node "$i" "sudo reboot" || true
  done

  echo
  echo "Phase 1 completed. Wait for all nodes to reboot, then run:"
  echo "  $0 phase2"
}

#############################################
# Generate portal.list files (controller)
#############################################

build_portal_lists() {
  local outdir="portal_lists"
  rm -rf "$outdir"
  mkdir -p "$outdir"

  # Helper: create portal.list for given node index
  build_portal_for_node() {
    local self_idx="$1"
    local out_file="$2"
    local entries=()

    for d in "${DAEMON_IDX[@]}"; do
      # Skip self: must be *remote*
      if [[ "$d" -eq "$self_idx" ]]; then
        continue
      fi
      entries+=( "${NODE_IPS[$d]}:9400" )
    done

    local n=${#entries[@]}
    {
      echo "$n"
      for e in "${entries[@]}"; do
        echo "$e"
      done
    } > "$out_file"
  }

  for i in "${BD_IDX[@]}"; do
    local node_dir="$outdir/node${i}"
    mkdir -p "$node_dir"
    local out_file="$node_dir/portal.list"
    build_portal_for_node "$i" "$out_file"

    echo "Generated $out_file for node $i (${NODE_HOSTS[$i]}, ip=${NODE_IPS[$i]}, role=${NODE_ROLES[$i]}):"
    cat "$out_file"
    echo "--------"
  done
}

copy_portal_lists() {
  local outdir="portal_lists"
  for i in "${BD_IDX[@]}"; do
    local h="${NODE_HOSTS[$i]}"
    local src="${outdir}/node${i}/portal.list"
    if [[ ! -f "$src" ]]; then
      echo "Skipping node $i ($h): no $src"
      continue
    fi
    echo "Copying $src → $h:$REMOTE_SETUP_DIR/portal.list"
    ssh_node "$i" "mkdir -p '$REMOTE_SETUP_DIR'"
    scp_to_node "$i" "$src" "$REMOTE_SETUP_DIR/portal.list"
  done
}

#############################################
# Remote snippets (Phase 2)
#############################################

remote_install_base_packages() {
  ssh_node "$1" "bash -s" << 'EOF'
set -euo pipefail

# Try to fix any half-configured packages non-interactively
sudo DEBIAN_FRONTEND=noninteractive apt-get -y -o Dpkg::Options::="--force-confold" -f install || true

# Update package index
sudo apt-get update

# Cache kernel version
KVER=$(uname -r)

# Install dependencies (note: no 'kmod' here)
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential autoconf automake libtool pkg-config \
  libevent-dev libsasl2-dev zlib1g-dev \
  linux-headers-"$KVER" flex bison libelf-dev libssl-dev bc cpio \
  curl ca-certificates wget git tmux numactl net-tools \
  libibverbs1 libibverbs-dev \
  librdmacm1 librdmacm-dev \
  ibverbs-utils perftest infiniband-diags \
  libmlx4-1 ethtool tcpdump mstflint cgroup-tools scons gengetopt

unzip -o Archive4.zip

sudo rm Archive4.zip

sudo pkill screen || true
EOF
}

remote_load_rdma_modules() {
  ssh_node "$1" "bash -s" << 'EOF'
set -euo pipefail
sudo modprobe ib_core || true
sudo modprobe ib_uverbs || true
sudo modprobe rdma_ucm || true
sudo modprobe ib_cm || true

sudo modprobe mlx4_core || true
sudo modprobe mlx4_en   || true
sudo modprobe mlx4_ib   || true

sudo ibv_devices || true
EOF
}

remote_start_daemon_screen() {
  local idx="$1"
  local ip="${NODE_IPS[$idx]}"

  # Pass IP and ISROOT as positional args to remote bash
  ssh_node "$idx" "bash -s '$ip' '$REMOTE_ISROOT'" << 'EOF'
set -euo pipefail
IP="$1"
ROOT="$2"

echo "[daemon] Using ROOT=$ROOT IP=$IP"

cd "$ROOT/infiniswap_daemon"

sudo ./autogen.sh || true
sudo ./configure || true
sudo make -j"$(nproc)" || true

# Kill old daemon + old screen if any
sudo pgrep -f 'infiniswap-daemon' | xargs -r sudo kill -9 || true
screen -S is_daemon -X quit || true

# Start a detached screen; it will start the default login shell (bash)
screen -dmS is_daemon

# Configure screen logging for this session
# screen -S is_daemon -X logfile /tmp/infiniswap-daemon.log
# screen -S is_daemon -X log on

# Send the command into the screen window, then an Enter
echo "[daemon] Starting infiniswap-daemon in screen 'is_daemon'"
screen -S is_daemon -X stuff "cd \"$ROOT/infiniswap_daemon\" && sudo ./infiniswap-daemon "$IP" 9400 "`echo -ne '\015'`

EOF
}

remote_wait_daemon_ready() {
  local idx="$1"
  local host="${NODE_HOSTS[$idx]}"

  echo "[daemon] Waiting for infiniswap-daemon to be running on $host ..."

  ssh_node "$idx" "bash -s" << 'EOF'
set -euo pipefail
TRIES=30
SLEEP=2

for i in $(seq 1 "$TRIES"); do
  if pgrep -f infiniswap-daemon >/dev/null 2>&1; then
    echo "[daemon] infiniswap-daemon process is running"
    exit 0
  fi
  sleep "$SLEEP"
done

echo "[daemon] WARNING: infiniswap-daemon did not appear after $((TRIES*SLEEP)) seconds" >&2
exit 1
EOF
}

remote_setup_bd() {
  local idx="$1"

  ssh_node "$idx" "bash -s '$REMOTE_LOOP_IMG' '$REMOTE_IS_SIZE_GB' '$REMOTE_ISROOT'" << 'EOF'
set -euo pipefail

IS_IMG="$1"
IS_SIZE_GB="$2"
ISROOT="$3"

echo "[bd] Using IS_IMG=$IS_IMG SIZE=${IS_SIZE_GB}G ROOT=$ISROOT"

sudo mkdir -p "$(dirname "$IS_IMG")"

# Create loopfile if missing
if [[ ! -f "$IS_IMG" ]]; then
  echo "[bd] Creating loopfile $IS_IMG (${IS_SIZE_GB}G)..."
  sudo fallocate -l "${IS_SIZE_GB}G" "$IS_IMG" 2>/dev/null || \
    sudo dd if=/dev/zero of="$IS_IMG" bs=1M count=0 seek=$((IS_SIZE_GB*1024))
fi

sudo losetup -fP "$IS_IMG"
IS_LOOP=$(sudo losetup -j "$IS_IMG" | cut -d: -f1)
echo "[bd] Using loop device: $IS_LOOP"

cd "$ISROOT/setup"

# Build BD
sudo ./install.sh bd

sudo modprobe configfs || true
sudo insmod "$ISROOT/infiniswap_bd/infiniswap.ko" 2>/dev/null || sudo modprobe infiniswap || true
mount | grep -q ' type configfs ' || sudo mount -t configfs none /sys/kernel/config || true

# Configure BD via configfs
sudo ./infiniswap_bd_setup.sh

# Use infiniswap0 as highest-priority swap
sudo swapoff -a || true
sudo swapon --priority 32767 /dev/infiniswap0
sudo swapon --show
EOF
}

remote_start_memcached_screen() {
  local idx="$1"
  local role="${NODE_ROLES[$idx]}"
  local host="${NODE_HOSTS[$idx]}"

  ssh_node "$idx" "bash -s '$MEMCACHED_VER' '$PREFIX' '$role'" << 'EOF'
set -euo pipefail

MEMCACHED_VER="$1"
PREFIX="$2"
ROLE="$3"

echo "[memcached] HOST=$(hostname) MEMCACHED_VER=$MEMCACHED_VER PREFIX=$PREFIX ROLE=$ROLE"

# Ensure memcached is built (no-op if already installed)
cd /tmp
if ! "$PREFIX/bin/memcached" -h >/dev/null 2>&1; then
  echo "[memcached] Building memcached from source..."
  curl -fsSL "https://memcached.org/files/memcached-${MEMCACHED_VER}.tar.gz" -o "memcached-${MEMCACHED_VER}.tar.gz"
  tar xzf "memcached-${MEMCACHED_VER}.tar.gz"
  cd "memcached-${MEMCACHED_VER}"
  ./configure --prefix="${PREFIX}" --enable-64bit
  make -j"$(nproc)"
  sudo make install
fi

# Create cgroups
sudo cgcreate -g memory:/memc 2>/dev/null || true
sudo cgcreate -g memory:/memc2 2>/dev/null || true

# Limits based on ROLE
case "$ROLE" in
  # Under-provisioned server: both tenants tight
  U|u)
    LIMIT_MB1=64   # memc  (11212)
    MEMSW_MB1=6000
    LIMIT_MB2=64   # memc2 (11213)
    MEMSW_MB2=6000
    ;;
  # Over-provisioned server: both tenants relaxed
  O|o)
    LIMIT_MB1=6000
    MEMSW_MB1=8000
    LIMIT_MB2=6000
    MEMSW_MB2=8000
    ;;
  # Mixed server: 11212 relaxed (O-like), 11213 tight (U-like)
  M|m)
    LIMIT_MB1=64   # memc  (11212) - low limit
    MEMSW_MB1=8000
    LIMIT_MB2=6000   # memc2 (11213) - high limit
    MEMSW_MB2=6000
    ;;
  # Default fallback
  *)
    LIMIT_MB1=1000
    MEMSW_MB1=6000
    LIMIT_MB2=1000
    MEMSW_MB2=6000
    ;;
esac

echo "[memcached] Setting cgroup limits: memc=${LIMIT_MB1}MB/${MEMSW_MB1}MB, memc2=${LIMIT_MB2}MB/${MEMSW_MB2}MB"

sudo cgset -r memory.limit_in_bytes=$(( LIMIT_MB1 * 1024 * 1024 )) memc
sudo cgset -r memory.memsw.limit_in_bytes=$(( MEMSW_MB1 * 1024 * 1024 )) memc
sudo cgset -r memory.swappiness=100 memc

sudo cgset -r memory.limit_in_bytes=$(( LIMIT_MB2 * 1024 * 1024 )) memc2
sudo cgset -r memory.memsw.limit_in_bytes=$(( MEMSW_MB2 * 1024 * 1024 )) memc2
sudo cgset -r memory.swappiness=100 memc2

# Kill old memcached + screens
sudo pgrep -f 'memcached' | xargs -r sudo kill -9 || true
screen -S memc11212 -X quit 2>/dev/null || true
screen -S memc11213 -X quit 2>/dev/null || true

# Screen for memcached on 11212
screen -dmS memc11212
# screen -S memc11212 -X logfile /tmp/memc11212.log
# screen -S memc11212 -X log on
screen -S memc11212 -X stuff "sudo cgexec -g memory:memc \"$PREFIX/bin/memcached\" -u root -m 5000 -t 4 -p 11212"`echo -ne '\015'`

# Screen for memcached on 11213
screen -dmS memc11213
# screen -S memc11213 -X logfile /tmp/memc11213.log
# screen -S memc11213 -X log on
screen -S memc11213 -X stuff "sudo cgexec -g memory:memc2 \"$PREFIX/bin/memcached\" -u root -m 5000 -t 4 -p 11213"`echo -ne '\015'`
EOF
}

remote_start_benchmark() {
  local idx="$1"
  local role="${NODE_ROLES[$idx]}"
  local host="${NODE_HOSTS[$idx]}"

  ssh_node "$idx" "bash -s '$role'" << 'EOF'
set -euo pipefail

ROLE="$1"
BASE="/users/AMH/mydata/scripts/"

case "$ROLE" in
  U|u)
    echo "[bench] U-server: starting run_bench_U.sh"
    nohup bash "$BASE/run_mutilate_inf.sh" &
    ;;
  O|o)
    echo "[bench] O-server: starting run_bench_O.sh"
    nohup bash "$BASE/run_mutilate_inf.sh" &
    ;;
  M|m)
    echo "[bench] M-server: starting run_bench_M.sh"
    nohup bash "$BASE/run_mutilate_inf.sh" &
    ;;
  *)
    echo "[bench] Unknown role '$ROLE', not starting any benchmark" >&2
    ;;
esac
EOF
}

#############################################
# Phase 2: everything after reboot
#############################################

phase2_setup_all() {
  local num_nodes=${#NODE_HOSTS[@]}

  echo "=== Phase2: install base packages + RDMA modules on all nodes ==="
  for (( i=0; i<num_nodes; i++ )); do
    echo "--- Node $i: ${NODE_HOSTS[$i]} ---"
    remote_install_base_packages "$i"
    remote_load_rdma_modules "$i"
  done

  echo
  echo "=== Build and copy portal.list files (controller) ==="
  build_portal_lists
  copy_portal_lists

  echo
  echo "=== Start Infiniswap daemon (screen) on O+M nodes ==="
  for i in "${DAEMON_IDX[@]}"; do
    echo "--- Node $i: ${NODE_HOSTS[$i]} (daemon) ---"
    remote_start_daemon_screen "$i"
  done

  echo
  echo "=== Wait for all daemons to be ready on port 9400 ==="
  for i in "${DAEMON_IDX[@]}"; do
    remote_wait_daemon_ready "$i"
  done

  echo
  echo "=== Setup Infiniswap BD + swap on U+M nodes ==="
  for i in "${BD_IDX[@]}"; do
    echo "--- Node $i: ${NODE_HOSTS[$i]} (bd) ---"
    remote_setup_bd "$i"
  done

  echo
  echo "=== Start memcached (screen) on all nodes ==="
  for (( i=0; i<num_nodes; i++ )); do
    echo "--- Node $i: ${NODE_HOSTS[$i]} (role=${NODE_ROLES[$i]}) ---"
    remote_start_memcached_screen "$i"
  done

  echo
  echo "=== Start benchmarks on all nodes (by role) ==="
  for (( i=0; i<num_nodes; i++ )); do
    echo "--- Node $i: ${NODE_HOSTS[$i]} (role=${NODE_ROLES[$i]}) ---"
    remote_start_benchmark "$i"
  done

  echo
  echo "Phase 2 completed. Daemons + memcached + benchmarks should now be running."

}

#############################################
# Main
#############################################

main() {
  if [[ $# -lt 1 ]]; then
    echo "Usage: $0 {phase1|phase2}"
    exit 1
  fi

  require_tools
  load_nodes_and_ips
  load_roles

  case "$1" in
    phase1)
      phase1_enable_cgroup_swap
      ;;
    phase2)
      phase2_setup_all
      ;;
    *)
      echo "Unknown mode: $1 (use phase1 or phase2)"
      exit 1
      ;;
  esac
}

main "$@"