#!/usr/bin/env Rscript
# Microbenchmark B (resizing overhead) analyzer
# - Finds "quiet point" = when external logger net_rx_bytes_d drops to 0 (MTC stops)
# - Selects an ACTIVE window before quiet point and a QUIET window after it
# - Computes deltas (active - quiet) for CloudsSuite metrics + external logger metrics
# - Generates paper-friendly time-series plots with the quiet-point marked
#
# Usage (from the experiment working directory):
#   Rscript analyze_resize_overhead.R \
#     --active_window_sec=60 --quiet_window_sec=60 --guard_sec=10 --out_dir=analysis_out
#
# Notes/assumptions:
# - cloudsuite logs have floating ts; external logger ts is integer seconds.
# - We align by time range, not by exact join. For “per-second” plots, cloudsuite is binned to integer seconds via floor(ts).
# - Quiet point is detected per-node; we use the median across nodes to be robust.
# - "net_rx_bytes_d == 0" is used as the stoppage indicator; you can change to udp_in_d/udp_out_d if desired.

# ---- Config for running inside RStudio (no CLI) ----
USE_CLI <- FALSE   # set TRUE if running via Rscript with arguments

ACTIVE_W <- 60
QUIET_W  <- 60
GUARD    <- 0
ZERO_RUN <- 5
OUT_DIR  <- "analysis_out"
QUIET_MODE <- "net_udp"         # net_udp or signal_only
REQUIRE_RDMA_ZERO <- FALSE      # keep FALSE in your case
SKIP_QUIET_SEARCH <- 300

suppressPackageStartupMessages({
  library(readr)
  library(dplyr)
  library(tidyr)
  library(stringr)
  library(ggplot2)
  library(optparse)
  library(purrr)
})

# ----------------------------
# CLI options
# ----------------------------
option_list <- list(
  make_option("--active_window_sec", type="integer", default=60,
              help="Length (sec) of ACTIVE window immediately before quiet point [default %default]"),
  make_option("--quiet_window_sec", type="integer", default=60,
              help="Length (sec) of QUIET window immediately after quiet point [default %default]"),
  make_option("--guard_sec", type="integer", default=0,
              help="Guard band (sec) excluded around the quiet point on each side [default %default]"),
  make_option("--out_dir", type="character", default="analysis_out",
              help="Output directory for tables/plots [default %default]"),
  make_option("--quiet_zero_run", type="integer", default=3,
              help="Require this many consecutive quiet seconds [default %default]"),
  make_option("--quiet_mode", type="character", default="net_udp",
              help="Quiet detection mode: net_udp (net+udp==0) or signal_only [default %default]"),
  make_option("--quiet_require_rdma_zero", action="store_true", default=FALSE,
              help="If set, also require ib_rx_{read,write}_requests_d == 0 during quiet detection"),
  make_option("--skip_quiet_search_sec", type="integer", default=300,
              help="Skip first N seconds of external log when detecting quiet point [default %default]")
  
)


if (USE_CLI) {
  opt <- parse_args(OptionParser(option_list=option_list))
  ACTIVE_W <- opt$active_window_sec
  QUIET_W  <- opt$quiet_window_sec
  GUARD    <- opt$guard_sec
  OUT_DIR  <- opt$out_dir
  ZERO_RUN <- opt$quiet_zero_run
  QUIET_MODE <- opt$quiet_mode
  REQUIRE_RDMA_ZERO <- opt$quiet_require_rdma_zero
  SKIP_QUIET_SEARCH <- opt$skip_quiet_search_sec
} else {
  dir.create(OUT_DIR, showWarnings = FALSE, recursive = TRUE)
}


dir.create(OUT_DIR, showWarnings = FALSE, recursive = TRUE)

# ----------------------------
# Helpers
# ----------------------------
stop_if_missing <- function(path) {
  if (!file.exists(path)) stop("Missing path: ", path, call. = FALSE)
}

read_cloudsuite_csv <- function(path) {
  # columns: ts,timeDiff,rps,requests,gets,sets,hits,misses,avg_lat,90th,95th,99th,std,min,max,avgGetSize
  read_csv(path, show_col_types = FALSE) %>%
    mutate(
      ts = as.numeric(ts),
      sec = floor(ts)
    )
}

read_external_csv <- function(path) {
  # external logger is 1Hz and ts is seconds
  read_csv(path, show_col_types = FALSE) %>%
    mutate(ts = as.numeric(ts))
}

