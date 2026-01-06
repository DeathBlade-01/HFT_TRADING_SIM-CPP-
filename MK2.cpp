#include <iostream>
#include <queue>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <iomanip>
#include <random>
#include <array>

using namespace std;
using namespace chrono;

// Market data message structure
struct MarketData {
    uint64_t sequenceNum;
    double price;
    uint64_t volume;
    high_resolution_clock::time_point timestamp;
};

// Statistics tracker
struct Statistics {
    atomic<uint64_t> messagesProduced{0};
    atomic<uint64_t> messagesConsumed{0};
    atomic<uint64_t> lockContentions{0};
    atomic<uint64_t> droppedMessages{0};
    atomic<uint64_t> totalLatencyNs{0};
    
    void reset() {
        messagesProduced = 0;
        messagesConsumed = 0;
        lockContentions = 0;
        droppedMessages = 0;
        totalLatencyNs = 0;
    }
    
    void printStats(const string& title, double elapsedSec) {
        uint64_t produced = messagesProduced.load();
        uint64_t consumed = messagesConsumed.load();
        uint64_t contentions = lockContentions.load();
        uint64_t dropped = droppedMessages.load();
        uint64_t avgLatency = consumed > 0 ? totalLatencyNs.load() / consumed : 0;
        
        cout << "\n" << string(60, '=') << "\n";
        cout << "📊 " << title << "\n";
        cout << string(60, '=') << "\n";
        cout << "Messages Produced:    " << produced << "\n";
        cout << "Messages Consumed:    " << consumed << "\n";
        cout << "Messages Dropped:     " << dropped << " (" 
             << (produced > 0 ? (dropped * 100.0 / produced) : 0) << "%)\n";
        cout << "Lock Contentions:     " << contentions << "\n";
        cout << "Average Latency:      " << avgLatency << " ns ("
             << (avgLatency / 1000) << " μs)\n";
        cout << "Throughput:           " << (uint64_t)(produced / elapsedSec) 
             << " msg/sec\n";
        cout << "Consumption Rate:     " << (uint64_t)(consumed / elapsedSec)
             << " msg/sec\n";
        cout << "Elapsed Time:         " << fixed << setprecision(2) 
             << elapsedSec << " seconds\n";
        cout << string(60, '=') << "\n\n";
    }
};

// ============================================================================
// CONVENTIONAL QUEUE WITH MUTEX
// ============================================================================
class ConventionalQueue {
private:
    queue<MarketData> q;
    mutex mtx;
    const size_t MAX_SIZE = 10000;
    Statistics& stats;

public:
    ConventionalQueue(Statistics& s) : stats(s) {}
    
    bool push(const MarketData& data) {
        unique_lock<mutex> lock(mtx, try_to_lock);
        
        if (!lock.owns_lock()) {
            stats.lockContentions++;
            lock.lock();
        }
        
        if (q.size() >= MAX_SIZE) {
            stats.droppedMessages++;
            return false;
        }
        
        q.push(data);
        return true;
    }
    
    bool pop(MarketData& data) {
        unique_lock<mutex> lock(mtx, try_to_lock);
        
        if (!lock.owns_lock()) {
            stats.lockContentions++;
            lock.lock();
        }
        
        if (q.empty()) {
            return false;
        }
        
        data = q.front();
        q.pop();
        return true;
    }
};

// ============================================================================
// LOCK-FREE SPMC RING BUFFER
// ============================================================================
template<size_t SIZE>  // A special template form for the circular ring buffer 
                        // as I need to know the value at compile time
class SPMCRingBuffer {
private:
    static constexpr size_t BUFFER_SIZE = SIZE;
    static constexpr uint64_t MASK = SIZE - 1;
    static_assert((SIZE & MASK) == 0, "SIZE must be power of 2");
    
    alignas(64) atomic<uint64_t> writePos{0}; //VVIMP for cache line padding
    char pad1[64 - sizeof(atomic<uint64_t>)];
    alignas(64) atomic<uint64_t> readPos{0};
    char pad2[64 - sizeof(atomic<uint64_t>)];
    alignas(64) array<MarketData, SIZE> buffer;
    
    Statistics& stats;

public:
    SPMCRingBuffer(Statistics& s) : stats(s) {}
    
