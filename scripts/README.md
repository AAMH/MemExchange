# Experiment Scripts

This directory contains scripts used to automate MemExchange and comparison-system experiments.

- `setup_infiniswap_cluster.sh` configures the InfiniSwap comparison setup across CloudLab nodes.
- `roles.cfg` assigns node roles for the InfiniSwap deployment.
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