# Find quiet point: first time we get ZERO_RUN consecutive zeros after seeing non-zero (drop-to-zero)
find_quiet_point <- function(df_ext, zero_run = 3,
                             mode = c("net_udp", "signal_only"),
                             signal_col = "net_rx_bytes_d",
                             require_rdma_zero = FALSE,
                             skip_sec = 0) {
  
  mode <- match.arg(mode)
  
  required_cols <- c("ts")
  if (mode == "net_udp") {
    required_cols <- c(required_cols, "net_rx_bytes_d","net_tx_bytes_d","udp_in_d","udp_out_d")
  } else {
    required_cols <- c(required_cols, signal_col)
  }
  if (require_rdma_zero) {
    required_cols <- c(required_cols, "ib_rx_read_requests_d", "ib_rx_write_requests_d")
  }
  
  # Skip initial portion (e.g., first 5 minutes) to avoid "all zeros at the beginning"
  if (skip_sec > 0) {
    t0 <- min(df_ext$ts, na.rm = TRUE)
    df_ext <- df_ext %>% filter(ts >= (t0 + skip_sec))
  }
  
  missing <- setdiff(required_cols, names(df_ext))
  if (length(missing) > 0) {
    stop("External log missing required cols for quiet detection: ",
         paste(missing, collapse=", "), call. = FALSE)
  }
  
  # Define "quiet" per second
  quiet <- if (mode == "net_udp") {
    (df_ext$net_rx_bytes_d == 0) &
      (df_ext$net_tx_bytes_d == 0)
  } else {
    (df_ext[[signal_col]] == 0)
  }
  
  if (require_rdma_zero) {
    quiet <- quiet &
      (df_ext$ib_rx_read_requests_d == 0) &
      (df_ext$ib_rx_write_requests_d == 0)
  }
  
  quiet[is.na(quiet)] <- FALSE
  
  # We want the first run of quiet lasting >= zero_run that occurs AFTER any non-quiet
  r <- rle(quiet)
  ends <- cumsum(r$lengths)
  starts <- ends - r$lengths + 1
  
  idx_runs <- which(r$values & r$lengths >= zero_run)
  if (length(idx_runs) == 0) return(NA_real_)
  
  for (k in idx_runs) {
    run_start <- starts[k]
    if (run_start > 1 && any(!quiet[1:(run_start-1)])) {
      return(df_ext$ts[run_start])
    }
  }
  
  # Fallback: first sustained quiet run start
  return(df_ext$ts[starts[idx_runs[1]]])
}

# Compute summary stats for a window for selected numeric columns
window_summary <- function(df, cols, label) {
  df %>%
    summarise(across(all_of(cols), ~ mean(.x, na.rm = TRUE))) %>%
    mutate(window = label) %>%
    select(window, everything())
}

# Robust diff: active - quiet for all numeric columns in a summary table
diff_active_quiet <- function(sum_active, sum_quiet, id_cols = "metric_group") {
  # Both are single-row data frames (one row each)
  common_cols <- intersect(names(sum_active), names(sum_quiet))
  common_cols <- setdiff(common_cols, c("window", id_cols))
  
  out <- tibble(metric = common_cols) %>%
    mutate(
      active = as.numeric(sum_active[1, common_cols]),
      quiet  = as.numeric(sum_quiet[1, common_cols]),
      delta  = active - quiet,
      pct_delta = ifelse(is.finite(quiet) & quiet != 0, 100 * delta / quiet, NA_real_)
    )
  out
}

save_csv <- function(df, name) {
  out_path <- file.path(OUT_DIR, name)
  write_csv(df, out_path)
  message("Wrote: ", out_path)
}

save_plot <- function(p, name, w=10, h=4) {
  out_path <- file.path(OUT_DIR, name)
  ggsave(out_path, plot = p, width = w, height = h, units = "in", dpi = 300)
  message("Saved plot: ", out_path)
}

add_active_quiet_deltas <- function(df_wide, metrics) {
  # df_wide has columns like <metric>_active and <metric>_quiet already
  for (m in metrics) {
    a <- paste0(m, "_active")
    q <- paste0(m, "_quiet")
    if (!(a %in% names(df_wide)) || !(q %in% names(df_wide))) next
    
    df_wide[[paste0(m, "_delta")]] <- df_wide[[a]] - df_wide[[q]]
    df_wide[[paste0(m, "_pct_delta")]] <- ifelse(is.finite(df_wide[[q]]) & df_wide[[q]] != 0,
                                                 100 * df_wide[[paste0(m, "_delta")]] / df_wide[[q]],
                                                 NA_real_)
  }
  df_wide
}

