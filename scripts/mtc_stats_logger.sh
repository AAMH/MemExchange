#!/usr/bin/env bash
set -euo pipefail

INTERVAL=1
OUTBASE="/users/AMH/Log/mtc_resize_overhead"
IFACE="auto"
IBDEV="mlx5_2"
IBPORT="1"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --interval) INTERVAL="$2"; shift 2;;
    --outbase) OUTBASE="$2"; shift 2;;
    --iface) IFACE="$2"; shift 2;;
    --ibdev) IBDEV="$2"; shift 2;;
    --ibport) IBPORT="$2"; shift 2;;
    *) echo "Unknown arg: $1" >&2; exit 1;;
  esac
done

HOST="$(hostname -s)"
OUTDIR="${OUTBASE}/${HOST}"
mkdir -p "$OUTDIR"
OUTCSV="${OUTDIR}/stats_1hz.csv"

# --- helpers ---
read_u64() { cat "$1" 2>/dev/null || echo 0; }

# Auto-detect iface that has 10.10.1.x if requested
if [[ "$IFACE" == "auto" ]]; then
  IFACE="$(ip -o -4 addr show | awk '$4 ~ /^10\.10\.1\./ {print $2; exit}')"
  if [[ -z "${IFACE:-}" ]]; then
    echo "ERROR: Could not auto-detect 10.10.1.x interface. Use --iface <name>." >&2
    exit 1
  fi
fi

# --- IB counter directory selection ---
IBROOT="/sys/class/infiniband/${IBDEV}/ports/${IBPORT}"
IBPATH_HW="${IBROOT}/hw_counters"
IBPATH_SW="${IBROOT}/counters"

