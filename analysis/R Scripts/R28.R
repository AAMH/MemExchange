#!/usr/bin/env Rscript

suppressPackageStartupMessages({
  library(readr)
  library(dplyr)
  library(stringr)
  library(tidyr)
  library(purrr)
  library(ggplot2)
})

# -----------------------------
# Config
# -----------------------------
ROOT <- "."                      # run from parent of RDMA/RXE/TCP
SCHEMES <- c("RDMA", "RXE", "TCP")
OUTDIR <- file.path(ROOT, "analysis_out")
dir.create(OUTDIR, showWarnings = FALSE, recursive = TRUE)

RECEIVER_IP <- "10.10.1.1"
DONOR_IP    <- "10.10.1.2"

# -----------------------------
# Helpers
# -----------------------------
phase_from_filename <- function(fname) {
  if (str_detect(fname, "setonly\\.csv$")) "SET" else "GET"
}

role_from_filename <- function(fname) {
  # cloudsuite_<ip>_<port>.csv
  ip <- str_match(fname, "cloudsuite_([0-9\\.]+)_")[,2]
  if (is.na(ip)) return(NA_character_)
  if (ip == RECEIVER_IP) "victor" else if (ip == DONOR_IP) "victim" else paste0("ip_", ip)
}

read_cloudsuite_file <- function(path, scheme) {
  fname <- basename(path)
  role  <- role_from_filename(fname)
  phase <- phase_from_filename(fname)
  
  # Read without fixed col_types first, because RDMA files may be missing 'ts'
  df <- read_csv(path, show_col_types = FALSE)
  
  # If 'ts' is missing, assume 1 row per second and synthesize ts = 0..N-1
  if (!("ts" %in% names(df))) {
    df <- df %>%
      mutate(ts = row_number() - 1L) %>%
      relocate(ts, .before = 1)
  }
  
  # Ensure standard columns exist (add NAs if missing), then order them
  required <- c("ts","timeDiff","rps","requests","gets","sets","hits","misses",
                "avg_lat","90th","95th","99th","std","min","max","avgGetSize")
  missing_cols <- setdiff(required, names(df))
  if (length(missing_cols) > 0) {
    df[missing_cols] <- NA
  }
  df <- df %>% select(any_of(required), everything())
  
  # Coerce known numeric columns (including the percentile columns)
  df <- df %>%
    mutate(across(any_of(required), ~ suppressWarnings(as.numeric(.x))))
  
  df %>%
    mutate(
      scheme = scheme,
      role   = role,
      phase  = phase,
      file   = fname
    )
}

safe_read_external_logger <- function(path, scheme, node) {
  # External logger differs for TCP (extra TCP columns).
  # We'll read with guess + keep all cols.
  df <- read_csv(path, show_col_types = FALSE, progress = FALSE)
  df %>%
    mutate(
      scheme = scheme,
      node = node,
      file = basename(path)
    )
}

cloudsuite_window <- function(df) {
  # returns per (scheme, role, phase) time window
  df %>%
    group_by(scheme, role, phase) %>%
    summarise(
      t_min = min(ts, na.rm = TRUE),
      t_max = max(ts, na.rm = TRUE),
      n_rows = n(),
      .groups = "drop"
    )
}

summarise_cloudsuite <- function(df) {
  # Per (scheme, role, phase) summary.
  # Quantiles in logs are already per-second, we’ll aggregate across seconds.
  df %>%
    group_by(scheme, role, phase) %>%
    summarise(
      seconds = n(),
      duration_s = sum(timeDiff, na.rm = TRUE),
      rps_mean = mean(rps, na.rm = TRUE),
      rps_sd   = sd(rps, na.rm = TRUE),
      req_total = sum(requests, na.rm = TRUE),
      gets_total = sum(gets, na.rm = TRUE),
      sets_total = sum(sets, na.rm = TRUE),
      hit_rate = ifelse(sum(gets, na.rm = TRUE) > 0,
                        sum(hits, na.rm = TRUE) / sum(gets, na.rm = TRUE),
                        NA_real_),
      # latency (ms)
      lat_avg_mean = mean(avg_lat, na.rm = TRUE),
      lat_avg_median = median(avg_lat, na.rm = TRUE),
      lat_p90_mean = mean(`90th`, na.rm = TRUE),
      lat_p95_mean = mean(`95th`, na.rm = TRUE),
      lat_p99_mean = mean(`99th`, na.rm = TRUE),
      lat_p99_median = median(`99th`, na.rm = TRUE),
      lat_max = max(max, na.rm = TRUE),
      lat_min = min(min, na.rm = TRUE),
      avg_get_size_B = mean(avgGetSize, na.rm = TRUE),
      t_start = min(ts, na.rm = TRUE),
      t_end = max(ts, na.rm = TRUE),
      .groups = "drop"
    )
}

