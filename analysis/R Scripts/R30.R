# ============================================================
# CloudSuite RPS Sweep Analysis (RDMA vs RXE vs TCP)
# ============================================================

suppressPackageStartupMessages({
  library(data.table)
  library(ggplot2)
})

# -------------------------
# Configuration
# -------------------------
root_dir <- "."                 # directory containing RDMA/, RXE/, TCP/
schemes  <- c("RDMA", "RXE", "TCP")

out_dir <- file.path(root_dir, "analysis_out_rps_sweep")
dir.create(out_dir, showWarnings = FALSE, recursive = TRUE)

warmup_sec <- 30                # discard first N seconds
min_achieved_frac <- 0.98       # sustainable if achieved >= 98% of target

# -------------------------
# Helpers
# -------------------------
parse_filename_meta <- function(fname) {
  # expects: cloudsuite_rps20000_w1_c25.csv
  m <- regexec(
    "cloudsuite_rps([0-9]+)_w([0-9]+)_c([0-9]+)\\.csv",
    fname
  )
  parts <- regmatches(fname, m)[[1]]
  if (length(parts) != 4) return(NULL)
  
  list(
    target_rps = as.integer(parts[2]),
    workers    = as.integer(parts[3]),
    conns      = as.integer(parts[4])
  )
}

robust_quantile <- function(x, p) {
  x <- x[is.finite(x)]
  if (!length(x)) return(NA_real_)
  as.numeric(quantile(x, p, names = FALSE))
}

# -------------------------
# Read all CloudSuite files
# -------------------------
all_runs <- list()

for (scheme in schemes) {
  scheme_dir <- file.path(root_dir, scheme)
  
  files <- list.files(
    scheme_dir,
    pattern = "^cloudsuite_rps[0-9]+_w[0-9]+_c[0-9]+\\.csv$",
    full.names = TRUE
  )
  
  if (!length(files)) {
    warning("No files found for scheme: ", scheme)
    next
  }
  
  for (f in files) {
    meta <- parse_filename_meta(basename(f))
    if (is.null(meta)) next
    
    dt <- fread(f)
    setnames(dt, trimws(names(dt)))
    
    stopifnot("ts" %in% names(dt), "rps" %in% names(dt))
    
    dt[, ts := as.numeric(ts)]
    t0 <- min(dt$ts, na.rm = TRUE)
    dt[, t := ts - t0]
    
    # discard warmup
    dt <- dt[t >= warmup_sec]
    
    dt[, `:=`(
      scheme     = scheme,
      target_rps = meta$target_rps,
      workers    = meta$workers,
      conns      = meta$conns,
      file       = basename(f)
    )]
    
    all_runs[[length(all_runs) + 1]] <- dt
  }
}

cloudsuite <- rbindlist(all_runs, fill = TRUE)
stopifnot(nrow(cloudsuite) > 0)

# -------------------------
# Per-run summary
# -------------------------
run_summary <- cloudsuite[, .(
  achieved_rps = mean(rps, na.rm = TRUE),
  avg_lat_ms   = mean(avg_lat, na.rm = TRUE),
  p99_lat_ms   = robust_quantile(`99th`, 0.99),
  max_lat_ms   = max(max, na.rm = TRUE),
  hit_rate     = mean(hits / gets, na.rm = TRUE)
), by = .(scheme, target_rps, workers, conns, file)]

run_summary[, achieved_frac := achieved_rps / target_rps]
run_summary[, sustainable := achieved_frac >= min_achieved_frac]

fwrite(run_summary, file.path(out_dir, "table_per_run_summary.csv"))

# -------------------------
# Aggregate per scheme + RPS
# -------------------------
agg <- run_summary[, .(
  achieved_rps  = mean(achieved_rps),
  achieved_frac = mean(achieved_frac),
  avg_lat_ms    = mean(avg_lat_ms),
  p99_lat_ms    = mean(p99_lat_ms),
  sustainable   = all(sustainable)
), by = .(scheme, target_rps)]

fwrite(agg, file.path(out_dir, "table_rps_sweep_aggregate.csv"))

# -------------------------
# Capacity (knee) detection
# -------------------------
capacity <- agg[
  sustainable == TRUE,
  .(max_sustainable_rps = max(target_rps)),
  by = scheme
]

