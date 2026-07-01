library(ggplot2) # Random Experiment - Latency Plots & Grid
library(dplyr)
library(tidyr)
library(gridExtra)

# Set base directory
base_dir <- getwd()

# List of schemes
schemes <- c("MemExchange", "MemSweeper", "Memcached", "Infiniswap")

# Define the column to plot
latency_column <- "avg_lat"  # Change this if needed

# Initialize a list to store data for each tenant
tenant_data <- list()
scheme_med_latencies <- data.frame(Scheme = character(), MedianLatency = numeric(), stringsAsFactors = FALSE)

# Regular expression pattern for matching file names
file_pattern <- "^cloudsuite_\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}_\\d{5}\\.csv$"

scheme_all_seconds <- list()
# Read all CSV files for each scheme
for (scheme in schemes) {
  cloudsuite_dir <- file.path(base_dir, scheme, "cloudsuite")
  files <- list.files(cloudsuite_dir, pattern = file_pattern, full.names = TRUE)
  
  for (file in files) {
    # Extract the tenant name from the filename (IP and port)
    file_name <- basename(file)
    tenant_id <- sub("cloudsuite_(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}_\\d{5})\\.csv", "\\1", file_name)
    
    # Read CSV file
    data <- read.csv(file, stringsAsFactors = FALSE, check.names = FALSE)
    
    # Skip empty files
    if (nrow(data) == 0) {
      message(paste("Skipping empty file:", file_name))
      next
    }
    
    # Check if latency column exists
    if (!(latency_column %in% colnames(data))) {
      message(paste("Column", latency_column, "not found in:", file_name))
      next
    }
    
    # Convert latency column to numeric
    data[[latency_column]] <- as.numeric(data[[latency_column]])
    
    # Append raw per-second latencies for Option B
    scheme_all_seconds[[scheme]] <- c(
      scheme_all_seconds[[scheme]],
      data[[latency_column]]
    )
    
    # Add metadata
    data$Time <- seq_len(nrow(data))  # Assuming each row represents one second
    data$Scheme <- scheme
    data$Tenant <- tenant_id
    
    if (nrow(data) >= 1000) {  # Skip tenants with fewer than 1000 rows
      # tenant_key <- paste(scheme, tenant_id, sep = "_")
      # tenant_data[[tenant_key]] <- data
      
      tenant_key <- tenant_id  # keep grouping by tenant
      tenant_data[[tenant_key]] <- rbind(tenant_data[[tenant_key]], data)
      
      # Compute median latency for this tenant
      # Count latency spikes (values >= 1)
      num_spikes <- sum(data[[latency_column]] >= 1, na.rm = TRUE)
      
      # Filter out latency spikes
      filtered_latencies <- data[[latency_column]][data[[latency_column]] < 1]
      
      # Compute median latency, ignoring spikes
      # avg_latency <- median(filtered_latencies, na.rm = TRUE)
      # Compute median latency including spikes
      median_latency <- median(data[[latency_column]], na.rm = TRUE)
      
      
      # Print the per-tenant median latency
      cat("Tenant:", tenant_id, "| Scheme:", scheme, "| Median Latency (No Spikes):", median_latency, "| Spikes:", num_spikes, "\n")
      
      # Store the median latency per scheme
      scheme_med_latencies <- rbind(scheme_med_latencies, data.frame(Scheme = scheme, MedianLatency = median_latency, Spikes = num_spikes))
    }
    
  }
}

# -----------------------------------------
# HIT RATE ANALYSIS
# -----------------------------------------

scheme_hit_data <- data.frame()

initial_window_size <- 60   # first 60 seconds
final_window_size   <- 60   # last 60 seconds

for (scheme in schemes) {
  cloudsuite_dir <- file.path(base_dir, scheme, "cloudsuite")
  files <- list.files(cloudsuite_dir, pattern = file_pattern, full.names = TRUE)
  
  for (file in files) {
    
    data <- read.csv(file, stringsAsFactors = FALSE, check.names = FALSE)
    if (nrow(data) < 1000) next
    
    data$Time <- seq_len(nrow(data))
    
    # --------- Overall steady-state hit rate ---------
    total_hits <- sum(data$hits, na.rm = TRUE)
    total_gets <- sum(data$gets, na.rm = TRUE)
    overall_hr <- total_hits / total_gets
    
    # --------- Initial window ---------
    initial_data <- head(data, initial_window_size)
    init_hr <- sum(initial_data$hits) / sum(initial_data$gets)
    
    # --------- Final window ---------
    final_data <- tail(data, final_window_size)
    final_hr <- sum(final_data$hits) / sum(final_data$gets)
    
    scheme_hit_data <- rbind(
      scheme_hit_data,
      data.frame(
        Scheme = scheme,
        OverallHR = overall_hr,
        InitialHR = init_hr,
        FinalHR = final_hr
      )
    )
  }
}

hit_summary <- scheme_hit_data %>%
  group_by(Scheme) %>%
  summarise(
    OverallHR = mean(OverallHR),
    InitialHR = mean(InitialHR),
    FinalHR   = mean(FinalHR)
  )