    bool push(const MarketData& data) {
        uint64_t currentWrite = writePos.load(memory_order_relaxed); //Only one producer... Therefore No chance of race condition
        uint64_t currentRead = readPos.load(memory_order_acquire); // Another thread may be reading this... So to prevent dirty read
        
        if (currentWrite - currentRead >= SIZE) { // Will work as & is used instead of modulo
            stats.droppedMessages++;
            return false;
        }
        
        buffer[currentWrite & MASK] = data;
        writePos.store(currentWrite + 1, memory_order_release);
        return true;
    }
    
    bool pop(MarketData& data) {
        for (int attempt = 0; attempt < 3; attempt++) {  // Try 3 times
            uint64_t currentRead = readPos.load(memory_order_relaxed);
            uint64_t currentWrite = writePos.load(memory_order_acquire);
            
            if (currentRead >= currentWrite) {
                return false;  // Empty queue
            }
            
            if (readPos.compare_exchange_weak(
                    currentRead, currentRead + 1,
                    memory_order_acquire, memory_order_relaxed)) {
                data = buffer[currentRead & MASK];
                return true;
            }
        }
        
        return false;  // Failed after 3 attempts
    }
};

// ============================================================================
// PRODUCER FUNCTIONS
// ============================================================================
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

template<size_t SIZE>
void producerRingBuffer(SPMCRingBuffer<SIZE>& ringBuffer, Statistics& stats,
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
        
        if (ringBuffer.push(data)) {
            stats.messagesProduced++;
        }
        
        nextSend += interval;
        this_thread::sleep_until(nextSend);
    }
}

// ============================================================================
// CONSUMER FUNCTIONS
// ============================================================================
void consumerConventional(int id, ConventionalQueue& queue, Statistics& stats, 
                         atomic<bool>& running, int processingMicros) {
    MarketData data;
    
    while (running) {
        if (queue.pop(data)) {
            auto now = high_resolution_clock::now();
            auto latency = duration_cast<nanoseconds>(now - data.timestamp).count();
            
            stats.totalLatencyNs += latency;
            stats.messagesConsumed++;
            
            if (processingMicros > 0) {
                this_thread::sleep_for(microseconds(processingMicros));
            }
        } else {
            // Adaptive backoff: yield a few times, then sleep briefly
            static thread_local int spinCount = 0;
            if (spinCount++ < 100) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::nanoseconds(100));  // 0.1μs
                spinCount = 0;
            }
        }
    }
}

template<size_t SIZE>
void consumerRingBuffer(int id, SPMCRingBuffer<SIZE>& ringBuffer, Statistics& stats,
                       atomic<bool>& running, int processingMicros) {
    MarketData data;
    
    while (running) {
        if (ringBuffer.pop(data)) {
            auto now = high_resolution_clock::now();
            auto latency = duration_cast<nanoseconds>(now - data.timestamp).count();
            
            stats.totalLatencyNs += latency;
            stats.messagesConsumed++;
            
            if (processingMicros > 0) {
                this_thread::sleep_for(microseconds(processingMicros));
            }
        } else {
            std::this_thread::yield();  // Keep it simple
        }
    }
}

// ============================================================================
// BENCHMARK RUNNERS
// ============================================================================
void runConventionalBenchmark(int numConsumers, int messagesPerSec, int duration, int processingMicros) {
    cout << "\n";
    cout << "╔════════════════════════════════════════════════════════╗\n";
    cout << "║   CONVENTIONAL MUTEX-BASED QUEUE BENCHMARK            ║\n";
    cout << "╚════════════════════════════════════════════════════════╝\n\n";
    
    Statistics stats;
    ConventionalQueue queue(stats);
    atomic<bool> running{true};
    
    cout << "⚙️  Configuration:\n";
    cout << "   - Consumers: " << numConsumers << "\n";
    cout << "   - Target Rate: " << messagesPerSec << " msg/sec\n";
    cout << "   - Duration: " << duration << " seconds\n";
    cout << "   - Processing Time: " << processingMicros << " μs per message\n";
    cout << "   - Queue Type: Mutex-locked std::queue\n\n";
    
    thread producerThread(producerConventional, ref(queue), ref(stats), 
                          ref(running), messagesPerSec);
    
    vector<thread> consumerThreads;
    for (int i = 0; i < numConsumers; i++) {
        consumerThreads.emplace_back(consumerConventional, i, ref(queue), 
                                     ref(stats), ref(running), processingMicros);
    }
    
    cout << "⏱️  Running...\n";
    
    auto startTime = high_resolution_clock::now();
    this_thread::sleep_for(seconds(duration));
    running = false;
    
    producerThread.join();
    for (auto& t : consumerThreads) {
        t.join();
    }
    
    auto endTime = high_resolution_clock::now();
    double elapsed = duration_cast<milliseconds>(endTime - startTime).count() / 1000.0;
    
    stats.printStats("CONVENTIONAL RESULTS", elapsed);
}

