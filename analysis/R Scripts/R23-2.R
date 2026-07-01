library(ggplot2) # Cloud Experiment 2 - Memory Utilization Plot
library(dplyr)

# Experiment Parameters
num_under_provisioned <- 30  # Number of under-provisioned tenants
num_over_provisioned <- 10   # Over-provisioned tenants (fixed usage, not tracked individually)
initial_size_under <- 2001.40  # Initial size of under-provisioned tenants (MB)
fixed_over_provisioned_usage <- num_over_provisioned * (2.4 * 1024)  # Total fixed memory of over-provisioned (MB)
total_memory_cluster <- 100 * 1024  # Total cluster memory (convert GB to MB)

# Read all CSV files ending with '2' or '3' in the directory
files <- list.files(pattern = ".*[23]\\.csv$")

# Initialize an empty data frame to store memory utilization over time
memory_data <- data.frame()

# Initialize a vector to store ignored file names
ignored_files <- c()

for (file in files) {
  # Read only the first two columns, ignoring the rest, with headers
  df <- read.csv(file, header = TRUE, stringsAsFactors = FALSE)[, 1:2]
  
  # Rename columns for clarity (assuming first column is RemoteMemory, second is LocalMemory)
  colnames(df) <- c("RemoteMemory", "LocalMemory")
  
  # Convert columns to numeric, coercing errors to NA
  df$RemoteMemory <- as.numeric(df$RemoteMemory)
  df$LocalMemory <- as.numeric(df$LocalMemory)
  
  # Handle potential NA values (replace with 0)
  df[is.na(df)] <- 0
  
  # Check the last row to determine if it's an over-provisioned tenant
  last_row <- tail(df, 1)
  if (last_row$LocalMemory == 2170.52 || last_row$LocalMemory == 2165.52) {
    ignored_files <- c(ignored_files, file)  # Store filename for printing later
    next  # Skip this file (do not include it in memory_data)
  }
  
  # Calculate total allocated memory at each time step
  df$TotalMemory <- df$RemoteMemory + df$LocalMemory
  
  # Add a time column
  df$Time <- 1:nrow(df)
  
  # Store results using bind_rows
  memory_data <- bind_rows(memory_data, df)
}}


# Print ignored files
if (length(ignored_files) > 0) {
  print("Ignored files (over-provisioned tenants):")
  print(ignored_files)
} else {
  print("No over-provisioned tenants were ignored.")
}


# Aggregate memory utilization over all under-provisioned tenants
memory_summary <- memory_data %>%
  group_by(Time) %>%
  summarise(TotalUtilizedMemory = sum(TotalMemory, na.rm = TRUE) + fixed_over_provisioned_usage)

# Convert memory usage to percentage of total cluster memory
memory_summary$UtilizationPercentage <- (memory_summary$TotalUtilizedMemory / total_memory_cluster) * 100

# Ensure x-axis ends at 35,000 seconds
memory_summary <- memory_summary %>% filter(Time <= 20000)

# Get the last recorded value for annotation
last_time <- tail(memory_summary$Time, 1)
last_utilization <- tail(memory_summary$UtilizationPercentage, 1)

# Create the plot with better aesthetics and annotation
plot <- ggplot(memory_summary, aes(x = Time, y = UtilizationPercentage)) +
  geom_line(color = "blue", size = 1.2) +
  geom_text(aes(x = last_time, y = last_utilization, 
                label = paste0(round(last_utilization, 1), "%")), 
            hjust = 0.5, vjust = 1.5, size = 5, fontface = "bold", color = "black") +
  labs(
    title = "",
    x = "Time (seconds)",
    y = "Memory Utilization (%)"
  ) +
  scale_x_continuous(limits = c(0, 20000), breaks = seq(0, 20000, by = 5000)) +
  scale_y_continuous(limits = c(0, 100), breaks = seq(0, 100, by = 10)) +
  theme_minimal(base_size = 14) +
  theme(
    plot.title = element_text(hjust = 0.5, face = "bold"),
    axis.title.x = element_text(face = "bold"),
    axis.title.y = element_text(face = "bold"),
    axis.line = element_line(size = 1.2, color = "black"),
    axis.text = element_text(size = 12),
    panel.grid.major = element_line(color = "gray80"),
    panel.grid.minor = element_blank()
  )

# Save the plot as a PDF
ggsave("memory_utilization.pdf", plot, width = 10, height = 6)

# Display message
print("Memory utilization plot saved as 'memory_utilization.pdf'")