# ----------------------------
# Locate files
# ----------------------------
# CloudsSuite logs
stop_if_missing("cloudsuite")
cs_files <- list.files("cloudsuite", pattern="^cloudsuite_.*\\.csv$", full.names = TRUE)
if (length(cs_files) != 2) {
  stop("Expected exactly 2 cloudsuite_*.csv files in ./cloudsuite. Found: ", length(cs_files), call. = FALSE)
}

cs_receiver_path <- cs_files[str_detect(cs_files, "10\\.10\\.1\\.1")]
cs_donor_path    <- cs_files[str_detect(cs_files, "10\\.10\\.1\\.2")]
if (length(cs_receiver_path) != 1 || length(cs_donor_path) != 1) {
  stop("Could not uniquely identify receiver/donor cloudsuite logs by IP (10.10.1.1 / 10.10.1.2).", call. = FALSE)
}

# Donor cloudsuite may exist but be empty/invalid; we ignore it.


# External logs
stop_if_missing("mtc_resize_overhead")
ext_node0_path <- file.path("mtc_resize_overhead", "node0", "stats_1hz.csv")
ext_node1_path <- file.path("mtc_resize_overhead", "node1", "stats_1hz.csv")
stop_if_missing(ext_node0_path)
stop_if_missing(ext_node1_path)

# ----------------------------
# Load data
# ----------------------------
cs_receiver <- read_cloudsuite_csv(cs_receiver_path) %>% mutate(node="node0", role="receiver")
cs_donor    <- read_cloudsuite_csv(cs_donor_path)    %>% mutate(node="node1", role="donor")

cs_all <- bind_rows(cs_receiver, cs_donor)

cs_all <- cs_all %>%
  mutate(role = recode(role,
                       donor    = "victim",
                       receiver = "victor"))

ext0 <- read_external_csv(ext_node0_path) %>% mutate(node="node0", role="receiver")
ext1 <- read_external_csv(ext_node1_path) %>% mutate(node="node1", role="donor")

# ----------------------------
# Determine quiet point
# ----------------------------
qt0 <- find_quiet_point(ext0, zero_run = ZERO_RUN, mode = QUIET_MODE,
                        require_rdma_zero = REQUIRE_RDMA_ZERO,
                        skip_sec = SKIP_QUIET_SEARCH)
qt1 <- find_quiet_point(ext1, zero_run = ZERO_RUN, mode = QUIET_MODE,
                        require_rdma_zero = REQUIRE_RDMA_ZERO,
                        skip_sec = SKIP_QUIET_SEARCH)


if (!is.finite(qt0) && !is.finite(qt1)) {
  stop("Could not detect quiet point on either node using signal: ", QUIET_SIGNAL, call. = FALSE)
}

quiet_ts <- median(c(qt0, qt1), na.rm = TRUE)

message("Quiet point detected:")
message("  node0: ", qt0)
message("  node1: ", qt1)
message("  chosen quiet_ts (median): ", quiet_ts)

# Define windows
active_start <- quiet_ts - GUARD - ACTIVE_W
active_end   <- quiet_ts - GUARD
quiet_start  <- quiet_ts + GUARD
quiet_end    <- quiet_ts + GUARD + QUIET_W

plot_start <- quiet_ts - ACTIVE_W
plot_end   <- quiet_ts + QUIET_W

windows_df <- tibble(
  quiet_ts = quiet_ts,
  active_start = active_start, active_end = active_end,
  quiet_start = quiet_start, quiet_end = quiet_end,
  active_window_sec = ACTIVE_W,
  quiet_window_sec  = QUIET_W,
  guard_sec = GUARD,
  zero_run = ZERO_RUN
)

windows_df <- windows_df %>%
  mutate(quiet_mode = QUIET_MODE,
         quiet_require_rdma_zero = REQUIRE_RDMA_ZERO,
         skip_quiet_search_sec = SKIP_QUIET_SEARCH)

save_csv(windows_df, "window_definition.csv")

facet_if_multiple_roles <- function(p, df) {
  if (n_distinct(df$role) > 1) p + facet_wrap(~ role, ncol=1, scales="free_y") else p
}
# ----------------------------
# Filter data into windows
# ----------------------------
# CloudsSuite: use floating ts for filtering, and bin to seconds for plotting
cs_active <- cs_all %>% filter(ts >= active_start, ts < active_end) %>% mutate(window="active")
cs_quiet  <- cs_all %>% filter(ts >= quiet_start,  ts < quiet_end) %>% mutate(window="quiet")