void runRingBufferBenchmark(int numConsumers, int messagesPerSec, int duration, int processingMicros) {
    cout << "\n";
    cout << "╔════════════════════════════════════════════════════════╗\n";
    cout << "║   LOCK-FREE SPMC RING BUFFER BENCHMARK                ║\n";
    cout << "╚════════════════════════════════════════════════════════╝\n\n";
    
    Statistics stats;
    SPMCRingBuffer<16384> ringBuffer(stats);
    atomic<bool> running{true};
    
    cout << "⚙️  Configuration:\n";
    cout << "   - Consumers: " << numConsumers << "\n";
    cout << "   - Target Rate: " << messagesPerSec << " msg/sec\n";
    cout << "   - Duration: " << duration << " seconds\n";
    cout << "   - Processing Time: " << processingMicros << " μs per message\n";
    cout << "   - Buffer Type: Lock-free SPMC Ring Buffer\n\n";
    
    thread producerThread(producerRingBuffer<16384>, ref(ringBuffer), ref(stats),
                          ref(running), messagesPerSec);
    
    vector<thread> consumerThreads;
    for (int i = 0; i < numConsumers; i++) {
        consumerThreads.emplace_back(consumerRingBuffer<16384>, i, ref(ringBuffer),
                                     ref(stats), ref(running), processingMicros);
    }
    
    cout << "⏱️  Running...\n";
    
    auto startTime = high_resolution_clock::now();
    this_thread::sleep_for(seconds(duration));
    running = false;
    
    producerThread.join();
    for (auto& t : consumerThreads) {
        t.join();
    }
    
    auto endTime = high_resolution_clock::now();
    double elapsed = duration_cast<milliseconds>(endTime - startTime).count() / 1000.0;
    
    stats.printStats("LOCK-FREE RESULTS", elapsed);
}

// ============================================================================
// MAIN
// ============================================================================
int main() {
    cout << "\n";
    cout << "╔══════════════════════════════════════════════════════════════╗\n";
    cout << "║        LOCK-FREE vs MUTEX QUEUE: THE ULTIMATE SHOWDOWN      ║\n";
    cout << "╚══════════════════════════════════════════════════════════════╝\n";
    
    // ========================================================================
    // SCENARIO 1: RAW THROUGHPUT TEST (No processing delay)
    // ========================================================================
    cout << "\n";
    cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    cout << "  SCENARIO 1: RAW THROUGHPUT (No artificial processing delay)\n";
    cout << "  This shows pure queue performance - lock overhead matters!\n";
    cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    
    const int CONSUMERS_SCENARIO_1 = 4;
    const int RATE_SCENARIO_1 = 500'000; // 500K msg/sec
    const int DURATION = 3;
    const int PROCESSING_DELAY_1 = 0; // NO delay
    
    runConventionalBenchmark(CONSUMERS_SCENARIO_1, RATE_SCENARIO_1, DURATION, PROCESSING_DELAY_1);
    this_thread::sleep_for(seconds(1));
    runRingBufferBenchmark(CONSUMERS_SCENARIO_1, RATE_SCENARIO_1, DURATION, PROCESSING_DELAY_1);
    
    // ========================================================================
    // SCENARIO 2: BALANCED LOAD (Consumers can keep up)
    // ========================================================================
    cout << "\n\n";
    cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    cout << "  SCENARIO 2: BALANCED LOAD (Consumers match production rate)\n";
    cout << "  With 8 consumers @ 1μs processing: can handle ~8M msg/sec\n";
    cout << "  Production: 100K msg/sec - should have low latency\n";
    cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    
    const int CONSUMERS_SCENARIO_2 = 8;
    const int RATE_SCENARIO_2 = 100'000; // 100K msg/sec 
    const int PROCESSING_DELAY_2 = 1; // 1μs processing
    
    runConventionalBenchmark(CONSUMERS_SCENARIO_2, RATE_SCENARIO_2, DURATION, PROCESSING_DELAY_2);
    this_thread::sleep_for(seconds(1));
    runRingBufferBenchmark(CONSUMERS_SCENARIO_2, RATE_SCENARIO_2, DURATION, PROCESSING_DELAY_2);

    
    return 0;
}