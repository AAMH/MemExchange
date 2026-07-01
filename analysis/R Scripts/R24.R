library(dplyr) # calculates RPS based on hit rate logs - used mainly for ETC

# Set the directory containing CSV files
directory <- "hitRatio"

# Get list of all CSV files in the directory
csv_files <- list.files(directory, pattern = "*.csv", full.names = TRUE)

# Initialize a vector to store average RPS values
average_rps_list <- c()

# Loop through each CSV file
for (file in csv_files) {
  # Read the CSV file
  data <- read.csv(file, header = TRUE)
  
  # Ensure there are at least 3600 rows to process
  if (nrow(data) >= 3600) {
    # Select the first column for the first 3600 rows and ignore 0 values
    valid_requests <- data[1:3600, 1]
    valid_requests <- valid_requests[valid_requests > 0]  # Ignore zeros
    
    # Sum the filtered requests
    total_requests <- sum(valid_requests, na.rm = TRUE)
    
    # Count the number of non-zero seconds for averaging
    non_zero_seconds <- length(valid_requests)
    
    # Calculate the average RPS (Requests per Second), considering only non-zero values
    avg_rps <- ifelse(non_zero_seconds > 0, total_requests / non_zero_seconds, 0)
    
    # Store the average RPS for final computation
    if (avg_rps > 0) {
      average_rps_list <- c(average_rps_list, avg_rps)
    }
    
    # Print results
    cat("File:", basename(file), "\n")
    cat("Total requests in first 3600s (excluding 0s):", total_requests, "\n")
    cat("Average RPS in first 3600s (excluding 0s):", avg_rps, "\n\n")
  } else {
    cat("File:", basename(file), "has less than 3600 rows. Skipping...\n\n")
  }
}

# Compute the final average of all individual averages, excluding zero RPS values
if (length(average_rps_list) > 0) {
  final_average_rps <- mean(average_rps_list, na.rm = TRUE)
  cat("Final Average RPS across all files (excluding 0s):", final_average_rps, "\n")
} else {
  cat("No valid files processed. Cannot compute final average.\n")
}
