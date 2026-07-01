library(ggplot2) # Memory Differential Plot
library(dplyr)
library(tidyr)
library(RColorBrewer)

folder_path <- "memory"  # Replace path_to_your_folder with the actual folder path

# List all CSV files ending with 2 or 3
file_list <- list.files(path = folder_path, pattern = "memory_stats.*[23]\\.csv$", full.names = TRUE)

# Initialize a data frame to store the highest extra_remote values and Memory Given Away
memory_data <- data.frame(Tenant = character(), Remote = numeric(), MemoryGivenAway = numeric(), stringsAsFactors = FALSE)

tenant_counter <- 1

# Process each file
for (file in file_list) {
  data <- read.csv(file)
  
  # Calculate Remote Memory (assuming the highest 'extra_remote' value is in the last row)
  remote_memory_value <- tail(data$extra_remote, 1)
  
  # Calculate Local Memory gained/lost : the last value of 'total' minus 2001.40 (initial allocation size)
  last_total_value <- tail(data$total, 1)
  local_memory_value <- last_total_value - 2001.40

  # Extract tenant name from file name (removing 'memory_stats_' prefix and '.csv' suffix)
  tenant_name <- gsub("memory_stats_|\\.csv", "", file)
  parts <- strsplit(tenant_name, "_", fixed = TRUE)[[1]]
  ip_parts <- strsplit(parts[1], "\\.", fixed = TRUE)[[1]]
  
  tenant_name <- paste0("T", tenant_counter)
  
  # Increment the counter for the next tenant
  tenant_counter <- tenant_counter + 1
  
  # Extract the last part of the IP and combine it with the port
  short_name <- paste(ip_parts[length(ip_parts)], parts[2], sep = ":")
  final_name <- strsplit(short_name, split = "\\/")[[1]]
  
  #final_name <- tenant_name
  # Add to the data frame
  memory_data <- rbind(memory_data, data.frame(Tenant = final_name[length(final_name)], Remote = remote_memory_value, Local = local_memory_value))
}

# Function to insert a specified number of spacers after a defined number of tenants
insert_spacers <- function(df, tenants_between_spacers = 2, num_spacers = 1) {
  spacer_row <- df[1, , drop = FALSE]  # Copy the structure of a row (taking the first one as a template)
  spacer_row[1,] <- NA  # Set all values to NA
  spacer_row$Tenant <- "Spacer"  # Indicative name
  
  new_df <- df[1, , drop = FALSE]  # Start new dataframe
  new_df <- new_df[0,]  # Empty it but keep the structure
  
  counter <- 0  # To count rows between spacers
  for (i in 1:nrow(df)) {
    new_df <- rbind(new_df, df[i, , drop = FALSE])  # Add the actual data row
    counter <- counter + 1
    if (counter == tenants_between_spacers && i != nrow(df)) {  # Check if the defined number of rows have been added and it's not the last row
      for (j in 1:num_spacers) {
        spacer_id <- paste("Spacer", i, j, sep = "_")  # Create a unique identifier for each spacer
        spacer_row$Tenant <- spacer_id
        new_df <- rbind(new_df, spacer_row)  # Add spacer row
      }
      counter = 0  # Reset counter after adding spacers
    }
  }
  return(new_df)
}

# Example usage: Insert 2 spacers after every 3 tenants
memory_data_with_spacers <- insert_spacers(memory_data, tenants_between_spacers = 2, num_spacers = 2)

# Ensure that the data is ordered properly, if 'Tenant' is a factor make sure to reorder it
memory_data_with_spacers$Tenant <- factor(memory_data_with_spacers$Tenant, levels = unique(memory_data_with_spacers$Tenant))

# Now, continue with your usual plotting code
memory_data_long <- memory_data_with_spacers %>%
  pivot_longer(cols = c("Remote", "Local"), names_to = "MemoryType2", values_to = "Value") %>%
  mutate(MemoryCategory = "", Value = replace_na(Value, 0))

# Create a new factor indicating positive or negative values combined with MemoryType
memory_data_long <- memory_data_long %>%
  mutate(Sign = ifelse(Value >= 0, "allocated", "released"),
         MemoryType = paste(MemoryType2, Sign, sep = " - "))  # Combine MemoryType and Sign

# Define colors for each combination
colors <- c("Remote - allocated" = "steelblue", 
            "Local - allocated" = "lightblue", 
            "Remote - released" = "magenta",
            "Local - released" = "tomato")

p <- ggplot(memory_data_long, aes(x = Tenant, y = Value, fill = MemoryType)) +
  geom_bar(stat = "identity", position = "stack") +
  facet_grid(MemoryCategory ~ ., scales = "free_y", space = "free") +
  scale_fill_manual(values = colors) +
  theme_minimal() +
  scale_y_continuous(breaks = seq(-5000, 5000, by = 500)) +
  labs(title = "Memory Differential for Each Tenant", x = "Tenant (Number)", y = "Memory (MB)") +
  theme(axis.text.x = element_text(angle = 45, hjust = 1, size = 20, face = "bold"),
        axis.text.y = element_text(size = 20, face = "bold"),
        axis.title.x = element_text(size = 25, face = "bold", vjust = -0.5),  # Bold and larger X-axis label
        axis.title.y = element_text(size = 25, face = "bold", vjust = 0.5),  # Bold and larger Y-axis label
        plot.title = element_text(hjust = 0.5, size = 23, face = "bold"),
        legend.key.size = unit(1.5, "cm"),  # Enlarge the legend keys
        legend.text = element_text(size = 22, face = "bold"),  # Increase the text size
        legend.title = element_text(size = 24, face = "bold")) +  # Increase and bold the legend title  
  scale_x_discrete(labels = function(x) ifelse(grepl("Spacer", x), "", x))  # Replace spacer labels with empty strings

#p <- p + coord_flip()

# Save the plot to a PDF file in the same directory
ggsave("remote_memory_chart222.pdf", plot = p, path = folder_path, device = "pdf", width = 30, height = 15)

# Print the plot to RStudio viewer (optional)
print(p)

sum_remote_gained <- sum(memory_data$Remote[memory_data$Remote > 0])
print(sum_remote_gained)

sum_local_gained <- sum(memory_data$Local[memory_data$Local > 0])
print(sum_local_gained)

print(sum_remote_gained+sum_local_gained)

sum_released <- sum(memory_data$Local[memory_data$Local < 0])
print(sum_released)