# External logs: already per-second ts
ext_all <- bind_rows(ext0, ext1)

ext_all <- ext_all %>%
  mutate(role = recode(role,
                       donor    = "victim",
                       receiver = "victor"))

ext_active <- ext_all %>% filter(ts >= active_start, ts < active_end) %>% mutate(window="active")
ext_quiet  <- ext_all %>% filter(ts >= quiet_start,  ts < quiet_end) %>% mutate(window="quiet")

# Quick sanity checks
if (nrow(cs_active) == 0 || nrow(cs_quiet) == 0) warning("CloudSuite window has 0 rows for active or quiet.")
if (nrow(ext_active) == 0 || nrow(ext_quiet) == 0) warning("External window has 0 rows for active or quiet.")

# ----------------------------
# Compute overhead metrics
# ----------------------------
# CloudsSuite metrics to summarize (latencies in ms already)
cs_metrics <- c("rps","requests","gets","sets","hits","misses","avg_lat","90th","95th","99th","std","min","max","avgGetSize")

cs_summary <- cs_all %>%
  mutate(window = case_when(
    ts >= active_start & ts < active_end ~ "active",
    ts >= quiet_start  & ts < quiet_end  ~ "quiet",
    TRUE ~ NA_character_
  )) %>%
  filter(!is.na(window)) %>%
  group_by(node, role, window) %>%
  summarise(across(all_of(cs_metrics), ~ mean(.x, na.rm = TRUE)), .groups="drop")

save_csv(cs_summary, "cloudsuite_window_summary.csv")

# Per-role overhead (active - quiet)
cs_overhead <- cs_summary %>%
  select(node, role, window, all_of(cs_metrics)) %>%
  tidyr::pivot_wider(names_from = window, values_from = all_of(cs_metrics),
                     names_glue = "{.value}_{window}")

cs_overhead <- add_active_quiet_deltas(cs_overhead, cs_metrics)

save_csv(cs_overhead, "cloudsuite_overhead_active_minus_quiet.csv")

# External metrics: pick meaningful columns (you can add/remove here)
ext_metrics <- c(
  "cpu_pct","load1","load5","load15",
  "net_rx_bytes_d","net_tx_bytes_d","net_rx_pkts_d","net_tx_pkts_d",
  "udp_in_d","udp_out_d","udp_rcvbuf_err_d","udp_sndbuf_err_d",
  "ib_port_xmit_data_d","ib_port_rcv_data_d","ib_port_xmit_packets_d","ib_port_rcv_packets_d",
  "ib_unicast_xmit_packets_d","ib_unicast_rcv_packets_d",
  "ib_rx_write_requests_d","ib_rx_read_requests_d",
  "ib_rnr_nak_retry_err_d","ib_local_ack_timeout_err_d",
  "ib_out_of_sequence_d","ib_packet_seq_err_d",
  "ib_np_cnp_sent_d","ib_rp_cnp_handled_d"
)
ext_metrics <- ext_metrics[ext_metrics %in% names(ext_all)]  # keep only available

ext_summary <- ext_all %>%
  mutate(window = case_when(
    ts >= active_start & ts < active_end ~ "active",
    ts >= quiet_start  & ts < quiet_end  ~ "quiet",
    TRUE ~ NA_character_
  )) %>%
  filter(!is.na(window)) %>%
  group_by(node, role, window) %>%
  summarise(across(all_of(ext_metrics), ~ mean(.x, na.rm = TRUE)), .groups="drop")

save_csv(ext_summary, "external_window_summary.csv")

ext_overhead <- ext_summary %>%
  select(node, role, window, all_of(ext_metrics)) %>%
  tidyr::pivot_wider(names_from = window, values_from = all_of(ext_metrics),
                     names_glue = "{.value}_{window}")

ext_overhead <- add_active_quiet_deltas(ext_overhead, ext_metrics)

save_csv(ext_overhead, "external_overhead_active_minus_quiet.csv")

# ----------------------------
# Print overhead summaries (console)
# ----------------------------
format_row <- function(role, metric, active, quiet, delta, pct) {
  sprintf("%-9s  %-18s  active=%-12.4f  quiet=%-12.4f  delta=%-12.4f  pct=%s",
          role, metric, active, quiet, delta,
          ifelse(is.na(pct), "NA", sprintf("%.2f%%", pct)))
}

