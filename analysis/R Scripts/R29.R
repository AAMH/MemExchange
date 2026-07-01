# ============================================================
# MemExchange Stress Test Analysis (CloudSuite + RTT + NIC logs)
# - Produces plots A–D + useful tables
# - Saves everything under out_dir
# ============================================================

suppressPackageStartupMessages({
  library(data.table)
  library(ggplot2)
  library(dplyr)
})

# -------------------------
# User-configurable settings
# -------------------------
root_dir  <- "."                  # folder that contains: cloudsuite/, rtt/, mtc_resize_overhead/
out_dir   <- file.path(root_dir, "analysis_out")
dir.create(out_dir, showWarnings = FALSE, recursive = TRUE)

victim_node <- "node9"
victim_ip   <- "10.10.1.10"

# node2 not used; victors are "all nodes except victim and node2"
unused_node <- ""

# Saturation detector settings
sat_hold_seconds  <- 1           # must stay near max for this many seconds
sat_frac_of_max   <- 0.97         # "near max" threshold (97% of max)
min_run_seconds   <- 30           # ignore early start-up if needed

# Optional: set to TRUE if you prefer PDF output instead of PNG
use_pdf <- FALSE

# -------------------------
# Helpers
# -------------------------
parse_ts_to_seconds <- function(x) {
  as.numeric(trimws(as.character(x)))
}

save_plot <- function(p, filename, w = 10, h = 5) {
  f <- file.path(out_dir, filename)
  if (use_pdf) {
    ggsave(sub("\\.png$", ".pdf", f), p, width = w, height = h, device = cairo_pdf)
  } else {
    ggsave(f, p, width = w, height = h, dpi = 200)
  }
}

robust_quantile <- function(x, probs) {
  x <- x[is.finite(x)]
  if (!length(x)) return(rep(NA_real_, length(probs)))
  as.numeric(quantile(x, probs = probs, na.rm = TRUE, names = FALSE, type = 7))
}

# -------------------------
# Discover files
# -------------------------
cloudsuite_dir <- file.path(root_dir, "cloudsuite")
rtt_dir        <- file.path(root_dir, "rtt")
ext_root       <- file.path(root_dir, "mtc_resize_overhead")

stopifnot(dir.exists(cloudsuite_dir), dir.exists(rtt_dir), dir.exists(ext_root))
rtt_files        <- list.files(rtt_dir,        pattern = "_rtt$",                 full.names = TRUE)

# External logs: mtc_resize_overhead/nodeX/stats_1hz.csv
ext_nodes <- list.dirs(ext_root, full.names = TRUE, recursive = FALSE)
ext_files <- file.path(ext_nodes, "stats_1hz.csv")
ext_files <- ext_files[file.exists(ext_files)]

# -------------------------
# Read CloudSuite logs
# -------------------------
cloudsuite_files <- list.files(
  "cloudsuite",
  pattern = "^cloudsuite_.*_11212\\.csv$",
  full.names = TRUE
)

cloudsuite <- rbindlist(lapply(cloudsuite_files, function(path) {
  dt <- fread(path)
  
  # normalize column names (IMPORTANT)
  setnames(dt, trimws(names(dt)))
  
  # sanity check
  if (!"ts" %in% names(dt)) {
    stop("ts column missing in ", path)
  }
  
  # parse timestamp
  dt[, ts_sec := as.numeric(trimws(ts))]
  
  # extract IP from filename
  nm <- basename(path)
  ip <- sub("^cloudsuite_", "", nm)
  ip <- sub("_11212\\.csv$", "", ip)
  dt[, tenant_ip := ip]
  
  dt
}), fill = TRUE)


summary(cloudsuite$ts_sec)
# Normalize time (relative) for plotting
t0 <- min(cloudsuite$ts_sec, na.rm = TRUE)
cloudsuite[, t := ts_sec - t0]

# Derived metrics
cloudsuite[, hit_rate := fifelse(gets > 0, hits / gets, NA_real_)]
cloudsuite[, miss_rate := fifelse(gets > 0, misses / gets, NA_real_)]

stopifnot("ts_sec" %in% names(cloudsuite))