fwrite(capacity, file.path(out_dir, "table_capacity_summary.csv"))

# -------------------------
# Plot 1 — Achieved RPS vs Target RPS
# -------------------------
p1 <- ggplot(agg, aes(x = target_rps, y = achieved_rps, color = scheme)) +
  geom_line(linewidth = 1) +
  geom_point(size = 2) +
  geom_abline(slope = 1, intercept = 0, linetype = "dashed", alpha = 0.5) +
  labs(
    title = "", # "Achieved RPS vs Target RPS",
    x = "Target RPS",
    y = "Achieved RPS"
  ) +
  theme_bw(base_size = 12) +
  theme(axis.text.x = element_text(hjust = 1, size = 14, face = "bold"),
        axis.text.y = element_text(size = 14, face = "bold"),
        axis.title.x = element_text(size = 19, face = "bold"),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 19, face = "bold"),  # Bold and larger Y-axis label
        # plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
        legend.key.size = unit(1, "cm"),  # Enlarge the legend keys
        legend.text = element_text(size = 16, face = "bold"),  # Increase the text size
        legend.title = element_text(size = 18, face = "bold"),
        legend.position = "bottom") +
  guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15)))  # Increase legend key size

ggsave(
  file.path(out_dir, "plot_achieved_vs_target_rps.png"),
  p1, width = 7, height = 7, dpi = 200
)

# -------------------------
# Plot 2 — p99 latency vs Target RPS
# -------------------------
p2 <- ggplot(agg, aes(x = target_rps, y = p99_lat_ms, color = scheme)) +
  geom_line(linewidth = 1) +
  geom_point(size = 2) +
  labs(
    title = "", #"p99 Latency vs Target RPS",
    x = "Target RPS",
    y = "p99 Latency (ms)"
  ) +
  theme_bw(base_size = 12) +
  theme(axis.text.x = element_text(hjust = 1, size = 14, face = "bold"),
        axis.text.y = element_text(size = 14, face = "bold"),
        axis.title.x = element_text(size = 19, face = "bold"),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 19, face = "bold"),  # Bold and larger Y-axis label
        # plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
        legend.key.size = unit(1, "cm"),  # Enlarge the legend keys
        legend.text = element_text(size = 16, face = "bold"),  # Increase the text size
        legend.title = element_text(size = 18, face = "bold"),
        legend.position = "bottom") +
  guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15)))  # Increase legend key size

ggsave(
  file.path(out_dir, "plot_p99_latency_vs_rps.png"),
  p2, width = 7, height = 7, dpi = 200
)

# -------------------------
# Plot 3 — Achieved fraction vs RPS (collapse detector)
# -------------------------
p3 <- ggplot(agg, aes(x = target_rps, y = achieved_frac, color = scheme)) +
  geom_line(linewidth = 1) +
  geom_point(size = 2) +
  geom_hline(yintercept = min_achieved_frac, linetype = "dotted") +
  labs(
    title = "", #Achieved / Target RPS Fraction",
    x = "Target RPS",
    y = "Achieved Fraction"
  ) +
  theme_bw(base_size = 12) +
  theme(axis.text.x = element_text(hjust = 1, size = 14, face = "bold"),
        axis.text.y = element_text(size = 14, face = "bold"),
        axis.title.x = element_text(size = 19, face = "bold"),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 19, face = "bold"),  # Bold and larger Y-axis label
        # plot.title = element_text(hjust = 0.5, size = 10, face = "bold"),
        legend.key.size = unit(1, "cm"),  # Enlarge the legend keys
        legend.text = element_text(size = 16, face = "bold"),  # Increase the text size
        legend.title = element_text(size = 18, face = "bold"),
        legend.position = "bottom") +
  guides(color = guide_legend(override.aes = list(linewidth = 6, linelength = 15)))  # Increase legend key size


ggsave(
  file.path(out_dir, "plot_achieved_fraction_vs_rps.png"),
  p3, width = 7, height = 7, dpi = 200
)

# -------------------------
# Final message
# -------------------------
message("RPS sweep analysis complete.")
message("Results written to: ", normalizePath(out_dir))
