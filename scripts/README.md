# Experiment Scripts

This directory contains the automation used to deploy MemExchange experiments,
run benchmark workloads, and collect auxiliary statistics for the paper.

These scripts are intentionally close to the CloudLab setup used during the
evaluation. Before reusing them in another environment, review paths, usernames,
IP addresses, interface names, and node assignments.

## Scripts

- `setup_infiniswap_cluster.sh`: configures the InfiniSwap comparison setup
  across CloudLab nodes.
- `roles.cfg`: assigns node roles for InfiniSwap deployment.
- `run_cloudsuite.sh`: runs CloudSuite against MemExchange tenants.
- `run_cloudsuite_inf.sh`: runs CloudSuite against the InfiniSwap setup.
- `run_mutilate.sh`: runs mutilate against MemExchange tenants.
- `run_mutilate_inf.sh`: runs mutilate against the InfiniSwap setup.
- `rps_sweep.sh`: runs CloudSuite across a range of request rates.
- `mtc_stats_logger.sh`: logs MTC-related network and RDMA statistics over
  time.

## Typical Workflow

```text
Prepare CloudLab nodes and RDMA interfaces
        |
Start trackers and cache tenants
        |
Run CloudSuite or mutilate clients
        |
Collect benchmark logs and MTC statistics
        |
Analyze results with scripts in analysis/
```

## CloudLab Assumptions

Many scripts assume the experiment layout used in the paper, including:

- CloudLab RDMA/private IPs in the `10.10.1.x` range.
- Project paths under `/users/AMH/mydata`.
- Benchmark repositories stored under `/users/AMH/mydata/benchmarks`.
- `screen` sessions for launching tenants and benchmark clients.

If you use a different cluster, update these assumptions before running the
scripts.
