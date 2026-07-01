library(ggplot2)
library(readr)

# Initialize counters for each category
counters  <- setNames(c(0, 0), c("Base_memcached", "MemSweeper"))
counters2 <- setNames(c(0, 0), c("Base_memcached", "MemSweeper"))

# Specify the folder containing the CSV files
folder_path <- "/Users/amh/Documents/Log/hitRatio"  # Update this to your folder path

# List all CSV files in the specified folder
files <- list.files(path = folder_path, pattern = "\\.csv$", full.names = TRUE)

# Process each file
for (file in files) {
  # Extract just the file name from the path
  file_name <- basename(file)
  
  # Determine the category based on the file name
  category <- ifelse(grepl("[23]\\.csv$", file_name), "MemSweeper", "Base_memcached")
  
  # Read the CSV file (with headers)
  data <- read_csv(file, col_names = TRUE, col_types = cols(
    column1 = col_double(),
    column2 = col_double(),
    column3 = col_double()
  ))
  
  # Check if the data frame is empty
  if (nrow(data) == 0) {
    #cat("-Skipping empty file.", "\n")
    next
  }
  
  # Check if any row satisfies the condition where 2nd column / 1st column = 1
  # Set condition_met to TRUE if any such row exists, FALSE otherwise
  condition_met <- any(data[[2]] / ifelse(data[[1]] <= 25000, NA, data[[1]]) >= 1, na.rm = TRUE)
  
  # If the condition is met in any row, increment the counter for the corresponding category by 1
  if (condition_met) {
    counters[category] <- counters[category] + 1
    cat("Category: ", category, ", filename: ", file_name, "\n")
  }
  else
    counters2[category] <- counters2[category] + 1
  
}

# Create a data frame for plotting
plot_data <- data.frame(Category = names(counters), Count = as.integer(counters))

# Draw the bar chart with adjustments
p <- ggplot(plot_data, aes(x = Category, y = Count, fill = Category)) +
  geom_bar(stat = "identity", width = 0.5) +
  theme_light(base_size = 16) +  # Using theme_light with a larger base font size
  scale_y_continuous(expand = expansion(mult = c(0.1, 0.1))) +  # Adjusting y scale for a larger view
  labs(title = "Number of Tenants reaching at least 100% IHR", x = "Category", y = "Count") +
  theme(plot.title = element_text(size = 20),  # Making the title larger
        axis.title.x = element_text(size = 16),  # Adjusting x-axis title size
        axis.title.y = element_text(size = 16),  # Adjusting y-axis title size
        legend.title = element_text(size = 16),  # Adjusting legend title size
        legend.text = element_text(size = 14))  # Adjusting legend text size

print(p)

# Save the plot, adjust the dimensions as needed for a larger scale
ggsave(filename = "category_counts.pdf", plot = p, path = folder_path, width = 12, height = 8, dpi = 300)

# Print the counters for verification
print(counters)
print(counters2)