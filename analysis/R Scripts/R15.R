# Load necessary libraries
library(dplyr)
library(ggplot2)

# Set the directory containing the CSV files
folder_path <- "hitRatio"

# Get the list of all CSV files in the folder
csv_files <- list.files(path = folder_path, pattern = "*.csv", full.names = TRUE)

# Initialize empty data frames to store all tenants' data
instantaneous_data <- data.frame()
overall_data <- data.frame()

tenant_counter <- 1

# Loop through each file to read and process the data
for (file in csv_files) {
  # Extract the tenant name from the file name
  if (grepl("\\.csv$", file) && grepl("[23]\\.csv$", file)) {
    tenant_name <- gsub("hit_stats_|\\.csv", "", basename(file))
    #tenant_name <- paste0("T", tenant_counter)
    
    # Increment the counter for the next tenant
    tenant_counter <- tenant_counter + 1
    
    # Read the CSV file
    data <- read.csv(file)
    if (nrow(data) > 1){
      # Ensure the necessary columns are present
      if ("last_sec_total_req" %in% colnames(data) & "last_sec_hits" %in% colnames(data)) {
        # Calculate the "Instantaneous Hit Ratio"
        instantaneous_hit_ratio <- data$last_sec_hits / data$last_sec_total_req
        
        # Create a time vector based on the number of rows
        time <- seq_along(instantaneous_hit_ratio)
        
        # Create a data frame with the tenant's "Instantaneous Hit Ratio" data
        tenant_instantaneous_data <- data.frame(Time = time, Instantaneous_Hit_Ratio = instantaneous_hit_ratio, Tenant = tenant_name)
        
        # Append the tenant's data to the overall instantaneous data frame
        instantaneous_data <- rbind(instantaneous_data, tenant_instantaneous_data)
      } else {
        warning(paste("Required columns 'last_sec_total_req' or 'last_sec_hits' not found in", file))
      }
      
      # Check and process the "overall_hit_rate" column
      if ("overall_hit_rate" %in% colnames(data)) {
        # Create a data frame with the tenant's "Overall Hit Rate" data
        tenant_overall_data <- data.frame(Time = time, Overall_Hit_Rate = data$overall_hit_rate, Tenant = tenant_name)
        
        # Append the tenant's data to the overall data frame
        overall_data <- rbind(overall_data, tenant_overall_data)
      } else {
        warning(paste("Column 'overall_hit_rate' not found in", file))
      }
    }
  }
}

# Plot the Instantaneous Hit Ratio using ggplot2
instantaneous_plot <- ggplot(instantaneous_data, aes(x = Time, y = Instantaneous_Hit_Ratio, color = Tenant)) +
  geom_line() +
  labs(title = "Instantaneous Hit Ratio Over Time",
       x = "Time (seconds)",
       y = "Instantaneous Hit Ratio") +
  xlim(0, 5000) +
  theme_minimal()

# Save the Instantaneous Hit Ratio plot to a PDF file
ggsave("instantaneous_hit_ratio_plot.pdf", plot = instantaneous_plot, path = folder_path, width = 30, height = 15, dpi = 300)

# Plot the Overall Hit Rate using ggplot2
overall_plot <- ggplot(overall_data, aes(x = Time, y = Overall_Hit_Rate, color = Tenant)) +
  geom_line(size = 1.5) +
  labs(
       x = "Time (seconds)",
       y = "Overall Hit Rate (%)") +
  theme_minimal() +
  theme(axis.text.x = element_text(size = 17, face = "bold"),
        axis.text.y = element_text(size = 17, face = "bold"),
        axis.title.x = element_text(size = 20, face = "bold"),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 20, face = "bold"),  # Bold and larger Y-axis label
        plot.title = element_text(face = "bold"),
        legend.text = element_text(size = 11, face = "bold"),  # Increase the text size
        legend.title = element_text(size = 16, face = "bold"),
        legend.position = "bottom") +
  guides(color = guide_legend(override.aes = list(linewidth = 5, linelength = 3)))  # Increase legend key size


# Save the Overall Hit Rate plot to a PDF file
ggsave("overall_hit_rate_plott.png", plot = overall_plot, path = folder_path, width = 12, height = 10, dpi = 200)
