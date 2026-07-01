library(ggplot2) # ETC & Random Experiments - Memory Transition Plots
library(dplyr)
library(viridis)  # Load viridis for better colors

# Set base directory to current working directory
base_dir <- getwd()

# List of schemes
schemes <- c("memexchange", "memsweeper", "base")

# Define columns
column1 <- "extra_remote"
column2 <- "total"

# Initialize a list to store data for each scheme
scheme_data <- list()

# Regular expression pattern for matching file names
file_pattern <- "^memory_stats_\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}_\\d{5}\\.csv$"

# Read all CSV files for each scheme
for (scheme in schemes) {
  memory_dir <- file.path(base_dir, scheme, "memory")
  files <- list.files(memory_dir, pattern = file_pattern, full.names = TRUE)
  
  tenant_data <- list()  # Store data per tenant
  
  for (file in files) {
    # Extract the tenant name from the filename (IP and port)
    file_name <- basename(file)
    tenant_id <- sub("memory_stats_(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}_\\d{5})\\.csv", "\\1", file_name)
    
    # Read the CSV file
    data <- read.csv(file, stringsAsFactors = FALSE)
    
    # Skip files that have no data (only column names)
    if (nrow(data) < 100) {
      message(paste("Skipping file (less than 100 rows):", file_name))
      next
    }
    
    # Convert columns to numeric
    data[[column2]] <- as.numeric(data[[column2]])
    
    # Ensure "extra_remote" column exists, otherwise use only "total"
    if (column1 %in% colnames(data)) {
      data[[column1]] <- as.numeric(data[[column1]])
      last_value <- data[[column2]][3600] + data[[column1]][3600]  # Sum at last row
    } else {
      message(paste("Column", column1, "not found in:", file_name, "- Using only", column2))
      last_value <- data[[column2]][3600]  # Use only "total" at last row
    }
    
    # Store the two points (100th row & last row)
    tenant_data[[tenant_id]] <- data.frame(
      Time = c(100, 3600),  # x-axis (time points)
      Value = c(data[[column2]][100], last_value),  # y-axis
      Tenant = tenant_id
    )
  }
  
  # Store all tenant data under this scheme
  scheme_data[[scheme]] <- do.call(rbind, tenant_data)
}

# Create a plot for each scheme
output_dir <- file.path(base_dir, "memory_plots")
dir.create(output_dir, showWarnings = FALSE)  # Create output directory if it doesn't exist

for (scheme in schemes) {
  # Extract data for the scheme
  scheme_df <- scheme_data[[scheme]]
  
  # Plot the lines connecting the two points for each tenant
  p <- ggplot(scheme_df, aes(x = Time, y = Value, group = Tenant, color = Tenant)) +
    geom_line(size = 1) +
    geom_point(size = 3) +  # Highlight points
    scale_color_viridis_d(option = "turbo") +  # Better colors
    labs(title = paste("", ""),
         x = "Time (second)",
         y = "Memory (MB)",
         color = "Tenant") +
    theme_minimal() +
    theme(
      axis.title = element_text(size = 14, face = "bold"),
      axis.text = element_text(size = 12),
      legend.position = "bottom",  # Move legend inside margins
      legend.box = "horizontal",  # Arrange items in a row
      legend.spacing.x = unit(0.2, 'cm'),  # Reduce spacing
      legend.text = element_text(size = 7),  # Adjust text size
      legend.title = element_text(size = 12, face = "bold"),  
      legend.key.size = unit(0.3, "cm")  # Resize keys
    )
  
  
  # Save the plot as PDF
  pdf_filename <- file.path(output_dir, paste0(scheme, "_transition_plot.pdf"))
  ggsave(pdf_filename, plot = p, width = 8, height = 8)
}

print("Transition plots saved successfully!")