# -------------------------
# Read RTT logs (victors only)
# -------------------------
read_rtt <- function(path) {
  # filename: "<ip>_rtt"
  nm <- basename(path)
  ip <- sub("_rtt$", "", nm)
  
  dt <- fread(path, col.names = c("ts", "rtt_ns", "status"))
  dt[, tenant_ip := ip]
  dt[, ts_sec := parse_ts_to_seconds(ts)]
  dt <- dt[is.finite(ts_sec)]
  dt[, t := ts_sec - t0]
  dt
}

rtt <- rbindlist(lapply(rtt_files, read_rtt), fill = TRUE)

# -------------------------
# Read External logs (all tenants)
# -------------------------
read_ext <- function(path) {
  node <- basename(dirname(path))
  dt <- fread(path)
  
  # Columns as given:
  # ts,host,iface,ibdev,ibport,cpu_pct,load1,load5,load15,net_rx_bytes_d,net_tx_bytes_d,
  # net_rx_pkts_d,net_tx_pkts_d,udp_in_d,udp_out_d,udp_rcvbuf_err_d,udp_sndbuf_err_d,
  # ib_port_xmit_data_d,ib_port_rcv_data_d,ib_port_xmit_packets_d,ib_port_rcv_packets_d,
  # ib_unicast_xmit_packets_d,ib_unicast_rcv_packets_d,ib_rx_write_requests_d,ib_rx_read_requests_d,
  # ib_rnr_nak_retry_err_d,ib_local_ack_timeout_err_d,ib_out_of_sequence_d,ib_packet_seq_err_d,
  # ib_np_cnp_sent_d,ib_rp_cnp_handled_d
  
  dt[, node := node]
  dt[, ts_sec := parse_ts_to_seconds(ts)]
  dt <- dt[is.finite(ts_sec)]
  dt[, t := ts_sec - t0]
  
  # Compute per-row dt (seconds) from timestamps to get throughput in Gb/s
  setorder(dt, ts_sec)
  dt[, dt_s := c(NA_real_, diff(ts_sec))]
  # Convert "ib_port_*_data" units of 4 bytes to bits/sec
  dt[, rdma_tx_gbps := (ib_port_xmit_data_d * 4 * 8) / dt_s / 1e9]
  dt[, rdma_rx_gbps := (ib_port_rcv_data_d  * 4 * 8) / dt_s / 1e9]
  
  # Sometimes dt_s could be 0/NA at first row; clamp
  dt[!is.finite(rdma_tx_gbps) | rdma_tx_gbps < 0, rdma_tx_gbps := NA_real_]
  dt[!is.finite(rdma_rx_gbps) | rdma_rx_gbps < 0, rdma_rx_gbps := NA_real_]
  
  dt
}

ext <- rbindlist(lapply(ext_files, read_ext), fill = TRUE)

# Identify victim ext log
victim_ext <- ext[node == victim_node]
victim_ext[, rdma_tx_gbps := pmin(rdma_tx_gbps, 25)]
stopifnot(nrow(victim_ext) > 10)

# -------------------------
# Detect saturation time T_sat from victim rdma throughput plateau
# -------------------------
# Strategy:
# 1) ignore first min_run_seconds
# 2) compute max rdma_tx_gbps
# 3) find earliest time where rdma_tx_gbps stays >= sat_frac_of_max * max for sat_hold_seconds
detect_Tsat <- function(dt) {
  d <- copy(dt)
  d <- d[t >= min_run_seconds & is.finite(rdma_tx_gbps)]
  if (nrow(d) < 5) return(NA_real_)
  
  mx <- max(d$rdma_tx_gbps, na.rm = TRUE)
  thr <- sat_frac_of_max * mx
  
  d[, near := rdma_tx_gbps >= thr]
  # compute consecutive run length of near==TRUE
  rle_near <- rle(d$near)
  ends <- cumsum(rle_near$lengths)
  starts <- ends - rle_near$lengths + 1
  
  # find first TRUE run whose duration (in seconds) >= sat_hold_seconds
  for (i in which(rle_near$values)) {
    s <- starts[i]; e <- ends[i]
    t_span <- d$t[e] - d$t[s]
    if (is.finite(t_span) && t_span >= sat_hold_seconds) {
      return(d$t[s])  # first time entering sustained plateau
    }
  }
  NA_real_
}

T_sat <- detect_Tsat(victim_ext)

# Pre/Post windows around T_sat (in seconds)
pre_window     <- 3000   # 5 min
post_window    <- 3000   # 5 min
post_burnin    <- 0   # 10 min