# If the specified dev/port doesn't exist, auto-pick one
if [[ ! -d "$IBROOT" ]]; then
  found=""
  for d in /sys/class/infiniband/*; do
    [[ -d "$d" ]] || continue
    dev="$(basename "$d")"
    for p in "$d"/ports/*; do
      [[ -d "$p" ]] || continue
      port="$(basename "$p")"
      IBDEV="$dev"
      IBPORT="$port"
      IBROOT="/sys/class/infiniband/${IBDEV}/ports/${IBPORT}"
      IBPATH_HW="${IBROOT}/hw_counters"
      IBPATH_SW="${IBROOT}/counters"
      found="yes"
      break 2
    done
  done
  if [[ -z "$found" ]]; then
    echo "WARN: No /sys/class/infiniband/*/ports/* found; RDMA stats will be 0." >&2
    IBROOT=""
    IBPATH_HW=""
    IBPATH_SW=""
  fi
fi

# Choose primary/fallback dirs: prefer hw_counters if present
IBPATH_PRIMARY=""
IBPATH_FALLBACK=""
if [[ -n "${IBROOT:-}" ]]; then
  if [[ -d "$IBPATH_HW" ]]; then
    IBPATH_PRIMARY="$IBPATH_HW"
    IBPATH_FALLBACK="$IBPATH_SW"
  else
    IBPATH_PRIMARY="$IBPATH_SW"
    IBPATH_FALLBACK="$IBPATH_HW"
  fi
fi

# Pick a file path for a given counter name (primary if exists, else fallback)
ib_counter_path() {
  local name="$1"
  if [[ -n "${IBPATH_PRIMARY:-}" && -f "${IBPATH_PRIMARY}/${name}" ]]; then
    echo "${IBPATH_PRIMARY}/${name}"
  elif [[ -n "${IBPATH_FALLBACK:-}" && -f "${IBPATH_FALLBACK}/${name}" ]]; then
    echo "${IBPATH_FALLBACK}/${name}"
  else
    echo ""
  fi
}

# RDMA counters to log (subset)
# A) traffic volume
# B) op mix
# C) congestion/retrans/timeouts (useful later for stress tests too)
IB_COUNTERS=(
  port_xmit_data
  port_rcv_data
  port_xmit_packets
  port_rcv_packets
  unicast_xmit_packets
  unicast_rcv_packets
  rx_write_requests
  rx_read_requests
  rnr_nak_retry_err
  local_ack_timeout_err
  out_of_sequence
  packet_seq_err
  np_cnp_sent
  rp_cnp_handled
)

NETRXB="/sys/class/net/${IFACE}/statistics/rx_bytes"
NETTXB="/sys/class/net/${IFACE}/statistics/tx_bytes"
NETRXP="/sys/class/net/${IFACE}/statistics/rx_packets"
NETTXP="/sys/class/net/${IFACE}/statistics/tx_packets"

# Read UDP global counters from /proc/net/snmp (InDatagrams, OutDatagrams, RcvbufErrors, SndbufErrors)
read_udp_snmp() {
  awk '
    $1=="Udp:"{
      if (seen==0){
        for(i=2;i<=NF;i++) k[i]=$i;
        seen=1;
      } else {
        for(i=2;i<=NF;i++) v[k[i]]=$i;
      }
    }
    END{
      printf "%s,%s,%s,%s\n",
        (v["InDatagrams"]==""?0:v["InDatagrams"]),
        (v["OutDatagrams"]==""?0:v["OutDatagrams"]),
        (v["RcvbufErrors"]==""?0:v["RcvbufErrors"]),
        (v["SndbufErrors"]==""?0:v["SndbufErrors"]);
    }
  ' /proc/net/snmp
}

# CPU from /proc/stat delta (overall utilization %)
read_cpu_stat() { awk '/^cpu /{print $2,$3,$4,$5,$6,$7,$8,$9,$10,$11}' /proc/stat; }

cpu_prev=($(read_cpu_stat))
tot_prev=0
idle_prev=0
for x in "${cpu_prev[@]}"; do tot_prev=$((tot_prev + x)); done
idle_prev=$((cpu_prev[3] + cpu_prev[4]))  # idle + iowait

# initial snapshots for deltas
net_rxb_prev=$(read_u64 "$NETRXB"); net_txb_prev=$(read_u64 "$NETTXB")
net_rxp_prev=$(read_u64 "$NETRXP"); net_txp_prev=$(read_u64 "$NETTXP")

udp_prev=$(read_udp_snmp)
udp_in_prev=$(echo "$udp_prev" | cut -d, -f1)
udp_out_prev=$(echo "$udp_prev" | cut -d, -f2)
udp_rcvbuf_prev=$(echo "$udp_prev" | cut -d, -f3)
udp_sndbuf_prev=$(echo "$udp_prev" | cut -d, -f4)

# per-counter previous values
declare -A ib_prev
for c in "${IB_COUNTERS[@]}"; do
  path="$(ib_counter_path "$c")"
  if [[ -n "$path" ]]; then
    ib_prev["$c"]="$(read_u64 "$path")"
  else
    ib_prev["$c"]="0"
  fi
done

# write header
if [[ ! -f "$OUTCSV" ]]; then
  header="ts,host,iface,ibdev,ibport,cpu_pct,load1,load5,load15,net_rx_bytes_d,net_tx_bytes_d,net_rx_pkts_d,net_tx_pkts_d,udp_in_d,udp_out_d,udp_rcvbuf_err_d,udp_sndbuf_err_d"
  for c in "${IB_COUNTERS[@]}"; do
    header+=",ib_${c}_d"
  done
  echo "$header" > "$OUTCSV"
fi

echo "Logging to $OUTCSV"
echo "IFACE=$IFACE IB=${IBDEV}:${IBPORT} (primary=$(basename "${IBPATH_PRIMARY:-none}"))"

while true; do
  ts=$(date +%s)

  # CPU delta
  cpu_now=($(read_cpu_stat))
  tot_now=0
  for x in "${cpu_now[@]}"; do tot_now=$((tot_now + x)); done
  idle_now=$((cpu_now[3] + cpu_now[4]))

  dt_tot=$((tot_now - tot_prev))
  dt_idle=$((idle_now - idle_prev))
  if [[ $dt_tot -le 0 ]]; then
    cpu_pct="0.0"
  else
    cpu_pct=$(awk -v dt_tot="$dt_tot" -v dt_idle="$dt_idle" 'BEGIN{printf "%.2f", 100.0*(dt_tot-dt_idle)/dt_tot}')
  fi
  tot_prev=$tot_now
  idle_prev=$idle_now

  # load avg
  read -r load1 load5 load15 _ < /proc/loadavg

  # NET deltas
  net_rxb=$(read_u64 "$NETRXB"); net_txb=$(read_u64 "$NETTXB")
  net_rxp=$(read_u64 "$NETRXP"); net_txp=$(read_u64 "$NETTXP")
  net_rxb_d=$((net_rxb - net_rxb_prev)); net_txb_d=$((net_txb - net_txb_prev))
  net_rxp_d=$((net_rxp - net_rxp_prev)); net_txp_d=$((net_txp - net_txp_prev))
  net_rxb_prev=$net_rxb; net_txb_prev=$net_txb
  net_rxp_prev=$net_rxp; net_txp_prev=$net_txp

  # UDP deltas
  udp_now=$(read_udp_snmp)
  udp_in=$(echo "$udp_now" | cut -d, -f1)
  udp_out=$(echo "$udp_now" | cut -d, -f2)
  udp_rcvbuf=$(echo "$udp_now" | cut -d, -f3)
  udp_sndbuf=$(echo "$udp_now" | cut -d, -f4)
  udp_in_d=$((udp_in - udp_in_prev))
  udp_out_d=$((udp_out - udp_out_prev))
  udp_rcvbuf_d=$((udp_rcvbuf - udp_rcvbuf_prev))
  udp_sndbuf_d=$((udp_sndbuf - udp_sndbuf_prev))
  udp_in_prev=$udp_in; udp_out_prev=$udp_out
  udp_rcvbuf_prev=$udp_rcvbuf; udp_sndbuf_prev=$udp_sndbuf

  # Build row
  row="$ts,$HOST,$IFACE,$IBDEV,$IBPORT,$cpu_pct,$load1,$load5,$load15,$net_rxb_d,$net_txb_d,$net_rxp_d,$net_txp_d,$udp_in_d,$udp_out_d,$udp_rcvbuf_d,$udp_sndbuf_d"

  # IB deltas
  for c in "${IB_COUNTERS[@]}"; do
    path="$(ib_counter_path "$c")"
    cur="0"
    if [[ -n "$path" ]]; then
      cur="$(read_u64 "$path")"
    fi
    prev="${ib_prev[$c]}"
    d=$((cur - prev))
    ib_prev["$c"]="$cur"
    row+=",${d}"
  done

  echo "$row" >> "$OUTCSV"
  sleep "$INTERVAL"
done
