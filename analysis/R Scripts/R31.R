library(dplyr) # ETC Experiment
library(stringr)
library(purrr)

schemes <- c("MemExchange", "MemSweeper", "Memcached", "Infiniswap")

# ---- Function to parse one mutilate log ----
parse_mutilate_log <- function(filepath, scheme_name) {
  lines <- readLines(filepath, warn = FALSE)
  
  read_line   <- lines[str_detect(lines, "^read")]
  update_line <- lines[str_detect(lines, "^update")]
  
  parse_table_line <- function(line) {
    nums <- str_split(str_trim(line), "\\s+")[[1]]
    list(
      avg = as.numeric(nums[2]),
      p99 = as.numeric(nums[8])
    )
  }
  
  read_stats   <- parse_table_line(read_line)
  update_stats <- parse_table_line(update_line)
  
  # Extract QPS + total requests
  qps_line <- lines[str_detect(lines, "Total QPS")]
  
  qps <- as.numeric(str_match(qps_line, "Total QPS = ([0-9\\.]+)")[,2])
  total_requests <- as.numeric(str_match(qps_line, "\\(([0-9]+)")[,2])
  
  # Extract Miss %
  miss_line <- lines[str_detect(lines, "^Misses")]
  miss_rate <- as.numeric(str_match(miss_line, "\\(([0-9\\.]+)%\\)")[,2])
  
  tibble(
    scheme = scheme_name,
    file   = basename(filepath),
    read_avg   = read_stats$avg,
    read_p99   = read_stats$p99,
    update_avg = update_stats$avg,
    update_p99 = update_stats$p99,
    qps        = qps,
    total_requests = total_requests,
    miss_rate  = miss_rate
  )
}

# ---- Parse everything ----
all_results <- map_dfr(schemes, function(scheme) {
  
  files <- list.files(
    file.path(scheme, "mutilate_results"),
    pattern = "^mutilate_.*\\.out$",
    full.names = TRUE
  )
  
  map_dfr(files, parse_mutilate_log, scheme_name = scheme)
})

# ---- Summarize per scheme ----
summary_table <- all_results %>%
  group_by(scheme) %>%
  summarise(
    # Latency → median across tenants
    read_avg_median   = median(read_avg, na.rm = TRUE),
    read_p99_median   = median(read_p99, na.rm = TRUE),
    update_avg_median = median(update_avg, na.rm = TRUE),
    update_p99_median = median(update_p99, na.rm = TRUE),
    
    # QPS → sum
    total_qps = sum(qps, na.rm = TRUE),
    qps_mean = mean(qps),
    
    # Weighted miss rate
    weighted_miss_rate = 
      100 * sum((miss_rate/100) * total_requests, na.rm = TRUE) /
      sum(total_requests, na.rm = TRUE)
  )

print(summary_table)