compare_receiver_vs_donor <- function(summary_df) {
  # Join receiver to donor within same scheme+phase.
  recv <- summary_df %>% filter(role == "victor") %>% select(-role)
  donor <- summary_df %>% filter(role == "victim") %>% select(-role)
  
  recv %>%
    inner_join(donor, by = c("scheme", "phase"), suffix = c("_recv", "_donor")) %>%
    mutate(
      # Absolute penalty (ms)
      d_lat_avg_ms = lat_avg_mean_recv - lat_avg_mean_donor,
      d_p99_ms     = lat_p99_mean_recv - lat_p99_mean_donor,
      # Slowdown factors
      x_lat_avg = lat_avg_mean_recv / lat_avg_mean_donor,
      x_p99     = lat_p99_mean_recv / lat_p99_mean_donor
    ) %>%
    arrange(phase, scheme)
}

# -----------------------------
# Load Cloudsuite
# -----------------------------
cloudsuite_paths <- map(SCHEMES, function(s) {
  dir <- file.path(ROOT, s, "cloudsuite")
  if (!dir.exists(dir)) return(tibble(scheme = character(), path = character()))
  tibble(
    scheme = s,
    path = list.files(dir, pattern = "^cloudsuite_.*\\.csv$", full.names = TRUE)
  )
}) %>% bind_rows()

if (nrow(cloudsuite_paths) == 0) {
  stop("No cloudsuite CSVs found. Run from parent folder containing RDMA/RXE/TCP.")
}

cloudsuite_all <- pmap_dfr(
  list(cloudsuite_paths$path, cloudsuite_paths$scheme),
  read_cloudsuite_file
)

cloudsuite_all <- cloudsuite_all %>%
  group_by(scheme, role, phase) %>%
  arrange(ts, .by_group = TRUE) %>%
  mutate(
    t = ts - first(ts)   # seconds since this run started
  ) %>%
  ungroup()

# Basic sanity checks
stopifnot(all(c("scheme","role","phase","ts") %in% colnames(cloudsuite_all)))

# Summaries
cloudsuite_summary <- summarise_cloudsuite(cloudsuite_all)
cloudsuite_comp <- compare_receiver_vs_donor(cloudsuite_summary)

write_csv(cloudsuite_summary, file.path(OUTDIR, "cloudsuite_phase_summary.csv"))
write_csv(cloudsuite_comp,    file.path(OUTDIR, "receiver_vs_donor_comparison.csv"))


# -----------------------------
# Specific plot: RDMA GET-only avg latency
# victim vs victor in one plot
# -----------------------------
p_rdma_get_avg_victim_victor <- cloudsuite_all %>%
  filter(
    scheme == "RDMA",
    phase == "GET",
    role %in% c("victim", "victor"),
    avg_lat < 0.150 # filter out noise >= 150 microsecond
  ) %>%
  ggplot(aes(x = t, y = avg_lat * 1000, color = role)) +
  geom_line(linewidth = 0.6, alpha = 0.9) +
  labs(
    title = "RDMA GET-only",
    x = "Time (s)",
    y = "Avg latency (µs)",
    color = "Role"
  ) +
  theme_bw() +
  theme(
    axis.text.x = element_text(size = 10, face = "bold"),
    axis.text.y = element_text(size = 10, face = "bold"),
    axis.title.x = element_text(size = 13, face = "bold"),
    axis.title.y = element_text(size = 13, face = "bold"),
    plot.title = element_text(hjust = 0.5, size = 12, face = "bold"),
    legend.position = "bottom",
    legend.key.size = unit(1, "cm"),  # Enlarge the legend keys
    legend.text = element_text(size = 10, face = "bold"),  # Increase the text size
    legend.title = element_text(size = 12, face = "bold")
  ) +
  guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15))) +  # Increase legend key size
  scale_color_manual(values = c(
    victim = "#1f77b4",
    victor = "#d62728"
  ))

