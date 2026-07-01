library(ggplot2) # ETC Experiment - Latency Bar Charts
library(reshape2)

# Read CSV file
df <- read.csv("read.csv")

# Convert the data from wide to long format
df_long <- melt(df, id.vars = "Tenant", variable.name = "Scheme_Metric", value.name = "Value")

# Extract Metric and Scheme information, assuming metric comes first in column names
df_long$Metric <- sub(".*_", "", df_long$Scheme_Metric)
df_long$Scheme <- sub("_.*", "", df_long$Scheme_Metric)

# Ensure the order of the schemes follows their appearance in the dataset
df_long$Scheme <- factor(df_long$Scheme, levels = c("MemExchange", "MemSweeper", "Memcached"))

df_long$Value <- df_long$Value / 1000  # Conversion step

# Define y-axis limits for each metric
y_axis_limits <- list(
  "avg" = c(0.030, 0.060),     # Change these limits as needed
  "min" = c(0.0150, 0.020),
  "99th" = c(0.050, 0.150)
)

# Generate and save a plot for each metric
metrics <- unique(df_long$Metric)

for (metric in metrics) {
  df_metric <- subset(df_long, Metric == metric)
  
  p <- ggplot(df_metric, aes(x = Tenant, y = Value, fill = Scheme)) +
    geom_bar(stat = "identity", position = position_dodge(width = 0.8), width = 0.6) +  # Adds spacing
    labs( y = "Latency (ms)", x = "Tenant") +
    scale_fill_manual(values = c("MemExchange" = "#1b9e77", "MemSweeper" = "#d95f02", "Memcached" = "#7570b3")) +  # Better colors
    theme_minimal() +
    theme(axis.text.x = element_text(angle = 45, hjust = 1, size = 10),
          axis.text.y = element_text(size = 12),
          axis.title = element_text(size = 16, face = "bold"),  # Larger & bold axis labels with spacing
          plot.title = element_text(size = 14, face = "bold"),
          legend.text = element_text(size = 14),
          legend.title = element_text(size = 16, face = "bold")) +
    coord_cartesian(ylim = y_axis_limits[[metric]])  # Apply metric-specific y-axis limits
  
  # Save each plot as a PDF
  pdf_filename <- paste0("Latency_Comparison_", metric, ".pdf")
  ggsave(pdf_filename, plot = p, width = 12, height = 5)  # Slightly wider for better readability
}
