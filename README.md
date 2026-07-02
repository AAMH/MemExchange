# MemExchange

MemExchange is a distributed memory management system for multi-tenant cloud caching environments. It enables cluster-wide memory trading using RDMA, allowing memory-constrained cache tenants to transparently use unused memory on remote servers while preserving low request latency and tenant isolation.

The system extends Memcached with distributed memory management, RDMA-backed remote access, tracker-based coordination, and automated benchmarking infrastructure. MemExchange was developed as part of my PhD research on cloud-scale memory management.

## Key Features

- Cluster-wide memory trading across physical servers
- RDMA-backed remote memory access
- Application-layer remote memory management
- Memcached integration
- Tracker-based local and remote memory coordination
- Greedy tenant mode for elastic cache resizing
- Support for enabling/disabling cluster-wide trading with `MTC_ON` / `MTC_OFF`
- Automation scripts for CloudLab experiments
- Benchmarking support for CloudSuite and mutilate
- R scripts for analysis, plots, and statistics

## Repository Structure

```text
MemExchange/
├── src/                 # Modified Memcached / MemExchange source code
├── tracker/             # Tracker and shared-memory management code
├── scripts/             # CloudLab and experiment automation scripts
├── analysis/            # R scripts for plots, figures, and statistics
├── benchmarks/          # Links and notes for benchmark repositories
├── docs/                # FAQ and additional documentation
└── README.md
```

## Build

Install dependencies:

```bash
sudo apt update
sudo apt install -y \
    autotools-dev \
    autoconf \
    libevent-dev \
    librdmacm-dev \
    ibverbs-utils \
    scons \
    gengetopt \
    screen
```

If running on a node without hardware RDMA support and using Soft-RoCE/RXE, also install:

```bash
sudo apt install -y rdma-core
```

Build MemExchange:

```bash
./autogen
./configure CFLAGS="-w"
make
```

Clean build artifacts:

```bash
make clean
```

## Tracker

The tracker initializes the shared-memory region and coordinates memory trading between tenants. It must be started before running MemExchange tenants.

Compile the tracker binaries:

```bash
gcc -g -o tracker start_tracker.c shm_malloc.c -lrt -pthread
gcc -g -o stop_tracker stop_tracker.c shm_malloc.c -lrt -pthread
```

Start the tracker:

```bash
./tracker 32000 0.25 0.25 0.25 0.25 MTC_ON
```

Usage:

```text
./tracker <shared_memory_MB> [tenant_share_1 tenant_share_2 tenant_share_3 tenant_share_4] <MTC_ON|MTC_OFF>
```

Example:

```bash
./tracker 32000 0.25 0.25 0.25 0.25 MTC_ON
```

`MTC_ON` enables cluster-wide memory trading. Tenants may use remote memory, and the node may lend unused memory to other machines.

`MTC_OFF` disables cluster-wide memory trading. Tenants are limited to local memory trading only and do not participate in remote memory exchange.

After tenants finish, clean up the shared-memory region:

```bash
./stop_tracker
```

## Running MemExchange Tenants

Example tenant:

```bash
./memcached -v -p 11212 -t 4 -m 4096 -G
```

The `-G` flag enables greedy mode, allowing a tenant to grow beyond its initial memory allocation when additional memory becomes available through MemExchange.

## Benchmarks

MemExchange was evaluated using modified versions of:

- CloudSuite
- mutilate
- InfiniSwap, used as a comparison system

These are maintained as separate repositories. See `benchmarks/README.md` for links and notes.

## Experiment Scripts

The `scripts/` directory contains automation for CloudLab deployment, benchmark execution, RPS sweeps, and monitoring. See `scripts/README.md`.

## Analysis

The `analysis/` directory contains R code used to generate plots, figures, and statistics for the paper/thesis. See `analysis/README.md`.

## Debugging

AddressSanitizer is useful for debugging memory corruption.

Example flags:

```makefile
CFLAGS += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
```

For undefined behavior checks:

```makefile
CFLAGS += -fsanitize=undefined
LDFLAGS += -fsanitize=undefined
```

## Soft-RoCE / RXE

On machines without hardware RDMA NICs, MemExchange can be tested using Soft-RoCE/RXE.

Example:

```bash
echo "mlx5_core.rdma.2" | sudo tee /sys/bus/auxiliary/drivers/mlx5_ib.rdma/unbind
sudo rdma link add rxe_0 type rxe netdev enp65s0f0np0
rdma link
```

Adjust the network interface name for your machine.

## Publication

MemExchange was developed as part of the paper:

**MemExchange: Cloud-Scale Memory Trading**

Citation and arXiv link will be added once available.

## Notes

This repository contains research software. Some scripts assume CloudLab-specific paths, usernames, IP ranges, and experiment layouts. These are documented to make the evaluation workflow transparent, but may require modification for other environments.

## Additional Documentation

- **FAQ:** `docs/faq.md`
- **Benchmarking:** `benchmarks/README.md`
- **Experiment Scripts:** `scripts/README.md`
- **Analysis & Figures:** `analysis/README.md`
