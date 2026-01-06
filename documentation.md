# High-Frequency Trading Queue - Detailed Code Documentation

## Table of Contents
1. [Data Structures](#data-structures)
2. [Statistics Tracker](#statistics-tracker)
3. [Conventional Mutex Queue](#conventional-mutex-queue)
4. [Lock-Free SPMC Ring Buffer](#lock-free-spmc-ring-buffer)
5. [Producer Functions](#producer-functions)
6. [Consumer Functions](#consumer-functions)
7. [Benchmark Runners](#benchmark-runners)
8. [Main Function](#main-function)

---

## Data Structures

### MarketData
```cpp
struct MarketData {
    uint64_t sequenceNum;
    double price;
    uint64_t volume;
    high_resolution_clock::time_point timestamp;
};
```

**Purpose:** Represents a single market data message (like a stock price update)

**Fields Explained:**
- `sequenceNum`: Unique message ID to track ordering (0, 1, 2, 3...)
- `price`: Stock/asset price (randomly generated between 100-200 for testing)
- `volume`: Number of shares/units traded (randomly 100-10,000 for testing)
- `timestamp`: Exact time when message was created - used to calculate latency

**Why timestamp matters:** By recording when the message was created and comparing it to when it's consumed, we can measure how long messages wait in the queue (latency).

---

## Statistics Tracker

### Statistics Structure
```cpp
struct Statistics {
    atomic<uint64_t> messagesProduced{0};
    atomic<uint64_t> messagesConsumed{0};
    atomic<uint64_t> lockContentions{0};
    atomic<uint64_t> droppedMessages{0};
    atomic<uint64_t> totalLatencyNs{0};
    
    void reset();
    void printStats(const string& title, double elapsedSec);
};
```

**Purpose:** Thread-safe counter to track benchmark metrics

**Why atomic?** Multiple threads (producer + consumers) update these counters simultaneously. `atomic` ensures no data corruption without needing locks.

### reset()
```cpp
void reset() {
    messagesProduced = 0;
    messagesConsumed = 0;
    lockContentions = 0;
    droppedMessages = 0;
    totalLatencyNs = 0;
}
```

**Purpose:** Reset all counters to 0 before starting a new benchmark

**When called:** Between different benchmark runs to get fresh statistics

### printStats()
```cpp
void printStats(const string& title, double elapsedSec) {
    uint64_t produced = messagesProduced.load();
    uint64_t consumed = messagesConsumed.load();
    uint64_t contentions = lockContentions.load();
    uint64_t dropped = droppedMessages.load();
    uint64_t avgLatency = consumed > 0 ? totalLatencyNs.load() / consumed : 0;
    
    // ... printing code ...
}
```

**Purpose:** Calculate and display benchmark results

**Key Calculations:**
- `avgLatency = totalLatencyNs / consumed`: Average time each message spent waiting
- `throughput = produced / elapsedSec`: Messages per second produced
- `consumption rate = consumed / elapsedSec`: Messages per second consumed
- `drop rate = (dropped * 100.0 / produced)`: Percentage of messages lost

NOTE: Atomics require explicit `.load()` to read their value safely.

---

## Conventional Mutex Queue

### Class Structure
```cpp
class ConventionalQueue {
private:
    queue<MarketData> q;              // Standard C++ queue
    mutex mtx;                        // Lock for thread safety
    const size_t MAX_SIZE = 10000;    // Prevent unlimited growth
    Statistics& stats;                // Reference to shared statistics
};
```

**Purpose:** Traditional thread-safe queue using mutex (lock) for synchronization

### push()
```cpp
bool push(const MarketData& data) {
    unique_lock<mutex> lock(mtx, try_to_lock);
    
    // Try to acquire lock without blocking
    if (!lock.owns_lock()) {
        stats.lockContentions++;      // Someone else has the lock
        lock.lock();                  // Now wait for it
    }
    
    // Check if queue is full
    if (q.size() >= MAX_SIZE) {
        stats.droppedMessages++;
        return false;                 // Reject message
    }
    
    q.push(data);                     // Add message to queue
    return true;                      // Success
}
```

**Step-by-step:**
1. Try to grab the lock without waiting (`try_to_lock`)
2. If lock is already held by another thread → record contention, then wait
3. Once we have the lock, check if queue is full
4. If full → drop message and return false
5. If space available → add message and return true


### pop()
```cpp
bool pop(MarketData& data) {
    unique_lock<mutex> lock(mtx, try_to_lock);
    
    // Try to acquire lock without blocking
    if (!lock.owns_lock()) {
        stats.lockContentions++;
        lock.lock();
    }
    
    // Check if queue is empty
    if (q.empty()) {
        return false;                 // Nothing to consume
    }
    
    data = q.front();                 // Copy front message
    q.pop();                          // Remove it from queue
    return true;                      // Success
}
```

**Step-by-step:**
1. Try to grab the lock (same as push)
2. Check if queue is empty
3. If empty → return false (consumer will try again later)
4. If not empty → copy the front message, remove it, return true

**Why copy first, then pop?** Standard `queue::pop()` doesn't return a value - it only removes. We need `front()` to get the data.

---

## Lock-Free SPMC Ring Buffer

### Class Structure
```cpp
template<size_t SIZE>
class SPMCRingBuffer {
private:
    static constexpr size_t BUFFER_SIZE = SIZE;
    static constexpr uint64_t MASK = SIZE - 1;
    static_assert((SIZE & MASK) == 0, "SIZE must be power of 2");
    
    alignas(64) atomic<uint64_t> writePos{0};
    char pad1[64 - sizeof(atomic<uint64_t>)];
    alignas(64) atomic<uint64_t> readPos{0};
    char pad2[64 - sizeof(atomic<uint64_t>)];
    alignas(64) array<MarketData, SIZE> buffer;
    
    Statistics& stats;
};
```

**Key Concepts:**

#### Template Parameter
```cpp
template<size_t SIZE>
```
- SIZE must be known at compile time
- Allows compiler to optimize with the exact buffer size
- Example: `SPMCRingBuffer<16384>` creates a 16K entry buffer

#### Power-of-2 Requirement
```cpp
static_assert((SIZE & MASK) == 0, "SIZE must be power of 2");
```
**Why?** Enables fast modulo using bitwise AND:
- Slow: `position % 16384` (division operation)
- Fast: `position & 16383` (single CPU instruction)
- Example: `100 & 15 = 4` is same as `100 % 16`

#### Cache Line Alignment
```cpp
alignas(64) atomic<uint64_t> writePos{0};
char pad1[64 - sizeof(atomic<uint64_t>)];
alignas(64) atomic<uint64_t> readPos{0};
char pad2[64 - sizeof(atomic<uint64_t>)];
```

**What's happening here?**

Modern CPUs load memory in 64-byte chunks called "cache lines". If two variables are in the same cache line, updating one on CPU core 1 forces CPU core 2 to reload the entire cache line - called "false sharing."

**The Fix:**
- `alignas(64)`: Forces variable to start at 64-byte boundary
- `char pad1[64 - ...]`: Fills rest of cache line with padding
- Result: `writePos` and `readPos` are in different cache lines

**Visual representation:**
```
Cache Line 0: [writePos + padding.........................]
Cache Line 1: [readPos + padding..........................]
Cache Line 2: [buffer data................................]
```

**Why this matters:** Producer updates `writePos` constantly, consumers update `readPos` constantly. Without padding, they'd interfere with each other's cache, killing performance.

#### Atomic Positions
```cpp
atomic<uint64_t> writePos{0};  // Only producer writes
atomic<uint64_t> readPos{0};   // Multiple consumers compete
```

**Ring buffer concept:**
- Circular array that wraps around
- `writePos` tracks where to insert next message
- `readPos` tracks where to read next message
- When position reaches SIZE, it wraps to 0

**Example with SIZE=8:**
```
writePos=5, readPos=3
Buffer: [old][old][old][msg][msg][msg][empty][empty]
                      ^read      ^write
```

### push()
```cpp
bool push(const MarketData& data) {
    uint64_t currentWrite = writePos.load(memory_order_relaxed);
    uint64_t currentRead = readPos.load(memory_order_acquire);
    
    // Check if buffer is full
    if (currentWrite - currentRead >= SIZE) {
        stats.droppedMessages++;
        return false;
    }
    
    // Write data to buffer
    buffer[currentWrite & MASK] = data;
    
    // Publish write position
    writePos.store(currentWrite + 1, memory_order_release);
    return true;
}
```

**Memory ordering explained:**

#### `memory_order_relaxed` (reading writePos)
```cpp
uint64_t currentWrite = writePos.load(memory_order_relaxed);
```
- **Meaning:** No synchronization needed
- **Why?** Only the producer thread writes to `writePos`, so no race condition possible
- **Benefit:** Faster than other memory orders

#### `memory_order_acquire` (reading readPos)
```cpp
uint64_t currentRead = readPos.load(memory_order_acquire);
```
- **Meaning:** Ensures we see the latest value written by consumers
- **Why?** Multiple consumer threads update `readPos`, we need the most recent value
- **Prevents:** Reading stale data from cache

#### Full Buffer Check
```cpp
if (currentWrite - currentRead >= SIZE) {
```
**How this works:**
- If write is 100 and read is 90: `100 - 90 = 10` slots used (OK if SIZE > 10)
- If write is 16390 and read is 10: `16390 - 10 = 16380` slots used (full if SIZE = 16384)
- Works even with wraparound due to unsigned integer overflow behavior

#### Writing Data
```cpp
buffer[currentWrite & MASK] = data;
```
- `currentWrite & MASK` converts absolute position to buffer index
- Example: If currentWrite=16385 and MASK=16383: `16385 & 16383 = 1`
- This is the fast modulo we discussed earlier

#### `memory_order_release` (updating writePos)
```cpp
writePos.store(currentWrite + 1, memory_order_release);
```
- **Meaning:** All previous writes (the `buffer[...] = data`) are visible before this
- **Why?** Ensures consumers see the complete message data when they see the new writePos
- **Prevents:** Consumers reading half-written data

### pop()
```cpp
bool pop(MarketData& data) {
    while (true) {
        uint64_t currentRead = readPos.load(memory_order_relaxed);
        uint64_t currentWrite = writePos.load(memory_order_acquire);
        
        // Check if buffer is empty
        if (currentRead >= currentWrite) {
            return false;
        }
        
        // Try to claim this position
        if (readPos.compare_exchange_weak(
                currentRead, currentRead + 1,
                memory_order_acquire, memory_order_relaxed)) {
            // Success! Read the data
            data = buffer[currentRead & MASK];
            return true;
        }
        
        // Failed - another consumer won, retry
        std::this_thread::yield();
    }
}
```

**Step-by-step explanation:**

#### Step 1: Load positions
```cpp
uint64_t currentRead = readPos.load(memory_order_relaxed);
uint64_t currentWrite = writePos.load(memory_order_acquire);
```
- Get current read position (relaxed because we're about to use CAS anyway)
- Get current write position (acquire to see producer's latest writes)

#### Step 2: Empty check
```cpp
if (currentRead >= currentWrite) {
    return false;
}
```
- If read caught up to write → buffer is empty
- Consumer will try again later

#### Step 3: Compare-and-Swap (CAS) - THE KEY OPERATION
```cpp
if (readPos.compare_exchange_weak(
        currentRead, currentRead + 1,
        memory_order_acquire, memory_order_relaxed))
```

**What CAS does:**
```
Atomic operation that does:
  if (readPos == currentRead) {
      readPos = currentRead + 1;
      return true;  // Success
  } else {
      currentRead = actual_readPos;  // Updates currentRead
      return false; // Another thread changed it
  }
```

**Why this works for multiple consumers:**
- Consumer A sees readPos=10, tries to set it to 11
- Consumer B sees readPos=10, tries to set it to 11
- Only ONE will succeed (hardware guarantees this)
- The loser retries with the new value (11)

**Real-world example:**
```
Initial: readPos=10, 3 consumers try to pop simultaneously

Consumer 1: CAS(10→11) → SUCCESS ✓ (gets message at position 10)
Consumer 2: CAS(10→11) → FAIL ✗ (readPos is already 11)
Consumer 3: CAS(10→11) → FAIL ✗ (readPos is already 11)

Consumer 2 & 3 loop back and retry:
Consumer 2: CAS(11→12) → SUCCESS ✓ (gets message at position 11)
Consumer 3: CAS(11→12) → FAIL ✗ (readPos is already 12)

Consumer 3 retries again:
Consumer 3: CAS(12→13) → SUCCESS ✓ (gets message at position 12)
```

#### Step 4: Read the data
```cpp
data = buffer[currentRead & MASK];
return true;
```
- Only the winner of CAS gets here
- Read data from the position we claimed
- Return success

#### Step 5: Retry on failure
```cpp
std::this_thread::yield();
```
- If CAS failed, give CPU to other threads briefly
- Loop back and try again with updated `currentRead`

---

## Producer Functions

### producerConventional()
```cpp
void producerConventional(ConventionalQueue& queue, Statistics& stats, 
                         atomic<bool>& running, int messagesPerSec) {
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<> priceDist(100.0, 200.0);
    uniform_int_distribution<> volumeDist(100, 10000);
    
    uint64_t seqNum = 0;
    auto interval = nanoseconds(1'000'000'000 / messagesPerSec);
    auto nextSend = high_resolution_clock::now();
    
    while (running) {
        MarketData data{
            seqNum++,
            priceDist(gen),
            static_cast<uint64_t>(volumeDist(gen)),
            high_resolution_clock::now()
        };
        
        if (queue.push(data)) {
            stats.messagesProduced++;
        }
        
        nextSend += interval;
        this_thread::sleep_until(nextSend);
    }
}
```

**Purpose:** Generate market data messages at a controlled rate

**Random number generation:**
```cpp
random_device rd;                              // Seed
mt19937 gen(rd());                            // Random number engine
uniform_real_distribution<> priceDist(100.0, 200.0);   // Prices: 100-200
uniform_int_distribution<> volumeDist(100, 10000);     // Volume: 100-10,000
```

**Rate limiting:**
```cpp
auto interval = nanoseconds(1'000'000'000 / messagesPerSec);
```
- 1 billion nanoseconds = 1 second
- If messagesPerSec = 100,000: interval = 10,000 ns = 10 μs
- Means send one message every 10 microseconds

**Timing loop:**
```cpp
auto nextSend = high_resolution_clock::now();

while (running) {
    // ... create and send message ...
    
    nextSend += interval;
    this_thread::sleep_until(nextSend);
}
```
- Calculate absolute time for next message
- Sleep until that time
- **Why not sleep(interval)?** Accumulated timing errors would add up

**Message creation:**
```cpp
MarketData data{
    seqNum++,                          // 0, 1, 2, 3...
    priceDist(gen),                    // Random price
    static_cast<uint64_t>(volumeDist(gen)),  // Random volume
    high_resolution_clock::now()       // Current timestamp
};
```

### producerRingBuffer()
```cpp
template<size_t SIZE>
void producerRingBuffer(SPMCRingBuffer<SIZE>& ringBuffer, Statistics& stats,
                       atomic<bool>& running, int messagesPerSec)
```

**Purpose:** Same as `producerConventional()` but for lock-free ring buffer

**Only difference:** Uses `ringBuffer.push(data)` instead of `queue.push(data)`

**Why template?** Because `SPMCRingBuffer` is templated with SIZE, the function that uses it must also be templated.

---

## Consumer Functions

### consumerConventional()
```cpp
void consumerConventional(int id, ConventionalQueue& queue, Statistics& stats, 
                         atomic<bool>& running, int processingMicros) {
    MarketData data;
    
    while (running) {
        if (queue.pop(data)) {
            // Calculate latency
            auto now = high_resolution_clock::now();
            auto latency = duration_cast<nanoseconds>(now - data.timestamp).count();
            
            stats.totalLatencyNs += latency;
            stats.messagesConsumed++;
            
            // Simulate processing time
            if (processingMicros > 0) {
                this_thread::sleep_for(microseconds(processingMicros));
            }
        } else {
            // Queue empty, yield CPU
            this_thread::yield();
        }
    }
}
```

**Purpose:** Continuously consume messages from queue and track latency

**Main loop:**
```cpp
while (running) {
    if (queue.pop(data)) {
        // Process message
    } else {
        // Queue empty, wait a bit
    }
}
```

**Latency calculation:**
```cpp
auto now = high_resolution_clock::now();
auto latency = duration_cast<nanoseconds>(now - data.timestamp).count();
```
- `data.timestamp`: When message was created (by producer)
- `now`: When message was consumed (right now)
- `latency`: Difference in nanoseconds = how long message waited in queue

**Statistics update:**
```cpp
stats.totalLatencyNs += latency;
stats.messagesConsumed++;
```
- Add this message's latency to total (used to calculate average later)
- Increment consumed count

**Processing simulation:**
```cpp
if (processingMicros > 0) {
    this_thread::sleep_for(microseconds(processingMicros));
}
```
- Simulates real work (e.g., updating database, sending to network)
- In Scenario 1: 0 μs (test pure queue performance)
- In Scenario 2: 1 μs (realistic processing time)

**Empty queue handling:**
```cpp
} else {
    this_thread::yield();
}
```
- If queue is empty, give CPU to other threads
- Prevents busy-waiting that would waste CPU

### consumerRingBuffer()
```cpp
template<size_t SIZE>
void consumerRingBuffer(int id, SPMCRingBuffer<SIZE>& ringBuffer, Statistics& stats,
                       atomic<bool>& running, int processingMicros)
```

**Purpose:** Same as `consumerConventional()` but for lock-free ring buffer

**Only difference:** Uses `ringBuffer.pop(data)` instead of `queue.pop(data)`

---

## Benchmark Runners

### runConventionalBenchmark()
```cpp
void runConventionalBenchmark(int numConsumers, int messagesPerSec, 
                             int duration, int processingMicros) {
    // Print header
    // ...
    
    Statistics stats;
    ConventionalQueue queue(stats);
    atomic<bool> running{true};
    
    // Print configuration
    // ...
    
    // Start producer thread
    thread producerThread(producerConventional, ref(queue), ref(stats), 
                          ref(running), messagesPerSec);
    
    // Start consumer threads
    vector<thread> consumerThreads;
    for (int i = 0; i < numConsumers; i++) {
        consumerThreads.emplace_back(consumerConventional, i, ref(queue), 
                                     ref(stats), ref(running), processingMicros);
    }
    
    // Run for specified duration
    auto startTime = high_resolution_clock::now();
    this_thread::sleep_for(seconds(duration));
    running = false;
    
    // Wait for all threads to finish
    producerThread.join();
    for (auto& t : consumerThreads) {
        t.join();
    }
    
    // Calculate elapsed time and print results
    auto endTime = high_resolution_clock::now();
    double elapsed = duration_cast<milliseconds>(endTime - startTime).count() / 1000.0;
    
    stats.printStats("CONVENTIONAL RESULTS", elapsed);
}
```

**Step-by-step:**

#### 1. Setup
```cpp
Statistics stats;
ConventionalQueue queue(stats);
atomic<bool> running{true};
```
- Create statistics tracker
- Create queue (passing stats reference so queue can update counters)
- Create shared `running` flag (all threads will check this)

#### 2. Start Threads
```cpp
thread producerThread(producerConventional, ref(queue), ref(stats), 
                      ref(running), messagesPerSec);
```
- Launches producer in separate thread
- `ref()`: Pass by reference (not copy)

```cpp
vector<thread> consumerThreads;
for (int i = 0; i < numConsumers; i++) {
    consumerThreads.emplace_back(consumerConventional, i, ref(queue), 
                                 ref(stats), ref(running), processingMicros);
}
```
- Launch N consumer threads
- Each gets unique `id` (0, 1, 2, 3...)
- All share same queue, stats, and running flag

#### 3. Run Benchmark
```cpp
auto startTime = high_resolution_clock::now();
this_thread::sleep_for(seconds(duration));
running = false;
```
- Record start time
- Main thread sleeps for benchmark duration (e.g., 3 seconds)
- Set running=false to signal all threads to stop

#### 4. Wait for Completion
```cpp
producerThread.join();
for (auto& t : consumerThreads) {
    t.join();
}
```
- `join()`: Wait for thread to finish
- Ensures all threads complete before calculating results
- **Why needed?** Threads might still be processing when `running=false`

#### 5. Calculate Results
```cpp
auto endTime = high_resolution_clock::now();
double elapsed = duration_cast<milliseconds>(endTime - startTime).count() / 1000.0;
stats.printStats("CONVENTIONAL RESULTS", elapsed);
```
- Measure actual elapsed time
- Print all statistics

### runRingBufferBenchmark()
```cpp
void runRingBufferBenchmark(int numConsumers, int messagesPerSec, 
                           int duration, int processingMicros)
```

**Purpose:** Same as `runConventionalBenchmark()` but tests lock-free ring buffer

**Only differences:**
- Uses `SPMCRingBuffer<16384>` instead of `ConventionalQueue`
- Uses `producerRingBuffer<16384>` and `consumerRingBuffer<16384>`

---

## Main Function

### main()
```cpp
int main() {
    // Print title banner
    // ...
    
    // ========================================================================
    // SCENARIO 1: RAW THROUGHPUT TEST
    // ========================================================================
    const int CONSUMERS_SCENARIO_1 = 4;
    const int RATE_SCENARIO_1 = 500'000;
    const int DURATION = 3;
    const int PROCESSING_DELAY_1 = 0;
    
    runConventionalBenchmark(CONSUMERS_SCENARIO_1, RATE_SCENARIO_1, 
                            DURATION, PROCESSING_DELAY_1);
    this_thread::sleep_for(seconds(1));
    runRingBufferBenchmark(CONSUMERS_SCENARIO_1, RATE_SCENARIO_1, 
                          DURATION, PROCESSING_DELAY_1);
    
    // ========================================================================
    // SCENARIO 2: BALANCED LOAD TEST
    // ========================================================================
    const int CONSUMERS_SCENARIO_2 = 8;
    const int RATE_SCENARIO_2 = 100'000;
    const int PROCESSING_DELAY_2 = 1;
    
    runConventionalBenchmark(CONSUMERS_SCENARIO_2, RATE_SCENARIO_2, 
                            DURATION, PROCESSING_DELAY_2);
    this_thread::sleep_for(seconds(1));
    runRingBufferBenchmark(CONSUMERS_SCENARIO_2, RATE_SCENARIO_2, 
                          DURATION, PROCESSING_DELAY_2);
    
    // Print final comparison
    // ...
    
    return 0;
}
```

**Scenario 1 - Raw Throughput:**
- **Goal:** Test pure queue performance
- **Setup:** 4 consumers, 500K msg/sec, 0 μs processing
- **What it shows:** Lock overhead becomes the bottleneck
- **Expected:** Lock-free dominates (20-30x lower latency)

**Scenario 2 - Balanced Load:**
- **Goal:** Test realistic workload
- **Setup:** 8 consumers, 100K msg/sec, 1 μs processing
- **What it shows:** Both queues can keep up
- **Expected:** Similar throughput, lock-free has lower latency

**Why sleep between runs?**
```cpp
this_thread::sleep_for(seconds(1));
```
- Let system settle (threads fully terminated, caches cleared)
- Prevents interference between benchmarks

---

## Key Concepts Summary

### Memory Ordering Quick Reference

| Memory Order | Use Case | Example |
|--------------|----------|---------|
| `relaxed` | Single-threaded access | Producer reading its own `writePos` |
| `acquire` | Reading shared data | Consumer reading `writePos` from producer |
| `release` | Publishing data | Producer updating `writePos` after writing data |

### Why Lock-Free is Faster

**Mutex Queue:**
```
Consumer 1: Try lock → WAIT (blocked) → Wake up → Grab lock → Pop → Unlock
Consumer 2: Try lock → WAIT (blocked) → Wake up → Grab lock → Pop → Unlock
Consumer 3: Try lock → WAIT (blocked) → Wake up → Grab lock → Pop → Unlock
```
- **Problem:** Waiting wastes time, context switches are expensive

**Lock-Free Queue:**
```
Consumer 1: CAS → SUCCESS → Read data (all in nanoseconds)
Consumer 2: CAS → FAIL → Retry CAS → SUCCESS → Read data
Consumer 3: CAS → FAIL → Retry CAS → FAIL → Retry CAS → SUCCESS → Read data
```
- **Advantage:** No blocking, no context switches, just fast retry

### When Each Approach Wins

**Lock-Free wins when:**
- Very high message rates (500K+ msg/sec)
- Many competing consumers
- Processing time is tiny (< 1 μs)
- Latency is critical

**Mutex is acceptable when:**
- Lower message rates
- Processing time dominates (milliseconds)
- Simplicity matters more than peak performance
- Fewer competing consumers

---

## Additional Notes

### Thread Safety Guarantees

**Conventional Queue:**
- Thread-safe: Mutex ensures only one thread accesses queue at a time
-  Performance: Blocking on contention

**Lock-Free Ring Buffer:**
-  Thread-safe: Atomic operations ensure correctness
-  Performance: No blocking, only retry on contention
-  Limitation: Single producer only (SPMC pattern)

### Buffer Size Considerations

**Why 16384 (16K) entries?**
- Power of 2 for fast modulo
- Large enough to handle bursts (at 500K msg/sec, fills in 32ms)
- Small enough to fit in CPU cache (16K × 32 bytes ≈ 512KB)

**What if buffer is too small?**
- Messages get dropped when full
- Shows up in "Messages Dropped" statistic

**What if buffer is too large?**
- Wastes memory
- May not fit in cache, reducing performance

---
