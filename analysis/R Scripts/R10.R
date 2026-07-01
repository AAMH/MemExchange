library(ggplot2) # Hit Ratio plots for each tenant & Grid
library(tools)
library(gridExtra) # For arranging multiple plots
library(patchwork)

# Set your directory path
path_to_files <- "hitRatio"

# List all CSV files
files <- list.files(path_to_files, pattern = "hit_stats_.*\\.csv$", full.names = TRUE)

# Process each file to extract data and metadata
data_list <- list()
for (file_path in files) {
  data <- read.csv(file_path, header = TRUE)
  
  # Check if the data frame is empty (has no rows)
  if (nrow(data) == 0) {
    next # Skip this iteration and move to the next file
  }
  
  filename <- basename(file_path)
  tenant_info <- gsub("hit_stats_|\\.csv", "", filename)
  parts <- strsplit(tenant_info, "_")[[1]]
  ip_address <- parts[1]
  port_number <- parts[2]
  port_ending <- substring(port_number, nchar(port_number))
  data_list[[filename]] <- list(data = data, ip_address = ip_address, port_number = port_number, port_ending = port_ending)
}

# Define plot function to handle both overall and instantaneous hit rates
plot_data <- function(data1, data2, ratio_type, title_suffix, ip, port_ending_group) {
  # Determine which ratio to plot
  hit_ratio_column <- ifelse(ratio_type == "overall", "overall_hit_rate", "instantaneous_hit_rate")
  
  # Combine data for plotting
  combined_data <- rbind(data.frame(time = 1:nrow(data1), hit_ratio = data1[[hit_ratio_column]], type = "MemExchange"),
                         data.frame(time = 1:nrow(data2), hit_ratio = data2[[hit_ratio_column]], type = "Base_Memcached"))
  
  # Generate the plot
  ggplot(combined_data, aes(x = time, y = hit_ratio, color = type)) +
    geom_line() +
    labs(title = paste(ip,":", paste(port_ending_group, collapse = " & ")),
         x = "Time (seconds)",
         y = title_suffix,
         color = "Tenant Type") +
    theme_minimal() +
    theme(legend.position = "none", 
          axis.text.x = element_text(size = 15, face = "bold"),
          axis.text.y = element_text(size = 15, face = "bold"),
          axis.title.x = element_text(size = 20, face = "bold"),  # Bold and larger X-axis label
          axis.title.y = element_text(size = 20, face = "bold"),  # Bold and larger Y-axis label
          plot.title = element_text(size = 20, face = "bold"))
}

plot_list_overall <- list()
plot_list_instant <- list()
i <- 1

# Loop through IPs and port ending groups to generate and save plots
for (ip in unique(sapply(data_list, function(x) x$ip_address))) {
  for (port_ending_group in list(c("2", "4"), c("3", "5"))) {
    port_group_data <- Filter(function(x) x$ip_address == ip && x$port_ending %in% port_ending_group, data_list)
    
    # Ensure we have two datasets to combine
    if (length(port_group_data) == 2) {
      names <- names(port_group_data)
      
      # Calculate Instantaneous Hit Rate for both datasets
      for (name in names) {
        port_group_data[[name]]$data$instantaneous_hit_rate <- with(port_group_data[[name]]$data, last_sec_hits / last_sec_total_req * 100)
      }
      
      # Generate and save the Overall Hit Rate plot
      plot_overall <- plot_data(port_group_data[[names[1]]]$data, port_group_data[[names[2]]]$data, "overall", "Overall Hit Rate (%)", ip, port_ending_group)
      ggsave(paste0(path_to_files, "/", ip, "_Overall_Hit_Ratio_Group_", paste(port_ending_group, collapse = "_"), ".pdf"), plot_overall, device = "pdf", width = 10, height = 6)
      plot_list_overall[[i]] <- plot_overall
      
      # Generate and save the Instantaneous Hit Rate plot
      plot_instantaneous <- plot_data(port_group_data[[names[1]]]$data, port_group_data[[names[2]]]$data, "instantaneous", "Inst. Hit Rate (%)", ip, port_ending_group)
      ggsave(paste0(path_to_files, "/", ip, "_Instantaneous_Hit_Raio_Group_", paste(port_ending_group, collapse = "_"), ".pdf"), plot_instantaneous, device = "pdf", width = 10, height = 6)
      plot_list_instant[[i]] <- plot_instantaneous
      i <- i + 1
    }
  }
}

plot_list_overall[[50]] <- plot_list_overall[[50]] + theme(legend.position = "right", legend.key.size = unit(3, "cm"),  # Enlarge the legend keys
                                                           legend.text = element_text(size = 30),  # Increase the text size
                                                           legend.title = element_text(size = 34, face = "bold", hjust = 0.25)) + guides(color = guide_legend(override.aes = list(linewidth = 15, linelength = 5)))
plot_list_instant[[50]] <- plot_list_instant[[50]] + theme(legend.position = "right", legend.key.size = unit(3, "cm"),  # Enlarge the legend keys
                                                           legend.text = element_text(size = 30),  # Increase the text size
                                                           legend.title = element_text(size = 34, face = "bold", hjust = 0.25)) + guides(color = guide_legend(override.aes = list(linewidth = 15, linelength = 5)))
         

combined_plot_overall <- wrap_plots(plot_list_overall, ncol = 10)
combined_plot_instant <- wrap_plots(plot_list_instant, ncol = 10)

# Save the combined plot to a file
ggsave("combined_plots_overall.png", plot = combined_plot_overall, width = 40, height = 40)
ggsave("combined_plots_instant.png", plot = combined_plot_instant, width = 40, height = 40)



plot_list_mixed <- list()

plot_list_mixed[[1]] <- plot_list_overall[[1]]
plot_list_mixed[[2]] <- plot_list_overall[[4]]
plot_list_mixed[[3]] <- plot_list_overall[[13]]
plot_list_mixed[[4]] <- plot_list_overall[[15]]
plot_list_mixed[[5]] <- plot_list_overall[[71]]
plot_list_mixed[[6]] <- plot_list_overall[[94]]

plot_list_mixed[[7]] <- plot_list_instant[[1]]
plot_list_mixed[[8]] <- plot_list_instant[[4]]
plot_list_mixed[[9]] <- plot_list_instant[[13]]
plot_list_mixed[[10]] <- plot_list_instant[[15]]
plot_list_mixed[[11]] <- plot_list_instant[[71]]
plot_list_mixed[[12]] <- plot_list_instant[[94]]


#plot_list_mixed[[6]] <- plot_list_mixed[[6]] + theme(legend.position = "right", legend.key.size = unit(3, "cm"),  # Enlarge the legend keys
#                                                           legend.text = element_text(size = 20),  # Increase the text size
#                                                           legend.title = element_text(size = 24, face = "bold"))   # Increase and bold the legend title

combined_plot_mixed <- wrap_plots(plot_list_mixed, ncol = 6)


combined_plot_mixed <- combined_plot_mixed + 
  plot_annotation(
    title = "Hit Ratio Over Time - 6 Random Tenants",
    theme = theme(
      plot.title = element_text(size = 30, hjust = 0.5, face = "bold")  # Adjust size and center title
    )
  )

# Save the combined plot to a file
ggsave("combined_plots_mixed.png", plot = combined_plot_mixed, width = 25, height = 8)