pre_start   <- max(0, T_sat - pre_window)
pre_end     <- T_sat

post_start  <- T_sat + post_burnin
post_end    <- post_start + post_window

# Save T_sat
writeLines(sprintf("T_sat (seconds since start t0): %s", ifelse(is.na(T_sat), "NA", format(T_sat, digits = 8))),
           con = file.path(out_dir, "Tsat.txt"))

# Define pre/post windows
if (is.finite(T_sat)) {
  pre_start  <- max(0, T_sat - pre_window)
  pre_end    <- T_sat
  post_start <- T_sat + post_burnin
  post_end   <- post_start + post_window
} else {
  # fallback: use middle of trace
  mid <- median(victim_ext$t, na.rm = TRUE)
  pre_start  <- max(0, mid - pre_window)
  pre_end    <- mid
  post_start <- mid
  post_end   <- mid + post_window
}

# -------------------------
# Define 3 analysis phases
# -------------------------
post0_start <- T_sat
post0_end   <- T_sat + post_burnin # post_window   # transient window

phase_of_t <- function(t) {
  fifelse(
    t >= pre_start & t < pre_end, "pre",
    fifelse(
      t >= post0_start & t < post0_end, "post_transient",
      fifelse(
        t >= post_start & t < post_end, "post_steady",
        NA_character_
      )
    )
  )
}
# -------------------------
# Plot A — Victim RDMA throughput ramp → plateau
# -------------------------
# -------------------------
# Plot A — Victim RDMA throughput (5s bins, mean + p10–p90)
# -------------------------
bin_s <- 5

victim_ds <- victim_ext[
  is.finite(rdma_tx_gbps),
  .(
    tx_mean = mean(rdma_tx_gbps, na.rm = TRUE),
    tx_median = median(rdma_tx_gbps, na.rm = TRUE),
    tx_p10  = quantile(rdma_tx_gbps, 0.10, na.rm = TRUE),
    tx_p90  = quantile(rdma_tx_gbps, 0.90, na.rm = TRUE)
  ),
  by = .(t_bin = floor(t / bin_s) * bin_s)
]

# 🔑 DROP pre-traffic bins
victim_ds <- victim_ds[tx_p90 > 0]

ylim_max <- max(victim_ds$tx_p90, na.rm = TRUE) * 1.05

pA <- ggplot(victim_ds, aes(x = t_bin)) +
  geom_ribbon(
    aes(ymin = tx_p10, ymax = tx_p90),
    # fill = "steelblue",
    alpha = 0.30
  ) +
  geom_line(
    aes(y = tx_median),
    color = "black",
    linewidth = 1
  ) +
  labs(
    title = "",#"Victim RDMA Throughput Over Time",
    x = "Time (s)",
    y = "RDMA Throughput (Gb/s)",
    caption = "5-second bins; median with 10th–90th percentile band."
  ) +
  coord_cartesian(ylim = c(0, ylim_max), xlim = c(0, 5000)) +
  theme_bw(base_size = 12) +
  theme(axis.text.x = element_text(hjust = 1, size = 10, face = "bold"),
        axis.text.y = element_text(size = 10, face = "bold"),
        axis.title.x = element_text(size = 15, face = "bold"),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 15, face = "bold"),  # Bold and larger Y-axis label
        plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
        legend.key.size = unit(0.7, "cm"),  # Enlarge the legend keys
        legend.text = element_text(size = 10),  # Increase the text size
        legend.title = element_text(size = 12, face = "bold"),
        legend.position = "bottom") +
  guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15)))  # Increase legend key size

if (is.finite(T_sat)) {
  pA <- pA +
    geom_vline(xintercept = T_sat, linetype = "dashed", linewidth = 0.8) +
    annotate(
      "text",
      x = T_sat,
      y = ylim_max * 0.95,   # 👈 inside the plot
      label = "Bandwidth saturation",
      angle = 90,
      vjust = 1.5,
      hjust = 1.5,
      size = 3.5
    )
}
save_plot(pA, "plotA_victim_rdma_throughput_binned.png", w = 8, h = 5)


