library(ggplot2) # Cloud Experiment 1 - Memory Utilization Plot
library(dplyr)

# Parameters
initial_size <- 2001.40  # Initial memory size of each tenant (MB)
count_idle_as_utilized <- FALSE  # Change to TRUE if idle tenants should be counted as utilized

# Read all CSV files ending with '2' or '3' in the directory
files <- list.files(pattern = ".*[23]\\.csv$")
num_tenants <- length(files)  # Total number of tenants

# Initialize an empty data frame to store memory utilization over time
memory_data <- data.frame()

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
  
  # Calculate total allocated memory at each time step
  df$TotalMemory <- df$RemoteMemory + df$LocalMemory
  
  # Determine if the tenant is over-provisioned based on the last row
  last_row <- tail(df, 1)
  is_over_provisioned <- (last_row$RemoteMemory > 0) | (last_row$LocalMemory > initial_size)
  
  # If the tenant is under-provisioned, decide whether to count its memory as utilized
  if (!is_over_provisioned) {
    if (!count_idle_as_utilized) {
      df$TotalMemory <- 0  # Set memory utilization to zero if not counting idle tenants
    }
  }
  
  # Add a time column
  df$Time <- 1:nrow(df)
  
  # Store results using bind_rows
  memory_data <- bind_rows(memory_data, df)
}

# Aggregate memory utilization over all tenants
memory_summary <- memory_data %>%
  group_by(Time) %>%
  summarise(TotalUtilizedMemory = sum(TotalMemory, na.rm = TRUE))

# Calculate total available memory in MB
total_available_memory <- initial_size * num_tenants

# Convert to percentage utilization
memory_summary$UtilizationPercentage <- (memory_summary$TotalUtilizedMemory / total_available_memory) * 100

# Ensure x-axis ends at 35,000 seconds
memory_summary <- memory_summary %>% filter(Time <= 35000)

# Get the last recorded time and utilization percentage
last_time <- tail(memory_summary$Time, 1)
last_utilization <- tail(memory_summary$UtilizationPercentage, 1)

# Create the plot with annotation
plot <- ggplot(memory_summary, aes(x = Time, y = UtilizationPercentage)) +
  geom_line(color = "blue", size = 1.2) +
  geom_text(aes(x = last_time, y = last_utilization, 
                label = paste0(round(last_utilization, 1), "%")), 
            hjust = 0.5, vjust = -1, size = 5, fontface = "bold", color = "black") +
  labs(
    title = "",
    x = "Time (seconds)",
    y = "Memory Utilization (%)"
  ) +
  scale_x_continuous(limits = c(0, 35000), breaks = seq(0, 35000, by = 5000)) +
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

plot <- plot + coord_cartesian(ylim = c(30, 80))


# Save the plot as a PDF
ggsave("memory_utilization.pdf", plot, width = 10, height = 6)

# Display message
print("Memory utilization plot saved as 'memory_utilization.pdf'")
