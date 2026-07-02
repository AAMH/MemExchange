# Benchmarks and Comparison Systems

MemExchange was evaluated using modified versions of CloudSuite and mutilate. InfiniSwap was used as a comparison system.

These projects are maintained in separate repositories to keep this repository focused on the MemExchange implementation.

## Benchmark Repositories

```text
CloudSuite fork:
https://github.com/AAMH/Cloudsuite-Client.git

mutilate fork:
https://github.com/AAMH/mutilate.git

InfiniSwap fork:
(https://github.com/AAMH/Infiniswap.git
```

## CloudSuite

CloudSuite is used to generate cache workloads based on a Twitter-like dataset.

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
  -a ../twitter_dataset/twitter_dataset_2 \
  -s tenants.txt \
  -g 1 \
  -T 1 \
  -c 25 \
  -w 1 \
  -r 25000
```

## mutilate

mutilate is used to simulate Facebook ETC-style cache workloads.

Example:

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

The `rps_ratio` controls the fraction of the base request rate assigned to that tenant.