# Optional overlay of congestion markers
if ("ib_np_cnp_sent_d" %in% names(victim_ext)) {
  pA2 <- ggplot(victim_ext, aes(x = t)) +
    geom_line(aes(y = rdma_tx_gbps), linewidth = 0.4) +
    geom_line(aes(y = ib_np_cnp_sent_d), linewidth = 0.4) +
    labs(
      title = sprintf("Victim RDMA TX vs CNP Sent (%s)", victim_node),
      x = "Time (s, rebased)",
      y = "Value (Gb/s or count per sample)",
      caption = "CNP is plotted on same axis (interpret trend, not magnitude)."
    ) +
    theme_bw(base_size = 12)
  if (is.finite(T_sat)) pA2 <- pA2 + geom_vline(xintercept = T_sat, linetype = "dotted")
  save_plot(pA2, "plotA2_victim_rdma_vs_cnp.png", w = 11, h = 5)
}

# -------------------------
# Plot B — Control-plane health vs saturation (UDP metrics)
# -------------------------
udp_cols <- c("udp_in_d","udp_out_d","udp_rcvbuf_err_d","udp_sndbuf_err_d")
missing_udp <- setdiff(udp_cols, names(ext))
if (length(missing_udp)) {
  message("Warning: Missing UDP columns in external logs: ", paste(missing_udp, collapse = ", "))
}

ext_udp <- ext[, .(t, node,
                   udp_in_d, udp_out_d,
                   udp_rcvbuf_err_d, udp_sndbuf_err_d)]
ext_udp <- ext_udp[is.finite(t)]

# Long format
ext_udp_long <- melt(ext_udp,
                     id.vars = c("t","node"),
                     measure.vars = intersect(udp_cols, names(ext_udp)),
                     variable.name = "metric",
                     value.name = "value"
)

pB <- ggplot(ext_udp_long, aes(x = t, y = value)) +
  geom_line(linewidth = 0.35) +
  facet_grid(metric ~ node, scales = "free_y") +
  labs(
    title = "Plot B — UDP Control-Plane Activity & Buffer Errors (all tenants)",
    x = "Time (s, rebased)",
    y = "Delta per sample"
  ) +
  theme_bw(base_size = 10) +
  theme(strip.text.x = element_text(size = 7))

if (is.finite(T_sat)) pB <- pB + geom_vline(xintercept = T_sat, linetype = "dotted")
save_plot(pB, "plotB_udp_health_all_tenants.png", w = 14, h = 7)

# -------------------------
# Plot C — MTC RTT: pre/post distributions + late fraction
# -------------------------
# status: 0 accepted, 1 rejected, 2 late/expired
if (nrow(rtt)) {
  rtt[, phase := phase_of_t(t)]
  rtt_pp <- rtt[!is.na(phase) & is.finite(rtt_ns)]
  
  rtt_pp[, rtt_ms := rtt_ns / 1e6]
  
  # Distribution plot
  pC <- ggplot(rtt_pp, aes(x = rtt_ms, fill = phase)) +
    geom_histogram(bins = 60, position = "identity", alpha = 0.55) +
    facet_wrap(~ tenant_ip, scales = "free_y") +
    scale_x_continuous(trans = "log10") +
    labs(
      title = "Plot C — MTC RTT distributions (log-x), pre vs post saturation window",
      x = "MTC RTT (ms, log10 scale)",
      y = "Count"
    ) +
    theme_bw(base_size = 10) +
    theme(legend.position = "top")
  save_plot(pC, "plotC_mtc_rtt_pre_post_hist.png", w = 14, h = 7)
  
  # Status fraction over time (rolling-ish via binning)
  rtt_bins <- rtt[, .(
    n = .N,
    late_frac = mean(status == 2, na.rm = TRUE),
    reject_frac = mean(status == 1, na.rm = TRUE),
    accept_frac = mean(status == 0, na.rm = TRUE),
    rtt_p50_ms = robust_quantile(rtt_ns/1e6, 0.50),
    rtt_p99_ms = robust_quantile(rtt_ns/1e6, 0.99)
  ), by = .(tenant_ip, t_bin = floor(t))]
  
  rtt_bins_long <- melt(rtt_bins,
                        id.vars = c("tenant_ip", "t_bin"),
                        measure.vars = c("late_frac","reject_frac","accept_frac","rtt_p50_ms","rtt_p99_ms"),
                        variable.name = "metric",
                        value.name = "value"
  )
  
  pC2 <- ggplot(rtt_bins_long, aes(x = t_bin, y = value)) +
    geom_line(linewidth = 0.35) +
    facet_grid(metric ~ tenant_ip, scales = "free_y") +
    labs(
      title = "MTC per-second RTT (p50/p99) and status fractions over time",
      x = "Time (s, rebased, binned)",
      y = "Value"
    ) +
    theme_bw(base_size = 10) +
    theme(strip.text.x = element_text(size = 7))
  if (is.finite(T_sat)) pC2 <- pC2 + geom_vline(xintercept = T_sat, linetype = "dotted")
  save_plot(pC2, "plotC2_mtc_rtt_and_status_timeseries.png", w = 14, h = 7)
  
  # Summary table: per-victor pre vs post
  rtt_summary <- rtt_pp[, .(
    n = .N,
    rtt_p50_ms = robust_quantile(rtt_ms, 0.50),
    rtt_p90_ms = robust_quantile(rtt_ms, 0.90),
    rtt_p99_ms = robust_quantile(rtt_ms, 0.99),
    late_frac  = mean(status == 2, na.rm = TRUE),
    reject_frac = mean(status == 1, na.rm = TRUE)
  ), by = .(tenant_ip, phase)]
  
  fwrite(rtt_summary, file.path(out_dir, "table_mtc_rtt_pre_post_by_victor.csv"))
}

