# Benchmarks and Comparison Systems

The MemExchange evaluation uses external benchmark and comparison-system
repositories. They are kept separate from this repository so the MemExchange
implementation, automation scripts, and analysis code remain focused.

## External Repositories

- **[CloudSuite Client (modified)](https://github.com/AAMH/Cloudsuite-Client)**
  for Twitter-like cache workloads.
- **[mutilate (modified)](https://github.com/AAMH/mutilate)** for
  Facebook ETC-style cache workloads.
- **[InfiniSwap (modified)](https://github.com/AAMH/Infiniswap)** as the
  RDMA-based remote-memory comparison system.

## Workloads

The paper evaluates MemExchange with three main workload families:

- **CloudSuite / Twitter:** skewed key access with heterogeneous object sizes.
- **mutilate / Facebook ETC Uniform:** low-locality access with fixed-size
  objects.
- **mutilate / Facebook ETC Zipf:** high-locality access with skewed demand.

The experiments compare static Memcached, local-only dynamic resizing
(MemSweeper-style behavior), MemExchange, and InfiniSwap where applicable.

## CloudSuite Example

Example warm-up and scaling command:

```bash
./loader \
  -a ../twitter_dataset/twitter_dataset_unscaled \
  -o ../twitter_dataset/twitter_dataset_30x \
  -s tenants.txt \
  -w 4 \
  -S 30 \
  -D 3357 \
  -j \
  -T 1 \
  -r 400000
```

Example main phase:

```bash
./loader \
  -a ../twitter_dataset/twitter_dataset_30x \
  -s tenants.txt \
  -g 1 \
  -T 1 \
  -c 25 \
  -w 1 \
  -r 25000
```

## mutilate Example

```bash
./mutilate \
  -s localhost:$port \
  -t 5000 \
  --keysize=fb_key \
  --valuesize=fb_value \
  --iadist=fb_ia \
  --records=3000000 \
  -q 20000 \
  -c 25
```

## Tenant Configuration

CloudSuite uses a tenant configuration file such as `tenants.txt`.

Example line:

```text
localhost, 11212, 1
```

Fields:

```text
ip, port, rps_ratio
```

The `rps_ratio` controls the fraction of the base request rate assigned to that
tenant.