print_overhead_block <- function(title, df_overhead, metrics, role_col="role") {
  cat("\n==============================\n")
  cat(title, "\n")
  cat("==============================\n")
  
  # df_overhead is the wide table (role, metric_active, metric_quiet, metric_delta, metric_pct_delta)
  for (r in unique(df_overhead[[role_col]])) {
    cat("\n-- ", r, " --\n", sep="")
    sub <- df_overhead %>% filter(.data[[role_col]] == r)
    
    for (m in metrics) {
      a <- sub[[paste0(m, "_active")]]
      q <- sub[[paste0(m, "_quiet")]]
      d <- sub[[paste0(m, "_delta")]]
      p <- sub[[paste0(m, "_pct_delta")]]
      cat(format_row(r, m, a, q, d, p), "\n")
    }
  }
}

# CloudsSuite: paper-relevant metrics
cs_metrics_print <- c("rps", "avg_lat", "99th", "misses", "gets", "sets", "hits")
# External: paper-relevant metrics
ext_metrics_print <- c("cpu_pct", "load1",
                       "net_rx_bytes_d", "net_tx_bytes_d",
                       "udp_in_d", "udp_out_d",
                       "ib_rx_read_requests_d", "ib_rx_write_requests_d",
                       "ib_port_xmit_data_d", "ib_port_rcv_data_d")

# Keep only metrics that exist (robust)
cs_metrics_print <- cs_metrics_print[cs_metrics_print %in% names(cs_overhead)]
ext_metrics_print <- ext_metrics_print[ext_metrics_print %in% names(ext_overhead)]

print_overhead_block("CloudSuite overhead (ACTIVE - QUIET)", cs_overhead, cs_metrics_print)
print_overhead_block("External overhead (ACTIVE - QUIET)", ext_overhead, ext_metrics_print)

cat("\nQuiet point ts = ", quiet_ts, "\n", sep="")
cat("ACTIVE window: [", active_start, ", ", active_end, ")\n", sep="")
cat("QUIET  window: [", quiet_start,  ", ", quiet_end, ")\n", sep="")

# ----------------------------
# Plots: time-series with quiet point and window shading
# ----------------------------
shade_df <- tibble(
  xmin = c(active_start, quiet_start),
  xmax = c(active_end,   quiet_end),
  window = c("active", "quiet")
)

# CloudsSuite: bin to per-second for clearer paper plots
cs_ts_1hz <- cs_all %>%
  group_by(node, role, sec) %>%
  summarise(
    rps = mean(rps, na.rm=TRUE),
    avg_lat = mean(avg_lat, na.rm=TRUE),
    p99 = mean(`99th`, na.rm=TRUE),
    misses = mean(misses, na.rm=TRUE),
    .groups="drop"
  ) %>%
  rename(ts = sec)

cs_ts_1hz_plot <- cs_ts_1hz %>%
  filter(ts >= plot_start, ts <= plot_end)

plot_cs_series <- function(df, ycol, ylab, fname) {
  p <- ggplot(df, aes(x = ts, y = .data[[ycol]] * 1000, linetype = role)) +
    geom_rect(data = shade_df, inherit.aes = FALSE,
              aes(xmin=xmin, xmax=xmax, ymin=-Inf, ymax=Inf),
              alpha = 0.12) +
    geom_vline(xintercept = quiet_ts, linewidth = 0.6) +
    geom_line(linewidth = 0.5) +
    facet_wrap(~ role, ncol=1, scales="free_y") +
    labs(x="Time (s, real-world)", y=ylab, #title=paste0("CloudSuite ", ylab, " (1Hz binned)"),
         subtitle = paste0("quiet_point=", quiet_ts,
                           " | active_window=[", active_start, ",", active_end, ")",
                           " | quiet_window=[", quiet_start, ",", quiet_end, ")")) +
    theme_bw() +
    theme(axis.text.x = element_text(size = 10, face = "bold"),
          axis.text.y = element_text(size = 10, face = "bold"),
          axis.title.x = element_text(size = 15, face = "bold"),  # Bold and larger X-axis label
          axis.title.y = element_text(size = 15, face = "bold"),  # Bold and larger Y-axis label
          plot.title = element_text(size = 10, face = "bold"),
          plot.subtitle = element_text(size = 10, face = "bold"),
          strip.text = element_text(face = "bold", size = 13),
          strip.background = element_rect(fill = "grey90", colour = "grey50"),
          legend.position = "none")
#  p <- facet_if_multiple_roles(p, df)
  save_plot(p, fname, w=10, h=5)
}