# -------------------------
# Plot D — Application impact on victors (CloudSuite latency + hit rate)
# -------------------------
# Filter out victim if it appears in CloudSuite
victor_cs <- cloudsuite[tenant_ip != victim_ip]

# Latency time series (avg + p99)
cs_long_lat <- melt(victor_cs,
                    id.vars = c("tenant_ip","t"),
                    measure.vars = intersect(c("avg_lat","99th"), names(victor_cs)),
                    variable.name = "metric",
                    value.name = "lat_ms"
)

bin_s <- 10

lat_ds <- cs_long_lat[
  is.finite(lat_ms),
  .(
    lat_med = median(lat_ms, na.rm = TRUE),
    lat_p10 = quantile(lat_ms, 0.10, na.rm = TRUE),
    lat_p90 = quantile(lat_ms, 0.90, na.rm = TRUE)
  ),
  by = .(
    tenant_ip,
    metric,
    t_bin = floor(t / bin_s) * bin_s
  )
]

pD1 <- ggplot(lat_ds, aes(x = t_bin, y = lat_med * 1000)) +
  geom_ribbon(
    aes(ymin = lat_p10 * 1000, ymax = lat_p90 * 1000),
    fill = "black",
    alpha = 0.25
  ) +
  geom_line(linewidth = 0.8, color = "black") +
  # facet_wrap(tenant_ip ~ metric, scales = "free_y", ncol = 2) +
  facet_grid(rows = vars(tenant_ip), cols = vars(metric), scales = "free_y", switch = "y") +
  coord_cartesian(xlim = c(0, 5000)) + #, ylim = c(0, 500)) +
  labs(
    title = "", #"Plot D1 — CloudSuite latency over time (victors)",
    x = "Time (s, rebased)",
    y = "Latency (µs)"
  ) +
  theme_bw(base_size = 10) +
  theme(strip.text.x = element_text(size = 7)) +
  theme(axis.text.x = element_text(hjust = 1, size = 12, face = "bold"),
        axis.text.y = element_text(size = 12, face = "bold"),
        axis.title.x = element_text(size = 15, face = "bold"),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 15, face = "bold"),  # Bold and larger Y-axis label
        plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
        strip.placement = "outside",
        strip.text = element_text(face = "bold", size = 13),
        strip.text.x = element_text(size = 12),
        strip.background = element_rect(fill = "grey90", colour = "grey50")) +
  guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15))) + # Increase legend key size
  scale_y_log10()

if (is.finite(T_sat))
  pD1 <- pD1 + geom_vline(xintercept = T_sat, linetype = "dotted")

save_plot(pD1, "plotD1_cloudsuite_latency_timeseries.png", w = 7, h = 15)

# Hit rate over time

victor_cs_sum <- victor_cs %>%
  mutate(t_bin = floor(t / 10) * 10) %>%
  group_by(tenant_ip, t_bin) %>%
  summarise(
    hit_med = median(hit_rate, na.rm = TRUE),
    hit_lo  = quantile(hit_rate, 0.10, na.rm = TRUE),
    hit_hi  = quantile(hit_rate, 0.90, na.rm = TRUE),
    .groups = "drop"
  )