ggsave(
  file.path(OUTDIR, "RDMA_GET_avg_victim_vs_victor.png"),
  p_rdma_get_avg_victim_victor,
  width = 8.5,
  height = 4.5,
  dpi = 200
)

# -----------------------------
# Plots: latency time series
# -----------------------------
p_ts_avg <- cloudsuite_all %>%
  ggplot(aes(x = t, y = avg_lat * 1000, color = role)) +
  geom_line(alpha = 0.8) +
  facet_grid(
    rows = vars(scheme, phase),
    cols = vars(role),
    scales = "free_x"
  ) +
  coord_cartesian(ylim = c(0, 200)) +
  scale_color_manual(values = c(donor = "#1f77b4", receiver = "#d62728")) +
  labs(
    title = "Cloudsuite avg latency over time",
    x = "Unix time (s)",
    y = "Latency (µs)",
    color = "Role"
  ) +
  theme_bw() +
  theme(
    strip.background = element_rect(fill = "grey90"),
    strip.text = element_text(face = "bold"),
    legend.position = "none"
  )


p_ts_p99 <- cloudsuite_all %>%
  ggplot(aes(x = t, y = `99th` * 1000, color = role)) +
  geom_line(alpha = 0.8) +
  facet_grid(
    rows = vars(scheme, phase),
    cols = vars(role),
    scales = "free_x"
  ) +
  coord_cartesian(ylim = c(0, 200)) +
  scale_color_manual(values = c(donor = "#1f77b4", receiver = "#d62728")) +
  labs(
    title = "Cloudsuite p99 latency over time",
    x = "Unix time (s)",
    y = "Latency (µs)",
    color = "Role"
  ) +
  theme_bw() +
  theme(
    strip.background = element_rect(fill = "grey90"),
    strip.text = element_text(face = "bold"),
    legend.position = "none"
  )


ggsave(file.path(OUTDIR, "latency_timeseries_avg.png"), p_ts_avg, width = 14, height = 7, dpi = 200)
ggsave(file.path(OUTDIR, "latency_timeseries_p99.png"), p_ts_p99, width = 14, height = 7, dpi = 200)


cs_long <- cloudsuite_all %>%
  # If you don’t already have a mapped time axis:
  group_by(scheme, role, phase) %>%
  mutate(t = row_number()) %>%
  ungroup() %>%
  select(scheme, role, phase, t, avg_lat, `99th`) %>%
  pivot_longer(
    cols = c(avg_lat, `99th`),
    names_to = "metric",
    values_to = "lat_ms"
  ) %>%
  mutate(metric = recode(metric, avg_lat = "avg", `99th` = "p99"))

median_mad_limits_nonneg <- function(y, k = 6) {
  m <- median(y, na.rm = TRUE)
  d <- mad(y, na.rm = TRUE)
  
  lower <- max(0, m - k * d)
  upper <- m + k * d
  
  c(lower, upper)
}

role_thresholds <- c(
  victim = 10000000,
  victor = 10000000
)

plot_phase_compare <- function(df, phase_name, title) {
  df %>%
    filter(phase == phase_name, lat_ms <= 1) %>%
    # filter(lat_ms <= role_thresholds[role]) %>%
    ggplot(aes(x = t, y = lat_ms * 1000, color = scheme)) +
    geom_line(linewidth = 0.4, alpha = 0.9) +
    facet_grid(metric ~ role, scales = "free_y") +
    labs(
      title = title,
      x = "Time (s)",
      y = "Latency (µs, log scale)",
      color = "Scheme"
    ) +
    theme_bw() +
    theme(axis.text.x = element_text(hjust = 1, size = 10, face = "bold"),
          axis.text.y = element_text(size = 10, face = "bold"),
          axis.title.x = element_text(size = 15, face = "bold"),  # Bold and larger X-axis label
          axis.title.y = element_text(size = 15, face = "bold", hjust = 0.75),  # Bold and larger Y-axis label
          plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
          legend.key.size = unit(0.7, "cm"),  # Enlarge the legend keys
          legend.text = element_text(size = 10),  # Increase the text size
          legend.title = element_text(size = 12, face = "bold"),
          legend.position = "bottom") +
    guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15))) +  # Increase legend key size
    scale_y_log10(
      labels = scales::label_number()
    )
