library(ggplot2) # ETC & Random Experiments - Overall Hit Ratio Bar Chart
library(dplyr)
library(tidyr)

# Set base directory to current working directory
base_dir <- getwd()

# List of schemes
schemes <- c("memexchange", "memsweeper", "base")

# Initialize a list to store data for each tenant
tenant_data <- data.frame()

# Regular expression pattern for matching file names
file_pattern <- "^hit_stats_\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}_\\d{5}\\.csv$"

# Read all CSV files for each scheme
for (scheme in schemes) {
  hit_ratio_dir <- file.path(base_dir, scheme, "hitRatio")
  files <- list.files(hit_ratio_dir, pattern = file_pattern, full.names = TRUE)
  
  for (file in files) {
    # Extract the tenant name from the filename (IP and port)
    file_name <- basename(file)
    tenant_id <- sub("hit_stats_(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}_\\d{5})\\.csv", "\\1", file_name)
    
    # Read the CSV file
    data <- read.csv(file)
    
    # Skip files that have no data (only column names) or don't have 18000 rows
    if (nrow(data) < 3600) {
      message(paste("Skipping file (not enough rows):", file_name))
      next
    }
    
    # Extract the value of the 3600th row // last available row
    overall_hit_value <- data$overall_hit_rate[14970]
    
    # Append data
    tenant_data <- rbind(tenant_data, data.frame(Tenant = tenant_id, Scheme = scheme, HitRate = overall_hit_value))
  }
}

# Create plots for 20 tenants (if available)
output_dir <- file.path(base_dir, "hitrate_plots")
dir.create(output_dir, showWarnings = FALSE)  # Create output directory if it doesn't exist

# Convert to data frame and assign T1, T2, ... labels
tenant_data <- as.data.frame(tenant_data)
tenant_data <- tenant_data %>%
  mutate(TenantNum = as.integer(factor(Tenant))) %>%
  arrange(TenantNum) %>%
  mutate(TenantLabel = factor(paste0("T", TenantNum),
                              levels = paste0("T", sort(unique(TenantNum)))))


# Compute the average hit rate per scheme
average_hit_rate <- tenant_data %>%
  group_by(Scheme) %>%
  summarise(AverageHitRate = mean(HitRate, na.rm = TRUE))

# Print the results
print(average_hit_rate)

# Create bar chart
p <- ggplot(tenant_data, aes(x = TenantLabel, y = HitRate, fill = Scheme)) +
  geom_bar(stat = "identity", position = position_dodge(width = 0.6), width = 0.6) +
  scale_fill_manual(values = c("memexchange" = "#1b9e77", "memsweeper" = "#d95f02", "base" = "#7570b3")) +
  ylim(0, 100) +
  labs(title = "",
       x = "Tenant",
       y = "Overall Hit Rate (%)",
       fill = "Scheme") +
  theme_minimal() +
  theme(
    axis.title = element_text(size = 22, face = "bold", margin = margin(t = 10, b = 10)),
    axis.text = element_text(size = 16, color = "black", face = "bold"),
    axis.line = element_line(size = 1.2, color = "black"),
    axis.ticks = element_line(size = 1, color = "black"),
#    axis.text.x = element_text(angle = 45, hjust = 1, size = 10),
    legend.title = element_text(size = 18, face = "bold"),
    legend.text = element_text(size = 16),
    legend.key.size = unit(2, "lines"),
    legend.position = "bottom"
  ) +
  coord_cartesian(ylim = c(90, 100))

# Save as PDF
pdf_filename <- file.path(output_dir, "all_tenants_barchart.pdf")
ggsave(pdf_filename, plot = p, width = 12, height = 6)

print("Single bar chart saved successfully!")