pD2 <- ggplot(victor_cs_sum, aes(x = t_bin)) +
  geom_ribbon(aes(ymin = hit_lo, ymax = hit_hi), alpha = 0.25) +
  geom_line(aes(y = hit_med), linewidth = 0.7) +
  facet_wrap(~ tenant_ip, scales = "free_y", ncol = 1) +
  coord_cartesian(xlim = c(0, 5000)) +
  labs(
    title = "", # "Plot D2 — CloudSuite hit rate over time (victors)",
    x = "Time (s, rebased)",
    y = "Hit rate"
  ) +
  theme_bw(base_size = 10) +
  theme(axis.text.x = element_text(hjust = 1, size = 12, face = "bold"),
        axis.text.y = element_text(size = 12, face = "bold"),
        axis.title.x = element_text(size = 15, face = "bold"),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 15, face = "bold"),  # Bold and larger Y-axis label
        plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
        legend.position = "bottom",
        # strip.placement = "outside",
        strip.text = element_text(face = "bold", size = 13),
        strip.text.x = element_text(size = 12),
        strip.background = element_rect(fill = "grey90", colour = "grey50")) +
  guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15)))


if (is.finite(T_sat))
  pD2 <- pD2 + geom_vline(xintercept = T_sat, linetype = "dotted")

save_plot(pD2, "plotD2_cloudsuite_hit_rate.png", w = 8, h = 15)

# pD2 <- ggplot(victor_cs, aes(x = t, y = hit_rate)) +
#   geom_line(linewidth = 0.35) +
#   facet_wrap(~ tenant_ip, scales = "free_y", ncol = 1) +
#   labs(
#     title = "Plot D2 — CloudSuite hit rate over time (victors)",
#     x = "Time (s, rebased)",
#     y = "Hit rate (hits/gets)"
#   ) +
#   theme_bw(base_size = 10)
# 
# pD2 <- pD2 +
#   coord_cartesian(xlim = c(0, 3600))   # e.g., show up to 5 ms
# 
# if (is.finite(T_sat)) pD2 <- pD2 + geom_vline(xintercept = T_sat, linetype = "dotted")
# save_plot(pD2, "plotD2_cloudsuite_hit_rate.png", w = 6, h = 10)

# -------------------------
# Pre/Post tables (CloudSuite + External)
# -------------------------
victor_cs[, phase := phase_of_t(t)]
cs_pp <- victor_cs[!is.na(phase)]


cs_summary <- cs_pp[, .(
  mean_rps = mean(rps, na.rm = TRUE),
  avg_lat_ms = mean(avg_lat, na.rm = TRUE),
  p99_lat_ms = mean(`99th`, na.rm = TRUE),
  hit_rate   = mean(hit_rate, na.rm = TRUE),
  miss_rate  = mean(miss_rate, na.rm = TRUE),
  max_lat_ms = max(max, na.rm = TRUE)
), by = .(tenant_ip, phase)]

# aggregate + worst-case (max p99) per phase
cs_agg <- cs_pp[, .(
  mean_rps = mean(rps, na.rm = TRUE),
  avg_lat_ms = mean(avg_lat, na.rm = TRUE),
  p99_lat_ms = mean(`99th`, na.rm = TRUE),
  hit_rate   = mean(hit_rate, na.rm = TRUE),
  miss_rate  = mean(miss_rate, na.rm = TRUE),
  worst_p99_ms = max(`99th`, na.rm = TRUE),
  worst_avg_ms = max(avg_lat, na.rm = TRUE)
), by = .(phase)]

fwrite(cs_summary, file.path(out_dir, "table_cloudsuite_pre_post_by_victor.csv"))
fwrite(cs_agg,     file.path(out_dir, "table_cloudsuite_pre_post_aggregate.csv"))

# External pre/post: victim rdma/udp and (optionally) all nodes
ext[, phase := phase_of_t(t)]
ext_pp <- ext[!is.na(phase)]


