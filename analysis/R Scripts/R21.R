library(ggplot2) # ETC & Random Experiments - Memory Over Time
library(dplyr)
library(tidyr)
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
    if (nrow(data) == 0) {
      message(paste("Skipping empty file:", file_name))
      next
    }
    
    # Convert "total" column to numeric
    data[[column2]] <- as.numeric(data[[column2]])
    
    # Check if "extra_remote" exists
    if (column1 %in% colnames(data)) {
      data[[column1]] <- as.numeric(data[[column1]])
      data$Sum <- data[[column1]] + data[[column2]]  # Sum both columns
    } else {
      message(paste("Column", column1, "not found in:", file_name, "- Using only", column2))
      data$Sum <- data[[column2]]  # Use only "total"
    }
    
    # Add time and tenant columns
    data$Time <- seq_len(nrow(data))  # Assuming each row is one second
    data$Tenant <- tenant_id  # Assign tenant ID
    
    # Store data for this tenant
    tenant_data[[tenant_id]] <- data
  }
  
  # Store all tenant data under this scheme
  scheme_data[[scheme]] <- tenant_data
}

# Create a plot for each scheme
output_dir <- file.path(base_dir, "memory_plots")
dir.create(output_dir, showWarnings = FALSE)  # Create output directory if it doesn't exist

for (scheme in schemes) {
  tenant_plots <- list()  # Store curves for all tenants
  
  for (tenant in names(scheme_data[[scheme]])) {
    tenant_df <- scheme_data[[scheme]][[tenant]]
    
    tenant_plots[[tenant]] <- geom_line(data = tenant_df, aes(x = Time, y = Sum, color = Tenant), size = 1)
  }
  
  # Combine all tenant curves into one plot
  p <- ggplot() +
    tenant_plots +
    scale_color_viridis_d(option = "turbo") +  # Better colors
    labs(title = paste("", ""),
         x = "Time (seconds)",
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
  
  p <- p + coord_cartesian(xlim = c(0, 10000), ylim = c(0, 6000))
  
  # Save the plot as PDF
  pdf_filename <- file.path(output_dir, paste0(scheme, "_memory_usage.pdf"))
  ggsave(pdf_filename, plot = p, width = 8, height = 8)
}

print("Memory usage plots saved successfully!")
