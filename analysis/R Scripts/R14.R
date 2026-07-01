# Load necessary libraries
library(dplyr)
library(ggplot2)

# Set the directory containing the CSV files
folder_path <- "/Users/amh/Documents/Log/hitRatio"

# Get the list of all CSV files in the folder
csv_files <- list.files(path = folder_path, pattern = "*.csv", full.names = TRUE)

# Initialize an empty list to store the data
data_list <- list()

# Loop through each file, read it, and store the relevant column
for (file in csv_files) {
  # Read the CSV file
  data <- read.csv(file)
  
  # Ensure the column name matches exactly
  if ("overall_hit_rate" %in% colnames(data)) {
    # Store the "overall_hit_rate" column
    data_list[[file]] <- data$overall_hit_rate
  } else {
    warning(paste("Column 'overall_hit_rate' not found in", file))
  }
}

# Combine all the data into a matrix where each column corresponds to a file
combined_data <- do.call(cbind, data_list)

# Calculate the average of "overall_hit_rate" across all files at each second
average_hit_rate <- rowMeans(combined_data, na.rm = TRUE)

# Create a time vector based on the number of rows
time <- seq_along(average_hit_rate)

# Create a data frame for plotting with ggplot2
plot_data <- data.frame(Time = time, Average_Hit_Rate = average_hit_rate)

# Plot using ggplot2
ggplot(plot_data, aes(x = Time, y = Average_Hit_Rate)) +
  geom_line(color = "blue") +
  labs(title = "Average Overall Hit Rate Over Time",
       x = "Time (seconds)",
       y = "Average Overall Hit Rate (%)") +
  theme_minimal() +
  xlim(0, 20000)

first_value <- plot_data$Average_Hit_Rate[1]
last_value <- plot_data$Average_Hit_Rate[20150]

# Print the values in the console
cat("First Average Hit Rate:", first_value, "\n")
cat("Last Average Hit Rate:", last_value, "\n")