plot_cs_series(cs_ts_1hz_plot, "avg_lat", "Avg latency (µs)", "plot_cloudsuite_avg_latency.png")
plot_cs_series(cs_ts_1hz_plot, "p99",     "p99 latency (µs)", "plot_cloudsuite_p99_latency.png")
plot_cs_series(cs_ts_1hz_plot, "rps",     "RPS",              "plot_cloudsuite_rps.png")
plot_cs_series(cs_ts_1hz_plot, "misses",  "Misses/sec (avg)", "plot_cloudsuite_misses.png")

# External logger plots (already 1Hz)
plot_ext_series <- function(df, ycol, ylab, fname) {
  if (!ycol %in% names(df)) return(invisible(NULL))
  p <- ggplot(df, aes(x = ts, y = .data[[ycol]], linetype = role)) +
    geom_rect(data = shade_df, inherit.aes = FALSE,
              aes(xmin=xmin, xmax=xmax, ymin=-Inf, ymax=Inf),
              alpha = 0.12) +
    geom_vline(xintercept = quiet_ts, linewidth = 0.6) +
    geom_line(linewidth = 0.5) +
    facet_wrap(~ role, ncol=1, scales="free_y") +
    labs(x="Time (s, real-world)", y=ylab, #title=paste0("External ", ylab),
         subtitle = paste0("quiet_point=", quiet_ts,
                           " | active_window=[", active_start, ",", active_end, ")",
                           " | quiet_window=[", quiet_start, ",", quiet_end, ")")) +
    theme_bw() +
    theme(axis.text.x = element_text(size = 10, face = "bold"),
          axis.text.y = element_text(size = 10, face = "bold"),
          axis.title.x = element_text(size = 15, face = "bold"),  # Bold and larger X-axis label
          axis.title.y = element_text(size = 15, face = "bold"),  # Bold and larger Y-axis label
          plot.title = element_text(size = 10, face = "bold"),
          plot.subtitle = element_text(size = 10, face = "bold"),
          strip.text = element_text(face = "bold", size = 13),
          strip.background = element_rect(fill = "grey90", colour = "grey50"),
          legend.position = "none")
  save_plot(p, fname, w=10, h=5)
}

ext_all_plot <- ext_all %>%
  filter(ts >= plot_start, ts <= plot_end)

plot_ext_series(ext_all_plot, "cpu_pct", "CPU (%)", "plot_external_cpu.png")
plot_ext_series(ext_all_plot, "load1", "Load (1m)", "plot_external_load1.png")
plot_ext_series(ext_all_plot, "net_rx_bytes_d", "net_rx_bytes_d", "plot_external_net_rx_bytes_d.png")
plot_ext_series(ext_all_plot, "net_tx_bytes_d", "net_tx_bytes_d", "plot_external_net_tx_bytes_d.png")
plot_ext_series(ext_all_plot, "udp_in_d", "udp_in_d", "plot_external_udp_in_d.png")
plot_ext_series(ext_all_plot, "udp_out_d", "udp_out_d", "plot_external_udp_out_d.png")
plot_ext_series(ext_all_plot, "ib_rx_read_requests_d", "ib_rx_read_requests_d", "plot_external_ib_rx_read_requests_d.png")
plot_ext_series(ext_all_plot, "ib_rx_write_requests_d", "ib_rx_write_requests_d", "plot_external_ib_rx_write_requests_d.png")
plot_ext_series(ext_all_plot, "ib_port_xmit_data_d", "ib_port_xmit_data_d", "plot_external_ib_port_xmit_data_d.png")
plot_ext_series(ext_all_plot, "ib_port_rcv_data_d", "ib_port_rcv_data_d", "plot_external_ib_port_rcv_data_d.png")

# ----------------------------
# Convenience: compact “paper table” outputs
# ----------------------------
paper_cs <- cs_summary %>%
  select(node, role, window, rps, avg_lat, `99th`, misses) %>%
  rename(p99 = `99th`) %>%
  arrange(role, window)
save_csv(paper_cs, "paper_table_cloudsuite_basic.csv")

paper_ext <- ext_summary %>%
  select(node, role, window,
         any_of(c("cpu_pct","load1","net_rx_bytes_d","net_tx_bytes_d","udp_in_d","udp_out_d",
                  "ib_rx_read_requests_d","ib_rx_write_requests_d",
                  "ib_port_xmit_data_d","ib_port_rcv_data_d"))) %>%
  arrange(role, window)
save_csv(paper_ext, "paper_table_external_basic.csv")

message("\nDone. Outputs in: ", OUT_DIR)