# Victim-focused external summary
victim_ext_pp <- ext_pp[node == victim_node]
victim_ext_summary <- victim_ext_pp[, .(
  rdma_tx_gbps_mean = mean(rdma_tx_gbps, na.rm = TRUE),
  rdma_tx_gbps_p99  = robust_quantile(rdma_tx_gbps, 0.99),
  rdma_rx_gbps_mean = mean(rdma_rx_gbps, na.rm = TRUE),
  udp_in_mean       = mean(udp_in_d, na.rm = TRUE),
  udp_out_mean      = mean(udp_out_d, na.rm = TRUE),
  udp_rcvbuf_err_sum = sum(udp_rcvbuf_err_d, na.rm = TRUE),
  udp_sndbuf_err_sum = sum(udp_sndbuf_err_d, na.rm = TRUE),
  rnr_err_sum        = sum(ib_rnr_nak_retry_err_d, na.rm = TRUE),
  ack_timeout_sum    = sum(ib_local_ack_timeout_err_d, na.rm = TRUE),
  cnp_sent_sum       = sum(ib_np_cnp_sent_d, na.rm = TRUE),
  cnp_handled_sum    = sum(ib_rp_cnp_handled_d, na.rm = TRUE)
), by = .(phase)]

fwrite(victim_ext_summary, file.path(out_dir, "table_victim_external_pre_post.csv"))

# All-nodes UDP error summary
udp_err_summary <- ext_pp[, .(
  udp_rcvbuf_err_sum = sum(udp_rcvbuf_err_d, na.rm = TRUE),
  udp_sndbuf_err_sum = sum(udp_sndbuf_err_d, na.rm = TRUE),
  udp_in_mean = mean(udp_in_d, na.rm = TRUE),
  udp_out_mean = mean(udp_out_d, na.rm = TRUE)
), by = .(node, phase)]
fwrite(udp_err_summary, file.path(out_dir, "table_udp_health_pre_post_all_nodes.csv"))

# -------------------------
# Extra: Victim saturation report + top offenders
# -------------------------
# 1) Victim plateau estimate (max, mean after T_sat)
victim_report <- data.table(
  T_sat = T_sat,
  victim_max_tx_gbps = max(victim_ext$rdma_tx_gbps, na.rm = TRUE),
  victim_mean_tx_gbps_post = mean(victim_ext[t >= post_start & t < post_end]$rdma_tx_gbps, na.rm = TRUE)
)
fwrite(victim_report, file.path(out_dir, "table_victim_saturation_report.csv"))

# 2) Identify worst victors by post-sat p99 latency (CloudSuite)
worst_victors <- cs_pp[phase == "post", .(
  post_mean_p99 = mean(`99th`, na.rm = TRUE),
  post_mean_avg = mean(avg_lat, na.rm = TRUE),
  post_hit_rate = mean(hit_rate, na.rm = TRUE)
), by = tenant_ip][order(-post_mean_p99)]
fwrite(worst_victors, file.path(out_dir, "table_worst_victors_by_post_p99.csv"))

# 3) If RTT exists: worst victors by late fraction
if (nrow(rtt)) {
  rtt_pp2 <- rtt[t >= post_start & t < post_end & is.finite(rtt_ns)]
  worst_late <- rtt_pp2[, .(
    post_late_frac = mean(status == 2, na.rm = TRUE),
    post_rtt_p99_ms = robust_quantile(rtt_ns/1e6, 0.99),
    post_rtt_p50_ms = robust_quantile(rtt_ns/1e6, 0.50)
  ), by = tenant_ip][order(-post_late_frac)]
  fwrite(worst_late, file.path(out_dir, "table_worst_victors_by_post_late_fraction.csv"))
}

# -------------------------
# Notes saved for reproducibility
# -------------------------
meta <- as.data.table(list(
  param = c("root_dir","victim_node","victim_ip","unused_node",
            "sat_hold_seconds","sat_frac_of_max","min_run_seconds",
            "pre_window_s","post_window_s","post_burnin_s",
            "pre_start","pre_end",
            "post_transient_start","post_transient_end",
            "post_steady_start","post_steady_end"),
  value = c(root_dir, victim_node, victim_ip, unused_node,
            sat_hold_seconds, sat_frac_of_max, min_run_seconds,
            pre_window, post_window, post_burnin,
            pre_start, pre_end,
            post0_start, post0_end,
            post_start, post_end)
))


fwrite(meta, file.path(out_dir, "analysis_metadata.csv"))

message("Done. Outputs written to: ", normalizePath(out_dir))
