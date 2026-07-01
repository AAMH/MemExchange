library(ggplot2)  # ETC Experiment - Memory Utilization Plot
library(dplyr)    

# Experiment Parameters
total_memory_cluster <- 60 * 1024  # Total cluster memory (MB)

# Define schemes and their corresponding data folders
schemes <- list("MemSweeper" = "Memsweeper/memory/", "MemExchange" = "Memexchange/memory/")

# Ensure the output directory exists
output_dir <- "memory_plots"
if (!dir.exists(output_dir)) {
  dir.create(output_dir)
}

# Initialize a combined data frame
combined_memory_data <- data.frame()

# Function to process memory utilization for a given scheme
process_scheme_memory <- function(scheme_name, folder_path) {
  # List all CSV files in the folder ending with '2' or '3'
  files <- list.files(path = folder_path, pattern = ".*[23]\\.csv$", full.names = TRUE)
  
  # Initialize an empty data frame to store memory utilization over time
  memory_data <- data.frame()
  
  for (file in files) {
    # Read only the first two columns with headers
    df <- read.csv(file, header = TRUE, stringsAsFactors = FALSE)[, 1:2]
    
    # Keep only the first 3600 rows
    df <- df[1:min(nrow(df), 10000), ]
    
    # Rename columns for clarity (assuming first column is RemoteMemory, second is LocalMemory)
    colnames(df) <- c("RemoteMemory", "LocalMemory")
    
    # Convert columns to numeric, handling errors
    df$RemoteMemory <- as.numeric(df$RemoteMemory)
    df$LocalMemory <- as.numeric(df$LocalMemory)
    
    # Replace NA values with 0
    df[is.na(df)] <- 0
    
    # Calculate total allocated memory at each time step
    df$TotalMemory <- df$RemoteMemory + df$LocalMemory
    
    # Add a time column
    df$Time <- 1:nrow(df)
    
    # Store results using bind_rows
    memory_data <- bind_rows(memory_data, df)
  }

  # Aggregate memory utilization across all tenants
  memory_summary <- memory_data %>%
    group_by(Time) %>%
    summarise(TotalUtilizedMemory = sum(TotalMemory, na.rm = TRUE))

  # Convert memory usage to percentage of total cluster memory
  memory_summary$UtilizationPercentage <- (memory_summary$TotalUtilizedMemory / total_memory_cluster) * 100

  # Ensure x-axis ends at 3600 seconds
  memory_summary <- memory_summary %>% filter(Time <= 10000)

  # Add scheme name for combined plotting
  memory_summary$Scheme <- scheme_name
  
  return(memory_summary)
}

# Process each scheme and combine data
for (scheme in names(schemes)) {
  scheme_data <- process_scheme_memory(scheme, schemes[[scheme]])
  combined_memory_data <- bind_rows(combined_memory_data, scheme_data)
}

final_labels <- combined_memory_data %>%
  group_by(Scheme) %>%
  filter(Time == max(Time))  # Get last recorded value for each scheme

# Create the filled line plot (soft area fill)
plot <- ggplot(combined_memory_data, aes(x = Time, y = UtilizationPercentage, group = Scheme)) +
  geom_area(aes(fill = Scheme), alpha = 0.2, position = "identity") +  # Soft fill under each line
  geom_line(aes(color = Scheme), size = 1.5) +  # Keep strong lines
  geom_text(data = final_labels, 
            aes(label = paste0(round(UtilizationPercentage, 1), "%"), 
                color = Scheme), 
            hjust = -0.1, vjust = 0.5, size = 5, fontface = "bold") +  # Label at the end of the line
  scale_fill_manual(values = c("blue", "red", "green", "purple", "orange")) +  # Custom colors
  scale_color_manual(values = c("blue", "red", "green", "purple", "orange")) +  # Matching line colors
  labs(
    title = "",
    x = "Time (seconds)",
    y = "Memory Utilization (%)",
    fill = "Scheme",
    color = "Scheme"
  ) +
  scale_x_continuous(limits = c(0, 10300), breaks = seq(0, 10000, by = 1000)) +
  scale_y_continuous(limits = c(0, 50), breaks = seq(0, 100, by = 10)) +
  theme_minimal(base_size = 14) +
  theme(
    plot.title = element_text(hjust = 0.5, face = "bold"),
    axis.title.x = element_text(face = "bold"),
    axis.title.y = element_text(face = "bold"),
    axis.text = element_text(size = 12),
    axis.line = element_line(size = 1.2, color = "black"),
    panel.grid.major = element_line(color = "gray80"),
    panel.grid.minor = element_blank(),
    legend.position = "bottom",
    legend.key.size = unit(1.5, "cm"),  
    legend.text = element_text(size = 15, face = "bold"),  
    legend.title = element_text(size = 17, face = "bold")
  ) +
  guides(fill = guide_legend(override.aes = list(alpha = 0.4)))  # Adjust transparency in legend

# Save the combined plot as a PDF
output_path <- file.path(output_dir, "Memory_Utilization_Comparison.pdf")
ggsave(output_path, plot, width = 10, height = 8)

# Display message
cat("Memory utilization comparison plot saved as", output_path, "\n")
