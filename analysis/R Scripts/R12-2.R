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

# ---- Compute total differential ----
memory_data <- memory_data %>%
  mutate(
    Total = Local + Remote
  ) %>%
  arrange(Total) %>%
  mutate(
    TenantIndex = row_number()
  )

zero_index <- min(memory_data$TenantIndex[memory_data$Total > 0])

memory_data_long <- memory_data %>%
  pivot_longer(cols = c("Remote", "Local"),
               names_to = "MemoryType",
               values_to = "Value") %>%
  filter(Value != 0)  # Remove pure zero rows

colors <- c(
  "Remote" = "steelblue",
  "Local"  = "lightblue"
)

p_sorted <- ggplot() +
  
  # Stacked bars
  geom_bar(data = memory_data_long,
           aes(x = TenantIndex,
               y = Value,
               fill = MemoryType),
           stat = "identity",
           width = 0.9) +
  
  # Total differential line
  geom_line(data = memory_data,
            aes(x = TenantIndex,
                y = Total),
            color = "black",
            size = 1.2) +
  
  geom_hline(yintercept = 0,
             linetype = "dashed",
             size = 1) +
  
  geom_vline(xintercept = zero_index, 
             linetype = "dotted", 
             size = 1) +
  
  scale_fill_manual(values = c(
    "Remote" = "steelblue",
    "Local"  = "lightblue"
  )) +
  
  labs(
    title = "", #"Cluster-wide Memory Redistribution Profile",
    x = "Tenants",
    y = "Memory Differential (MB)",
    fill = "Memory Type"
  ) +
  
  theme_minimal() +
  theme(
    axis.text.x = element_blank(),
    axis.ticks.x = element_blank(),
    axis.text.y = element_text(size = 18, face = "bold"),
    axis.title = element_text(size = 24, face = "bold"),
    plot.title = element_text(hjust = 0.5, size = 24, face = "bold"),
    legend.text = element_text(size = 18, face = "bold"),
    legend.title = element_text(size = 20, face = "bold"),
    legend.position = "bottom",
    legend.direction = "horizontal",
    legend.key.size = unit(1.3, "cm"),
    plot.margin = margin(20, 20, 20, 20)
  ) +
  
  annotate("rect",
           xmin = 0,
           xmax = zero_index,
           ymin = -Inf,
           ymax = Inf,
           alpha = 0.05,
           fill = "orange") +
  
  annotate("text",
           x = zero_index / 2,
           y = max(memory_data$Total) * 0.8,
           label = "Victims\n(Donors)",
           size = 7,
           fontface = "bold",
           color = "darkorange") +
  
  annotate("text",
           x = zero_index + (max(memory_data$TenantIndex) - zero_index)/2,
           y = min(memory_data$Total) * 0.8,
           label = "Victors\n(Receivers)",
           size = 7,
           fontface = "bold",
           color = "steelblue4") +
  
  annotate("text",
           x = zero_index + 30,
           y = max(memory_data$Total) * 0.95,
           label = paste0("Total Remote Allocated: ",
                          round(sum_remote_gained/1024, 1),
                          " GB"),
           hjust = 0,
           size = 6) +

  annotate("text",
           x = 30,
           y = min(memory_data$Total) * 0.9,
           label = paste0("Total Donated: ",
                          round(abs(sum_released)/1024, 1),
                          " GB"),
           hjust = 0,
           size = 6)

ggsave("memory_sorted_profile.pdf",
       plot = p_sorted,
       path = folder_path,
       width = 18,
       height = 8)


# CDF PLOT

memory_data_cdf <- memory_data %>%
  arrange(Total) %>%
  mutate(CDF = row_number() / n())

p_cdf <- ggplot(memory_data_cdf,
                aes(x = Total, y = CDF)) +
  geom_line(size = 1.5, color = "darkblue") +
  geom_vline(xintercept = 0, linetype = "dashed") +
  labs(
    title = "CDF of Tenant Memory Differential",
    x = "Memory Differential (MB)",
    y = "Cumulative Fraction of Tenants"
  ) +
  theme_minimal() +
  theme(
    axis.text = element_text(size = 18, face = "bold"),
    axis.title = element_text(size = 22, face = "bold"),
    plot.title = element_text(hjust = 0.5, size = 24, face = "bold")
  )

ggsave("memory_cdf.pdf",
       plot = p_cdf,
       path = folder_path,
       width = 12,
       height = 8)

print(p)

sum_remote_gained <- sum(memory_data$Remote[memory_data$Remote > 0])
print(sum_remote_gained)

sum_local_gained <- sum(memory_data$Local[memory_data$Local > 0])
print(sum_local_gained)

print(sum_remote_gained+sum_local_gained)

sum_released <- sum(memory_data$Local[memory_data$Local < 0])
print(sum_released)