#    coord_cartesian(
#      ylim = quantile(df$lat_ms, c(0.01, 0.99), na.rm = TRUE)
#      ylim = median_mad_limits_nonneg(df$lat_ms, k = 6)
#    )
}

p_compare_set <- plot_phase_compare(
  cs_long, "SET", "" # "Cloudsuite latency over time (SET-only) — RDMA vs RXE vs TCP"
)

p_compare_get <- plot_phase_compare(
  cs_long, "GET", "" # "Cloudsuite latency over time (GET-only) — RDMA vs RXE vs TCP"
)

ggsave(file.path(OUTDIR, "compare_SET.png"), p_compare_set, width = 11, height = 6, dpi = 200)
ggsave(file.path(OUTDIR, "compare_GET.png"), p_compare_get, width = 11, height = 6, dpi = 200)

# -----------------------------
# Separate compare plots (no grid)
# One file per (phase, metric, role)
# -----------------------------

plot_compare_single <- function(df, phase_name, metric_name, role_name) {
  df_s <- df %>%
    filter(phase == phase_name, metric == metric_name, role == role_name, lat_ms <= role_thresholds[role])
    
  ggplot(df_s, aes(x = t, y = lat_ms * 1000, color = scheme)) +
    geom_line(linewidth = 0.5, alpha = 0.9) +
    labs(
      title = "", #paste0(
        #"Cloudsuite ", metric_name, " latency — ", phase_name, " — ", role_name,
        #" (RDMA vs RXE vs TCP)"
      #),
      x = "Time (s)",
      y = "Latency (µs, log scale)",
      color = "scheme"
    ) +
    theme_bw()  +
    theme(axis.text.x = element_text(hjust = 1, size = 10, face = "bold"),
          axis.text.y = element_text(size = 10, face = "bold"),
          axis.title.x = element_text(size = 15, face = "bold"),  # Bold and larger X-axis label
          axis.title.y = element_text(size = 15, face = "bold", hjust = 0.75),  # Bold and larger Y-axis label
          plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
          legend.key.size = unit(1, "cm"),  # Enlarge the legend keys
          legend.text = element_text(size = 10),  # Increase the text size
          legend.title = element_text(size = 12, face = "bold"),
          legend.position = "bottom") +
    guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15))) +  # Increase legend key size
    scale_y_log10(
      labels = scales::label_number()
    )
#    coord_cartesian(
#     ylim = median_mad_limits_nonneg(df_s$lat_ms, k = 6)
#      ylim = quantile(df$lat_ms, c(0.01, 0.99), na.rm = TRUE)
#    )
}

# Generate & save 8 plots
PHASES  <- c("SET", "GET")
METRICS <- c("avg", "p99")
ROLES   <- c("victim", "victor")

for (ph in PHASES) {
  for (m in METRICS) {
    for (r in ROLES) {
      p <- plot_compare_single(cs_long, ph, m, r)
      out <- file.path(OUTDIR, paste0("compare_", ph, "_", m, "_", r, ".png"))
      ggsave(out, p, width = 8.5, height = 4.5, dpi = 200)
    }
  }
}

# -----------------------------
# Plots: receiver vs donor (bar/point)
# -----------------------------
comp_long <- cloudsuite_comp %>%
  select(scheme, phase, d_lat_avg_ms, d_p99_ms, x_lat_avg, x_p99) %>%
  pivot_longer(cols = -c(scheme, phase), names_to = "metric", values_to = "value")

