library(magick)

# Define file paths (Update with your actual file paths)
pdf1_path <- "memory_plots/memexchange_memory_usage.pdf"
pdf2_path <- "memory_plots/memsweeper_memory_usage.pdf"
output_pdf <- "memory_plots/ETC - memory_usage - both.pdf"

# Read the PDF figures (Extract the first page as images)
img1 <- image_read_pdf(pdf1_path, density = 300, pages = 1)
img2 <- image_read_pdf(pdf2_path, density = 300, pages = 1)

# Ensure images have the same height
height <- min(image_info(img1)$height, image_info(img2)$height)
img1 <- image_resize(img1, paste0("x", height))
img2 <- image_resize(img2, paste0("x", height))

# Combine images side by side
combined_img <- image_append(c(img1, img2))

# Save as a new PDF
image_write(combined_img, path = output_pdf, format = "pdf")

cat("Merged PDF saved as:", output_pdf, "\n")
