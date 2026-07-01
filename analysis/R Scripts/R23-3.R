library(ggplot2)  # Random Experiment - Memory Utilization Plot
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
  
  # Initialize a vector to store ignored file names (over-provisioned)
  ignored_files <- c()
  
  for (file in files) {
    # Read only the first two columns with headers
    df <- read.csv(file, header = TRUE, stringsAsFactors = FALSE)[, 1:2]
    
    # Rename columns for clarity (assuming first column is RemoteMemory, second is LocalMemory)
    colnames(df) <- c("RemoteMemory", "LocalMemory")
    
    # Convert columns to numeric, handling errors
    df$RemoteMemory <- as.numeric(df$RemoteMemory)
    df$LocalMemory <- as.numeric(df$LocalMemory)
    
    # Replace NA values with 0
    df[is.na(df)] <- 0
    
    # Extend data to 18,000 seconds if necessary
    if (nrow(df) < 18000 && nrow(df) > 0) {
      print(paste("Extending file with less than 18,000 rows:", file))
      
      last_remote <- tail(df$RemoteMemory, 1)
      last_local <- tail(df$LocalMemory, 1)
      
      if (!is.na(last_remote) && !is.na(last_local)) {
        df$Time <- 1:nrow(df)
        missing_rows <- data.frame(
          RemoteMemory = rep(last_remote, 18000 - nrow(df)),
          LocalMemory = rep(last_local, 18000 - nrow(df)),
          Time = seq(nrow(df) + 1, 18000)
        )
        df <- bind_rows(df, missing_rows)
      } else {
        print(paste("Skipping extension due to missing values in:", file))
      }
    } else {
      df$Time <- 1:nrow(df)
    }
    
    # Determine if tenant is over-provisioned
    reached_target <- tail(df$LocalMemory, 1) == 3938.76
    memory_decreased <- any(diff(df$LocalMemory) < 0)
    last_remote_memory <- tail(df$RemoteMemory, 1)
    
    if (memory_decreased && last_remote_memory > 0) {
      memory_decreased <- FALSE
    }
    
    is_over_provisioned <- reached_target || memory_decreased
    
    # Handle over-provisioned tenants
    if (is_over_provisioned) {
      df$TotalMemory <- 0
      ignored_files <- c(ignored_files, basename(file))
    } else {
      df$TotalMemory <- df$RemoteMemory + df$LocalMemory
    }
    
    # Store results using bind_rows
    memory_data <- bind_rows(memory_data, df)
  }
  
  # Print ignored files for this scheme
  if (length(ignored_files) > 0) {
    cat("Ignored files for", scheme_name, "(over-provisioned tenants):\n")
    print(ignored_files)
  } else {
    cat("No over-provisioned tenants ignored for", scheme_name, ".\n")
  }
  
  # Aggregate memory utilization over all under-provisioned tenants
  memory_summary <- memory_data %>%
    group_by(Time) %>%
    summarise(TotalUtilizedMemory = sum(TotalMemory, na.rm = TRUE))
  
  # Convert memory usage to percentage of total cluster memory
  memory_summary$UtilizationPercentage <- (memory_summary$TotalUtilizedMemory / total_memory_cluster) * 100
  
  # Ensure x-axis ends at 18,000 seconds
  memory_summary <- memory_summary %>% filter(Time <= 18000)
  
  # Add scheme name for combined plotting
  memory_summary$Scheme <- scheme_name
  
  return(memory_summary)
}

# Process each scheme and combine data
for (scheme in names(schemes)) {
  scheme_data <- process_scheme_memory(scheme, schemes[[scheme]])
  combined_memory_data <- bind_rows(combined_memory_data, scheme_data)
}

# Extract the last recorded value for each scheme
final_labels <- combined_memory_data %>%
  group_by(Scheme) %>%
  filter(Time == max(Time))  # Get last recorded value for each scheme

# Create the combined memory utilization plot with area fill
plot <- ggplot(combined_memory_data, aes(x = Time, y = UtilizationPercentage, group = Scheme)) +
  geom_area(aes(fill = Scheme), alpha = 0.2, position = "identity") +  # Soft fill under lines
  geom_line(aes(color = Scheme), size = 1) +  # Maintain strong line visibility
  geom_text(data = final_labels, 
            aes(label = paste0(round(UtilizationPercentage, 1), "%"), 
                color = Scheme), 
            hjust = 0.5, vjust = -0.3, size = 3.5, fontface = "bold") +  # Labels at end of lines
  scale_fill_manual(values = c("blue", "red", "green", "purple", "orange")) +  # Custom fill colors
  scale_color_manual(values = c("blue", "red", "green", "purple", "orange")) +  # Matching line colors
  labs(
    title = "",
    x = "Time (seconds)",
    y = "Memory Utilization (%)",
    fill = "Scheme",
    color = "Scheme"
  ) +
  scale_x_continuous(limits = c(0, 18000), breaks = seq(0, 18000, by = 5000)) +
  scale_y_continuous(limits = c(0, 100), breaks = seq(0, 100, by = 10)) +
  theme_minimal(base_size = 14) +
  theme(
    plot.title = element_text(hjust = 0.5, face = "bold"),
    axis.title.x = element_text(face = "bold", size = 12),
    axis.title.y = element_text(face = "bold", size = 12),
    axis.text = element_text(size = 10),
    axis.line = element_line(size = 1.2, color = "black"),
    panel.grid.major = element_line(color = "gray80"),
    panel.grid.minor = element_blank(),
    legend.position = "bottom",
    legend.key.size = unit(1, "cm"),  
    legend.text = element_text(size = 10, face = "bold"),  
    legend.title = element_text(size = 10, face = "bold")
  ) +
  guides(fill = guide_legend(override.aes = list(alpha = 0.4)))

# Save the combined plot as a PDF
output_path <- file.path(output_dir, "Memory_Utilization_Comparison_Random.pdf")
ggsave(output_path, plot, width = 5, height = 5)

# Display message
cat("Memory utilization comparison plot saved as", output_path, "\n")