# Improvement vs own initial
hit_summary <- hit_summary %>%
  mutate(
    AbsoluteImprovement = FinalHR - InitialHR,
    RelativeImprovementPct = 100 * (FinalHR - InitialHR) / InitialHR
  )

baseline_final <- hit_summary %>%
  filter(Scheme == "Memcached") %>%
  pull(FinalHR)

hit_summary <- hit_summary %>%
  mutate(
    ImprovementVsBaseline = FinalHR - baseline_final,
    RelativeVsBaselinePct = 100 * (FinalHR - baseline_final) / baseline_final,
    MissReductionVsBaseline = 
      ( (1 - baseline_final) - (1 - FinalHR) ) / (1 - baseline_final) * 100
  )

cat("\nHit Rate Summary (Steady State):\n")
print(hit_summary)

# Compute the median latency per scheme
final_median_latencies <- scheme_med_latencies %>%
  group_by(Scheme) %>%
  summarise(
    MedianLatency = median(MedianLatency, na.rm = TRUE),
    TotalSpikes = sum(Spikes, na.rm = TRUE)  # Sum all spikes per scheme
  )

# Option B: pooled median across all seconds per scheme
optionB_latencies <- data.frame(
  Scheme = names(scheme_all_seconds),
  PooledMedianLatency = sapply(scheme_all_seconds, function(x) {
    median(x, na.rm = TRUE)
  })
)

comparison <- final_median_latencies %>%
  rename(OptionA_Median = MedianLatency) %>%
  left_join(optionB_latencies, by = "Scheme")

comparison <- comparison %>%
  mutate(
    PercentDiff = 100 * (PooledMedianLatency - OptionA_Median) / OptionA_Median
  )

cat("\nComparison of Option A vs Option B:\n")
print(comparison)

# Print final median latencies along with total spikes
cat("\nMedian Median Latencies and Total Spikes per Scheme:\n")
print(final_median_latencies)

# Print just the total spikes per scheme for clarity
cat("\nTotal Number of Latency Spikes per Scheme:\n")
final_spikes <- final_median_latencies %>% select(Scheme, TotalSpikes)
print(final_spikes)


# Create output directory
output_dir <- file.path(base_dir, "latency_plots")
dir.create(output_dir, showWarnings = FALSE)

tenant_count <- 0
batch_count <- 1
plots <- list()

for (tenant in names(tenant_data)) {
  tenant_df <- tenant_data[[tenant]]
  
  # Create the latency plot
  p <- ggplot(tenant_df, aes(x = Time, y = .data[[latency_column]] * 1000, color = Scheme)) +
    geom_line(size = 2) +
    coord_cartesian(xlim = c(0, 5000)) +
    labs(title = paste("", tenant),
         x = "Time (seconds)",
         y = "Average Request Latency (µs)",
         color = "Scheme") +
    theme_minimal() +
    theme(
      axis.line = element_line(size = 1.2, color = "black"),
      axis.title = element_text(size = 42, face = "bold"),
      axis.text = element_text(size = 40, face = "bold"),
      plot.title = element_text(hjust = 0.5, size = 50, face = "bold"),
      legend.key.size = unit(2, "cm"),
      legend.text = element_text(size = 40),
      legend.title = element_text(size = 45, face = "bold")
    ) +
    theme(legend.position = "none") +  # Remove legend from individual plots
    scale_y_log10()
  # Add plot to list
  if (nrow(tenant_df) > 1000) {
    plots[[length(plots) + 1]] <- p
  
    # Every 5 tenants, save them in a single PDF
    tenant_count <- tenant_count + 1
    if (tenant_count %% 5 == 0 || tenant_count == length(tenant_data)) {
      pdf_filename <- file.path(output_dir, paste0("latency_batch_", batch_count, ".pdf"))

      # Generate a separate plot just for the legend
      legend_plot <- ggplot(tenant_df, aes(x = Time, y = get(latency_column), color = Scheme)) +
        geom_line(linewidth = 2) +
        theme_minimal() +
        theme(legend.position = "bottom", legend.key.size = unit(6, "cm"),
              legend.text = element_text(size = 60),
              legend.title = element_text(size = 65, face = "bold")) +
        labs(color = "Type")+  # Legend title
        guides(color = guide_legend(override.aes = list(linewidth = 40, linelength = 5))) 
      
      
      # Extract the legend from the plot
      extract_legend <- function(plot) {
        g <- ggplotGrob(plot)
        legends <- g$grobs[sapply(g$grobs, function(x) x$name) == "guide-box"]
        if (length(legends) > 0) return(legends[[1]])
        return(NULL)
      }
      
      legend_grob <- extract_legend(legend_plot)
      
      # Arrange plots vertically with the legend at the bottom
      pdf(pdf_filename, width = 45, height = 60)
      grid.arrange(grobs = c(plots, list(legend_grob)), ncol = 1, nrow = length(plots) + 1)  # Add legend as last row
      dev.off()
      
      # Reset for next batch
      plots <- list()
      batch_count <- batch_count + 1
    }
  }
  
  # Stop after 20 tenants
  if (tenant_count >= 20) break
}

print("Latency plots saved successfully, with 5 stacked in each PDF!")
