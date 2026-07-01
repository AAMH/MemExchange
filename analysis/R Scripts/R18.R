library(ggplot2) # ETC & Random Experiments - Hit Ratio Over time Grid
library(dplyr)
library(tidyr)
library(patchwork)  # For combining plots

# Set base directory to current working directory
base_dir <- getwd()

# List of schemes
schemes <- c("memexchange", "memsweeper", "base")

# Initialize a list to store data for each tenant
tenant_data <- list()

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
    
    # Skip files that have no data (only column names)
    if (nrow(data) == 0) {
      message(paste("Skipping empty file:", file_name))
      next
    }
    
    # Add scheme and time columns
    data$Time <- seq_len(nrow(data))  # Assuming each row is one second
    data$Scheme <- scheme
    data$Tenant <- tenant_id  # Assign tenant ID
    
    # Store data
    if (!tenant_id %in% names(tenant_data)) {
      tenant_data[[tenant_id]] <- data
    } else {
      tenant_data[[tenant_id]] <- rbind(tenant_data[[tenant_id]], data)
    }
  }
}

# Create plots without legends
plots <- list()
i <- 1  # Counter for tracking tenants

for (tenant in names(tenant_data)) {
  tenant_df <- tenant_data[[tenant]]
  
  p <- ggplot(tenant_df, aes(x = Time, y = overall_hit_rate, color = Scheme, group = Scheme, size = Scheme)) +
    geom_line() +
    scale_size_manual(values = c("memexchange" = 1.5, "memsweeper" = 1.5, "base" = 3)) +  # Adjust line thickness
    labs(title = tenant, x = "Time (seconds)", y = "Overall Hit Rate (%)") +
    theme_minimal() +
    theme(
      legend.position = "none",
      axis.title = element_text(size = 16, face = "bold"),
      axis.text = element_text(size = 14)
    )  # Remove legends from individual plots
  
  p <- p + coord_cartesian(xlim = c(0, 10000), ylim = c(25, 75))
  
  plots[[i]] <- p
  i <- i + 1
  
  # Stop at 10 tenants for grid
  if (i > 10) break
}

# Create plots for 20 tenants (if available)
output_dir <- file.path(base_dir, "hitrate_plots")
dir.create(output_dir, showWarnings = FALSE)  # Create output directory if it doesn't exist

# Create an extra plot with only the legend
# Create an extra plot with only the legend but with increased size
legend_plot <- ggplot(tenant_data[[1]], aes(x = Time, y = overall_hit_rate, color = Scheme, size = Scheme)) +
  geom_line() +
  scale_size_manual(values = c("memexchange" = 8, "memsweeper" = 8, "base" = 8)) +
  labs(color = "Scheme", size = "Scheme") +
  theme_void() +  # Remove axes, labels, etc.
  theme(
    legend.position = "bottom",  # Move legend to the right (vertical layout)
    legend.direction = "vertical",  
    legend.text = element_text(size = 16),  # Increase legend text size
    legend.title = element_text(size = 18), # Increase legend title size
    legend.key.size = unit(1.5, "cm")       # Increase legend key size
  )

# Extract the legend
legend_only <- cowplot::get_legend(legend_plot)

# Convert legend into a standalone ggplot object with increased size
legend_only_plot <- cowplot::ggdraw() + cowplot::draw_grob(legend_only, x = 0.5, y = 0, scale = 1.2)

# Arrange plots in a 4-column grid with the legend as the 11th plot
final_plot <- patchwork::wrap_plots(c(plots, list(legend_only_plot)), ncol = 4)

# Save as a single PDF
pdf_filename <- file.path(output_dir, "all_tenants_grid.pdf")
ggsave(pdf_filename, plot = final_plot, width = 15, height = 15)

print("Grid of plots saved successfully!")
