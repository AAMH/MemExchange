# Experiment Scripts

This directory contains scripts used to automate MemExchange and comparison-system experiments.

## Suggested Layout

```text
scripts/
├── cloudlab/
│   ├── setup_infiniswap_cluster.sh
│   └── roles.cfg
└── experiments/
    ├── run_cloudsuite.sh
    ├── run_cloudsuite_inf.sh
    ├── run_mutilate.sh
    ├── run_mutilate_inf.sh
    ├── rps_sweep.sh
    └── mtc_stats_logger.sh
```

## CloudLab Scripts

`cloudlab/` contains setup scripts for cluster-based experiments.

- `setup_infiniswap_cluster.sh` configures the InfiniSwap comparison setup across CloudLab nodes.
- `roles.cfg` assigns node roles for the InfiniSwap deployment.

## Experiment Scripts

`experiments/` contains scripts for running MemExchange and comparison benchmarks.

- `run_cloudsuite.sh` runs CloudSuite against MemExchange tenants.
- `run_cloudsuite_inf.sh` runs CloudSuite against the InfiniSwap setup.
- `run_mutilate.sh` runs mutilate against MemExchange tenants.
- `run_mutilate_inf.sh` runs mutilate against the InfiniSwap setup.
- `rps_sweep.sh` runs CloudSuite across a range of request rates.
- `mtc_stats_logger.sh` logs MTC-related network and RDMA statistics over time.

## Notes

Many scripts assume the CloudLab experiment layout used in the paper, including:

- CloudLab RDMA/private IPs in the `10.10.1.x` range
- project paths under `/users/AMH/mydata`
- benchmark repositories stored under `/users/AMH/mydata/benchmarks`
- `screen` sessions for launching tenants and benchmark clients

Before running these scripts on a new environment, update paths, usernames, network interfaces, and node assignments as needed.