p_comp <- comp_long %>%
  ggplot(aes(x = scheme, y = value)) +
  geom_col() +
  facet_grid(phase ~ metric, scales = "free_y") +
  labs(
    title = "Receiver vs donor: penalties and slowdowns",
    x = "Scheme",
    y = "Value"
  )

ggsave(file.path(OUTDIR, "receiver_vs_donor_penalties.png"), p_comp, width = 14, height = 9, dpi = 200)

# -----------------------------
# Load External Logger (optional, sanity)
# -----------------------------
ext_paths <- map(SCHEMES, function(s) {
  base <- file.path(ROOT, s, "external_logger")
  if (!dir.exists(base)) return(tibble())
  tibble(
    scheme = s,
    node = c("node0","node1"),
    path = c(
      file.path(base, "node0", "stats_1hz.csv"),
      file.path(base, "node1", "stats_1hz.csv")
    )
  ) %>% filter(file.exists(path))
}) %>% bind_rows()

external_all <- if (nrow(ext_paths) > 0) {
  pmap_dfr(list(ext_paths$path, ext_paths$scheme, ext_paths$node), safe_read_external_logger)
} else {
  tibble()
}

if (nrow(external_all) > 0) {
  # Trim external logger to each cloudsuite window (scheme, role, phase)
  win <- cloudsuite_window(cloudsuite_all)
  
  # map role->node
  win2 <- win %>%
    mutate(node = ifelse(role == "victor", "node0", ifelse(role == "victim", "node1", NA_character_))) %>%
    filter(!is.na(node))
  
  trimmed <- pmap_dfr(
    list(win2$scheme, win2$role, win2$phase, win2$node, win2$t_min, win2$t_max),
    function(scheme, role, phase, node, t_min, t_max) {
      external_all %>%
        filter(scheme == scheme, node == node) %>%
        filter(ts >= t_min, ts <= t_max) %>%
        mutate(role = role, phase = phase)
    }
  )
  
  
  write_csv(trimmed, file.path(OUTDIR, "external_logger_trimmed_to_cloudsuite_windows.csv"))
  
  # Quick sanity plot: cpu_pct + (UDP or TCP) segments if present
  # CPU plot
  p_cpu <- trimmed %>%
    ggplot(aes(x = ts, y = cpu_pct)) +
    geom_line() +
    facet_grid(phase ~ scheme + role, scales = "free_x") +
    labs(title = "External logger CPU% (trimmed to phase windows)", x = "Unix time (s)", y = "CPU %")
  ggsave(file.path(OUTDIR, "external_cpu_timeseries.png"), p_cpu, width = 14, height = 7, dpi = 200)
  
  # If TCP columns exist, plot tcp_outsegs_d; else plot udp_out_d
  if ("tcp_outsegs_d" %in% colnames(trimmed)) {
    p_net <- trimmed %>%
      ggplot(aes(x = ts, y = tcp_outsegs_d)) +
      geom_line() +
      facet_grid(phase ~ scheme + role, scales = "free_x") +
      labs(title = "External logger TCP outsegs/s (when available)", x = "Unix time (s)", y = "tcp_outsegs_d")
    ggsave(file.path(OUTDIR, "external_tcp_outsegs_timeseries.png"), p_net, width = 14, height = 7, dpi = 200)
  } else if ("udp_out_d" %in% colnames(trimmed)) {
    p_net <- trimmed %>%
      ggplot(aes(x = ts, y = udp_out_d)) +
      geom_line() +
      facet_grid(phase ~ scheme + role, scales = "free_x") +
      labs(title = "External logger UDP out/s (sanity)", x = "Unix time (s)", y = "udp_out_d")
    ggsave(file.path(OUTDIR, "external_udp_out_timeseries.png"), p_net, width = 14, height = 7, dpi = 200)
  }
}

# -----------------------------
# Console summary (quick glance)
# -----------------------------
message("\nWrote outputs to: ", normalizePath(OUTDIR))
message("\nKey tables:")
message("  - cloudsuite_phase_summary.csv")
message("  - receiver_vs_donor_comparison.csv")
if (nrow(external_all) > 0) {
  message("  - external_logger_trimmed_to_cloudsuite_windows.csv")
}

message("\nDone.\n")
