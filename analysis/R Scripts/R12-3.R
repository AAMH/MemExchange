library(tidyverse)
library(patchwork)
library(ggplot2)

# Folder
folder_path <- "hitRatio"

# Find relevant files
file_list <- list.files(
  path = folder_path,
  pattern = ".*[23]\\.csv$",
  full.names = TRUE
)

# Initialize list
tenant_list <- list()

tenant_id <- 1

for (file in file_list) {
  
  data <- read.csv(file)
  
  # Skip empty files (only header)
  if (nrow(data) == 0) next
  
  # Compute instantaneous hit rate
  data <- data %>%
    mutate(
      time = row_number(),
      inst_hit_rate = ifelse(last_sec_total_req > 0,
                             100 * last_sec_hits / last_sec_total_req,
                             NA),
      overall_hit_rate = overall_hit_rate,
      tenant = paste0("T", tenant_id)
    )
  
  tenant_list[[tenant_id]] <- data
  tenant_id <- tenant_id + 1
}

# Combine all tenants
hit_data <- bind_rows(tenant_list)

# Keep only relevant columns
hit_data <- hit_data %>%
  select(tenant, time, inst_hit_rate, overall_hit_rate)


hit_summary_inst <- hit_data %>%
  group_by(time) %>%
  summarise(
    median = median(inst_hit_rate, na.rm = TRUE),
    p10 = quantile(inst_hit_rate, 0.10, na.rm = TRUE),
    p90 = quantile(inst_hit_rate, 0.90, na.rm = TRUE)
  )

hit_summary_overall <- hit_data %>%
  group_by(time) %>%
  summarise(
    median = median(overall_hit_rate, na.rm = TRUE),
    p10 = quantile(overall_hit_rate, 0.10, na.rm = TRUE),
    p90 = quantile(overall_hit_rate, 0.90, na.rm = TRUE)
  )


p_inst <- ggplot(hit_summary_inst, aes(x = time)) +
  geom_ribbon(aes(ymin = p10, ymax = p90),
              fill = "steelblue",
              alpha = 0.25) +
  geom_line(aes(y = median),
            color = "steelblue4",
            linewidth = 1.2) +
  coord_cartesian(xlim = c(0, 35000), ylim = c(0, 100)) +
  scale_y_continuous(breaks = seq(0, 100, 10)) +
  scale_x_continuous(limits = c(0, 35000), expand = c(0,0)) +
  labs(
    x = "Time (s)",
    y = "Per-Second Hit Rate (%)"
  ) +
  theme_minimal() +
  theme(
    axis.text = element_text(size = 14, face = "bold"),
    axis.title = element_text(size = 16, face = "bold")
  )

ggsave(
  filename = "figX_a_instantaneous_hit_rate.pdf",
  plot = p_inst,
  width = 8,
  height = 5
)

p_overall <- ggplot(hit_summary_overall, aes(x = time)) +
  geom_ribbon(aes(ymin = p10, ymax = p90),
              fill = "darkgreen",
              alpha = 0.25) +
  geom_line(aes(y = median),
            color = "darkgreen",
            linewidth = 1.2) +
  coord_cartesian(xlim = c(0, 35000), ylim = c(0, 100)) +
  scale_y_continuous(breaks = seq(0, 100, 10)) +
  scale_x_continuous(limits = c(0, 35000), expand = c(0,0)) +
  labs(
    x = "Time (s)",
    y = "Overall Hit Rate (%)"
  ) +
  theme_minimal() +
  theme(
    axis.text = element_text(size = 14, face = "bold"),
    axis.title = element_text(size = 16, face = "bold")
  )

ggsave(
  filename = "figX_b_overall_hit_rate.pdf",
  plot = p_overall,
  width = 8,
  height = 5
)

# Sort tenants by final overall hit rate
tenant_order <- hit_data %>%
  group_by(tenant) %>%
  summarise(
    low_duration = sum(inst_hit_rate < 75, na.rm = TRUE)
  ) %>%
  arrange(desc(low_duration)) %>%
  pull(tenant)

hit_data$tenant <- factor(hit_data$tenant, levels = tenant_order)

p_heatmap <- ggplot(hit_data,
                    aes(x = time,
                        y = tenant,
                        fill = inst_hit_rate)) +
  geom_raster() +
  scale_fill_viridis_c(
    limits = c(0, 100),
    name = "Per-Second Hit Rate (%)",
    option = "C"
  ) +
  guides(
    fill = guide_colorbar(
      title.position = "top",
      title.hjust = 0.5,
      barwidth = 15,
      barheight = 1
    )
  ) +
  labs(
    x = "Time (s)",
    y = "Tenants",
    fill = "Inst. Hit Rate"
  ) +
  theme_minimal() +
  theme(
    axis.text.y = element_blank(),
    axis.ticks.y = element_blank(),
    axis.text.x = element_text(size = 16, face = "bold"),
    axis.title = element_text(size = 18, face = "bold"),
    legend.position = "bottom",
    legend.title = element_text(size = 16, face = "bold"),
    legend.title.align = 0.5,
    legend.text = element_text(size = 14, face = "bold"),
    legend.key.size = unit(1.5, "cm"),
  )

ggsave(
  filename = "figY_hit_rate_heatmap.png",
  plot = p_heatmap,
  width = 8,
  height = 9,
  dpi = 300
)

base_theme <- theme_minimal(base_size = 9) +
  theme(
    axis.text = element_text(size = 8, face = "bold"),
    axis.title = element_text(size = 9, face = "bold"),
    panel.grid.minor = element_blank(),
    panel.grid.major.x = element_blank(),
    plot.margin = margin(2, 4, 2, 2)
  )

# Top plot (no x-axis labels)
p_inst <- ggplot(hit_summary_inst, aes(x = time)) +
  geom_ribbon(aes(ymin = p10, ymax = p90),
              fill = "steelblue",
              alpha = 0.15) +
  geom_line(aes(y = median),
            color = "black",
            linewidth = 0.9) +
  coord_cartesian(xlim = c(0, 35000), ylim = c(0, 100)) +
  scale_y_continuous(breaks = seq(0, 100, 20)) +
  scale_x_continuous(limits = c(0, 35000), expand = c(0,0)) +
  labs(
    x = NULL,
    y = "Per-Second Hit Rate (%)"
  ) +
  base_theme +
  theme(
    axis.text.x = element_blank(),
    axis.ticks.x = element_blank()
  )

# Bottom plot (shared x-axis)
p_overall <- ggplot(hit_summary_overall, aes(x = time)) +
  geom_ribbon(aes(ymin = p10, ymax = p90),
              fill = "darkgreen",
              alpha = 0.15) +
  geom_line(aes(y = median),
            color = "black",
            linewidth = 0.9) +
  coord_cartesian(xlim = c(0, 35000), ylim = c(0, 100)) +
  scale_y_continuous(breaks = seq(0, 100, 20)) +
  scale_x_continuous(limits = c(0, 35000), expand = c(0,0)) +
  labs(
    x = "Time (s)",
    y = "Overall Hit Rate (%)"
  ) +
  base_theme

combined_plot <- p_inst / p_overall

ggsave(
  filename = "figX_combined_hit_rate.pdf",
  plot = combined_plot,
  width = 3.4,   # single-column width
  height = 6.2   # compact height
)