# High-Frequency Trading Message Queue: Lock-Free vs Mutex

A comprehensive performance comparison between lock-free SPMC (Single Producer Multiple Consumer) ring buffer and conventional mutex-based queue implementations for high-frequency trading applications.

## Overview

This project benchmarks two different approaches to concurrent message queue implementations in the context of market data processing:

1. **Conventional Mutex-Based Queue** - Traditional approach using `std::queue` with mutex synchronization
2. **Lock-Free SPMC Ring Buffer** - Advanced lock-free implementation using atomic operations and compare-and-swap (CAS)



## Performance Results

### Scenario 1: Raw Throughput (500K msg/sec, 4 consumers, 0μs processing)

| Metric | Lock-Free SPMC | Mutex-Based | Improvement |
|--------|----------------|-------------|-------------|
| **Average Latency** | **1.3 μs** | 27-36 μs | **~28x faster** |
| **Throughput** | 501K msg/sec | 480K msg/sec | 4% higher |
| **Lock Contentions** | **0** | 1.1M | ∞ |
| **Messages Dropped** | 0 | 0 | - |

### Scenario 2: Balanced Load (100K msg/sec, 8 consumers, 1μs processing)

| Metric | Lock-Free SPMC | Mutex-Based | Notes |
|--------|----------------|-------------|-------|
| **Average Latency** | 135-1002 μs | 153-320 μs | High variance |
| **Throughput** | ~100K msg/sec | ~100K msg/sec | Both saturated, but Lock-Free has higher throughput |
| **Lock Contentions** | **0** | 120K | No contention overhead |
| **Messages Dropped** | 0 | 0 | - |

## Key Insights

### When Lock-Free Dominates:
✅ **High message rates** (500K+ msg/sec)  
✅ **Fast processing** (< 1μs per message)  
✅ **Many competing consumers**  
✅ **Latency-critical applications** (HFT, real-time systems)

### When Mutex is Competitive:
 . Lower message rates with high consumer contention  
 . Processing time >> queue operation time  
 . Simpler codebase requirements

## Technical Implementation

### Lock-Free SPMC Ring Buffer

**Key Techniques:**
- **Atomic operations** with relaxed/acquire/release memory ordering
- **Compare-and-swap (CAS)** for lock-free consumer coordination
- **Cache-line padding** (64-byte alignment) to prevent false sharing
- **Power-of-2 buffer size** for efficient modulo operations using bit masking
- **Single producer optimization** - write position uses relaxed ordering



**Memory Ordering:**
- `memory_order_relaxed`: Single-threaded operations (producer writes)
- `memory_order_acquire`: Reading shared data (consumers check writePos)
- `memory_order_release`: Publishing data (producer updates writePos)

### Conventional Mutex Queue

**Contention Tracking:**
- Uses `try_to_lock` to measure lock contention
- Falls back to blocking lock on contention
- Bounded queue with configurable MAX_SIZE

## Requirements

- **C++17** or later
- **pthread** library (for threading)
- **Modern CPU** with atomic instruction support
- **g++** or **clang++** compiler

## Build & Run

```bash
# Compile with optimizations
g++ -std=c++17 -O3 -pthread -o queue_benchmark MK2.cpp -o queue_benchmark

# Run benchmark
./queue_benchmark
```

**Compiler Flags Explained:**
- `-std=c++17`: C++17 standard
- `-O3`: Maximum optimization
- `-pthread`: Enable POSIX threads
- `-march=native` (optional): Optimize for your CPU

##  Benchmark Scenarios

### Scenario 1: Raw Throughput Test
**Purpose:** Measure pure queue performance without processing overhead

**Configuration:**
- Consumers: 4
- Target Rate: 500,000 msg/sec
- Processing Time: 0 μs
- Duration: 3 seconds

**What it tests:** Lock overhead, cache effects, atomic operation efficiency

### Scenario 2: Balanced Load Test
**Purpose:** Realistic workload where consumers can keep up with production

**Configuration:**
- Consumers: 8
- Target Rate: 100,000 msg/sec
- Processing Time: 1 μs per message
- Duration: 3 seconds

**What it tests:** Queue behavior under normal operating conditions, consumer contention

## Statistics Tracked

- **Messages Produced/Consumed**: Total throughput
- **Messages Dropped**: Queue overflow events
- **Lock Contentions**: Mutex blocking events (0 for lock-free)
- **Average Latency**: Time from message creation to consumption
- **Throughput**: Messages per second
- **Consumption Rate**: Consumer processing rate

##  Learning Points

### Concurrency Concepts
1. **Lock-free programming** with atomic operations
2. **Memory ordering** semantics (acquire/release/relaxed)
3. **Cache-line alignment** and false sharing prevention
4. **Compare-and-swap (CAS)** algorithms
5. **Producer-consumer patterns** (SPMC)

### Performance Engineering
1. **Benchmarking methodology**
2. **Latency vs throughput tradeoffs**
3. **Understanding system bottlenecks**
4. **CPU cache effects**
5. **Lock contention analysis**

## Configuration Options

### Adjustable Parameters in `main()`:

```cpp
// Scenario 1
const int CONSUMERS_SCENARIO_1 = 4;
const int RATE_SCENARIO_1 = 500'000;        // msg/sec
const int PROCESSING_DELAY_1 = 0;            // μs

// Scenario 2
const int CONSUMERS_SCENARIO_2 = 8;
const int RATE_SCENARIO_2 = 100'000;         // msg/sec
const int PROCESSING_DELAY_2 = 1;            // μs

const int DURATION = 3;                      // seconds
```

### Ring Buffer Size:
```cpp
SPMCRingBuffer<16384> ringBuffer(stats);  // 16K entries (must be power of 2)
```

## Known Limitations

1. **Variance in Scenario 2**: Lock-free implementation shows higher latency variance under low-rate/high-contention conditions due to CAS retry overhead
2. **Single Producer Only**: Current implementation optimized for SPMC pattern
3. **Fixed Buffer Size**: Ring buffer size must be known at compile time (template parameter)
4. **No Backpressure**: Drops messages when buffer is full rather than blocking

## References

- [C++ Memory Model](https://en.cppreference.com/w/cpp/atomic/memory_order)
- [Lock-Free Programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)
- [False Sharing](https://mechanical-sympathy.blogspot.com/2011/07/false-sharing.html)
- [1024cores Lock-Free Algorithms](https://www.1024cores.net/home/lock-free-algorithms)


**⭐ If you found this project interesting, please star it on GitHub!**
