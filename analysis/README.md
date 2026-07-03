# Analysis

This directory contains the R scripts used to process experiment logs, generate
figures, and compute summary statistics for the MemExchange paper and related
dissertation work.

The scripts are reproducibility artifacts: they reflect the log layout, file
names, and experiment structure used during the evaluation.

## What the Scripts Cover

- Paper and dissertation figure generation.
- Latency and throughput analysis.
- Cache hit-rate and miss-rate analysis.
- Cluster memory-utilization analysis.
- Statistical summaries and significance tests.
- Auxiliary processing for raw benchmark logs.

## Workflow

```text
Run experiments
        |
Collect benchmark logs
        |
Process raw logs
        |
Run R scripts
        |
Generate figures and statistics
```

## Requirements

Most scripts require R 4.x or newer and common packages such as:

- `ggplot2`
- `dplyr`
- `tidyr`
- `readr`
- `reshape2`
- `gridExtra`

Additional package dependencies, when needed, are documented near the beginning
of the individual scripts.

## Notes

Some scripts expect input files produced by the automation in `scripts/` or by
the CloudLab workflow used for the paper. If you organize logs differently,
update the input paths before running the analysis.
