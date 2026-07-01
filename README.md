# MemExchange

> **Cluster-wide memory management for multi-tenant cloud caching using RDMA.**

MemExchange is a distributed memory management system that enables **cluster-wide memory trading** between Memcached tenants. Instead of treating memory as a resource confined to individual servers, MemExchange dynamically reallocates idle memory across physical machines, allowing memory-constrained tenants to transparently utilize spare capacity elsewhere in the cluster.

To enable efficient remote accesses, MemExchange introduces the **MemExchange Tracker Communication (MTC)** protocol, an application-layer mechanism that coordinates memory reallocation and enables one-sided RDMA operations without involving remote CPUs. By operating directly at the cache layer, MemExchange avoids the page faults and kernel overhead associated with operating-system-level remote memory systems while remaining compatible with existing Memcached deployments.

MemExchange was developed as part of my PhD research on distributed memory management for cloud-scale in-memory caching systems.

---

## Highlights

- RDMA-backed remote memory access
- Cluster-wide memory trading across physical servers
- Dynamic memory allocation using online Miss Ratio Curve (MRC) estimation
- Marginal-utility-based memory redistribution
- MemExchange Tracker Communication (MTC) protocol
- Object-level remote memory management
- Integration with Memcached
- Automated experiment and benchmarking framework
- Large-scale evaluation on CloudLab

---

## Motivation

Cloud providers typically provision memory based on **peak demand**, causing significant amounts of memory to remain idle across clusters while other tenants simultaneously experience memory pressure.

Traditional cache memory management is limited to a single physical machine. MemExchange extends memory management across the cluster by allowing tenants to dynamically borrow unused memory from remote servers through low-latency RDMA communication.

The result is a logical cluster-wide memory pool that improves overall memory utilization while reducing cache misses for memory-constrained tenants.

---

## Key Contributions

MemExchange introduces several ideas:

- **Cluster-wide Memory Trading (MTC)** that dynamically reallocates memory between physical servers.
- **Application-layer remote memory management**, avoiding kernel swap paths and page faults.
- **Marginal-utility-based allocation** using online Miss Ratio Curve estimation.
- **Efficient one-sided RDMA communication** for remote object accesses.
- **Large-scale implementation and evaluation** integrated directly into Memcached.

---

## System Overview

At a high level, MemExchange consists of four major components:

- **Memcached tenants** that serve cache requests.
- **Tracker**, which coordinates local and cluster-wide memory trading.
- **RDMA communication layer** enabling low-latency remote memory accesses.
- **Memory Trading Controller (MTC)** responsible for dynamically redistributing memory according to tenant demand.

> **Architecture diagram coming soon**

---

## Repository Structure

```
MemExchange/
├── src/                 # MemExchange implementation
├── docs/                # Documentation
├── scripts/             # Automation and experiment scripts
├── analysis/            # R scripts and figure generation
├── benchmarks/          # Benchmark documentation
└── ...
```

---

## Building

### Install dependencies

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

# Required for Soft-RoCE (RXE)
sudo apt install rdma-core
```

### Compile MemExchange

```bash
cd src/
./autogen
./configure CFLAGS="-w"
make
```

To clean the build:

```bash
make clean
```

---

## Running MemExchange

### Compile the Tracker

```bash
gcc -g -o tracker start_tracker.c shm_malloc.c -lrt -pthread

gcc -g -o stop_tracker stop_tracker.c shm_malloc.c -lrt -pthread
```

### Start the Tracker

Example:

```bash
./tracker 32000 0.25 0.25 0.25 0.25 MTC_ON
```

Arguments:

```
tracker <memory_MB> [tenant_share ...] <MTC_ON|MTC_OFF>
```

When **MTC_ON** is enabled:

- tenants may utilize remote memory
- idle memory may be lent to remote servers
- cluster-wide memory trading is enabled

When **MTC_OFF**:

- memory trading is limited to local tenants only
- no remote memory participation occurs

---

### Run MemExchange

Example:

```bash
./memcached -v -p 11212 -t 4 -m 4096 -G
```

The `-G` flag enables **Greedy Mode**, allowing tenants to grow beyond their initial memory allocation when additional cluster memory becomes available through MemExchange.

---

## Benchmarking

MemExchange was evaluated using:

- CloudSuite
- mutilate

Modified versions used in the evaluation are available in separate repositories.

See **benchmarks/README.md** for setup instructions.

---

## Documentation

Additional documentation is available under `docs/`.

- Architecture
- Memory Trading Controller (MTC)
- Benchmarking
- Reproducing the paper
- Software RDMA (RXE)
- Frequently Asked Questions

---

## Experimental Results

Evaluated on CloudLab deployments ranging from microbenchmarks to clusters of **100 servers**.

Highlights include:

- Up to **2.3×** lower remote-memory overhead compared to TCP-based designs.
- Up to **13%** higher cluster-wide memory utilization.
- Up to **63%** reduction in cache miss rate for memory-constrained tenants.

See the paper for complete evaluation details.

---

## Publications

**MemExchange: Cloud-Scale Memory Trading**

```
@article{...}
```

(arXiv link coming soon)

---

## License

This repository is released for research and educational purposes.

See `LICENSE` for details.
