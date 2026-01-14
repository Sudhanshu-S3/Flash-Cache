# Server Architecture Design Document

## Overview

RedisServer implements a single-threaded, event-driven key-value store using Linux epoll for I/O multiplexing. The design prioritizes ultra-low latency over maximum throughput.

## Architecture Diagram


```
┌─────────────────────────────────────────────────┐
│  Client 1   Client 2   Client 3   ...   Client N│
└─────────────┬───────────┬───────────────────────┘
              │           │
              ▼           ▼
         ┌─────────────────────┐
         │   Epoll Event Loop  │ ← Single thread
         └─────────────────────┘
                   │
       ┌───────────┴───────────┐
       ▼                       ▼
  ┌─────────┐           ┌──────────┐
  │ Parser  │           │  Arena   │
  │(Zero-Copy)          │Allocator │
  └─────────┘           └──────────┘
       │                       │
       └───────────┬───────────┘
                   ▼
           ┌──────────────┐
           │  Hash Map    │
           │(std::unordered_map)│
           └──────────────┘
```

## Design Philosophy

### Why Single-Threaded?

Multi-threading introduces overhead:

1. **Lock Contention**
   - Each mutex lock/unlock costs approximately 25ns
   - Cache coherency traffic between cores
   - Lock-free data structures still have atomic operations

2. **Context Switching**
   - TLB flushes (translation lookaside buffer)
   - Instruction cache misses
   - Scheduler overhead

3. **Complexity**
   - Race conditions
   - Deadlocks
   - Harder to reason about performance

Single-threaded benefits:
- Hot instruction cache (tight loop)
- No synchronization overhead
- Predictable latency
- Simple mental model

### When Does This Scale?

For multi-core systems:

**Option 1: Multiple instances with SO_REUSEPORT**
```bash
./flash_cache --port 6379 &  # Instance 1
./flash_cache --port 6379 &  # Instance 2
./flash_cache --port 6379 &  # Instance 3
```

The kernel load balances incoming connections across instances.

Option 2: Application-level sharding

```
Client → Load Balancer → [Server 1: keys A-M]
                        → [Server 2: keys N-Z]
```

## Event Loop Model

Epoll Basics 

Epoll is a Linux-specific I/O multiplexing mechanism:

```cpp
while (true) {
    events = epoll_wait();  // Blocks until activity
    for each event {
        if (event is new connection) {
            accept();
        }
        else {
            read() and process data
        }
    }
}
```

#### Edge-Triggered vs Level-Triggered

Level-Triggered (default):

- Notification: "Socket has data available"
- Notified repeatedly until you read all data
- Easier to use but more system calls

Edge-Triggered (EPOLLET):

- Notification: "Socket state changed"
- Notified only once per event
- Must read until EAGAIN
- Fewer system calls (better performance)

We use edge-triggered mode:

``` ev.events = EPOLLIN | EPOLLET; ```

Non-Blocking I/0
All sockets are set to non-blocking mode:

```fcntl(fd, F_SETFL, flags | O_NONBLOCK);```

This means:
- read() returns immediately if no data
- accept() returns immediately if no connections
- No thread blocking (critical for single-threaded design)

### Connection Lifecycle

Step 1: Accept
```Client connects → TCP handshake → accept() returns new socket```

Actions:
1. Accept connection
2. Set non blocking mode
3. Register with epoll edge- triggered
4. Allocate Client buffer
5. Store in clients_ map

Step 2: Read and Parse
```Data arrives → epoll notifies → read() → parse → execute```

Buffer Management:

```
[existing data][new data read here          ]
 <-consumed->  ^                            ^
 parser.pos    buf.len                      4096
```

After parsing complete commands:

```
memmove(buf.data, buf.data + consumed, remaining);
buf.len = remaining;
```

This shifts unparsed data to the beginning for next read.

Step 3: Execute and Respond
Commands are processed immediately:

- SET: Allocate in Arena, store in hash map
- GET: Lookup in hash map, return value
- PING: Return "+PONG\r\n"

Responses are batched:
```cpp
std::string responseBuffer;
responseBuffer.append("+OK\r\n");
responseBuffer.append("+PONG\r\n");
send(fd, responseBuffer.data(), responseBuffer.size(), 0);
```

Batching reduces system call overhead.

Step 4: Disconnect
Client closes connection or read() returns 0:

- Remove from epoll
- Erase from clients_ map (RAII closes socket)
- Erase client buffer


## Memory Management

### Client Buffers

Each client has a 4KB buffer:
```cpp
struct ClientBuffer {
    char data[4096];
    size_t len = 0;
};
```

Why 4KB:
- Typical TCP packet size
- Fits in single memory page
- Avoids excessive copying

### Value Storage

Values are stored using Arena + Hash Map combination:

```cpp
// Key lookup
std::unordered_map<std::string, std::string_view> store_;

// Value storage
Arena arena_;
```

Flow:
1. Parse command: `SET mykey myvalue`
2. Allocate in Arena: `char* ptr = arena_.Allocate(7);`
3. Copy data: `memcpy(ptr, "myvalue", 7);`
4. Store: `store_["mykey"] = string_view(ptr, 7);`

Benefits:
- Key stored once (string copy)
- Value stored in Arena (zero-copy reference)
- Fast lookups (hash table O(1))

### Trade-off: Key Allocation

Currently keys are `std::string` (heap allocated):
```cpp
store_[std::string(key_view)] = value_view;
```

For maximum performance, keys could also use Arena:
```cpp
char* key_ptr = arena_.Allocate(key.length());
memcpy(key_ptr, key.data(), key.length());
store_.emplace(string_view(key_ptr, key.length()), value_view);
```

Requires custom hash function for string_view keys.

## Performance Characteristics

### System Call Analysis

Per request breakdown:

**Traditional multi-threaded server:**
```
accept()       - 1 system call
read()         - 1 system call
write()        - 1 system call per response
Total: 3+ system calls
```

**Our event-driven server:**
```
epoll_wait()   - 1 system call (batched for multiple clients)
read()         - 1 system call
send()         - 1 system call (batched responses)
Total: ~1.5 system calls per request (amortized)
```

### Memory Access Patterns

Hot path memory accesses:
1. Client buffer (sequential read)
2. Arena allocation (sequential write)
3. Hash map lookup (random read)
4. Response buffer (sequential write)

Cache-friendly characteristics:
- Parser makes single forward pass
- Arena allocations are sequential
- Response building is sequential

### Bottleneck Analysis

Current bottlenecks in order:

1. **Hash Map Key Allocation**
   - Each SET allocates a std::string for the key
   - Solution: Arena-allocate keys

2. **Hash Map Lookup**
   - O(1) average but involves hash computation
   - Solution: Better hash function, or cache recent keys

3. **System Calls**
   - Even with batching, system calls dominate latency
   - Solution: Kernel bypass (DPDK, io_uring)

Not bottlenecks:
- Parser (zero-copy, minimal overhead)
- Arena allocation (just pointer bump)
- Single-threaded model (no lock contention)

## Error Handling

### Connection Errors

```cpp
if (bytes <= 0) {
    // Client disconnected or error
    cleanup_client(fd);
    return;
}
```

### Out of Memory

```cpp
char* ptr = arena_.Allocate(size);
if (!ptr) {
    send(fd, "-ERR OOM\r\n", 11, 0);
    return;
}
```

Production considerations:
- Implement LRU eviction
- Multiple arenas with garbage collection
- Graceful degradation

### Malformed Protocol

Parser returns 0 for incomplete/invalid data:
```cpp
if (consumed == 0) {
    // Wait for more data or invalid format
    break;
}
```

Future improvement: Distinguish between "need more data" and "invalid format".

## Deployment Considerations

### Resource Limits

Set system limits:
```bash
ulimit -n 65535  # Max file descriptors
sysctl -w net.core.somaxconn=1024  # Connection backlog
```

### Monitoring

Key metrics to track:
- Connections per second
- Commands per second
- P50/P99/P999 latency
- Arena memory usage
- Client buffer overflow rate

### Graceful Shutdown

Currently missing. Should implement:
```cpp
void Shutdown() {
    // 1. Stop accepting new connections
    epoll_ctl(epollFd_.Get(), EPOLL_CTL_DEL, serverSocket_.Get(), nullptr);
    
    // 2. Finish processing existing clients
    // 3. Close all connections
    // 4. Persist data (if needed)
}
```

## Future Improvements

### Persistence

Add snapshot or AOF (append-only file):
```cpp
void Snapshot() {
    std::ofstream file("dump.rdb");
    for (auto& [key, value] : store_) {
        file << key << ":" << value << "\n";
    }
}
```

### Replication

Master-slave architecture:
```
Master → Replicate writes → Slave 1
                         → Slave 2
```

### Cluster Mode

Consistent hashing for sharding:
```
Client → Hash(key) % N → Server[N]
```

### Additional Commands

- DEL: Remove key
- EXISTS: Check key existence
- EXPIRE: TTL support
- MGET/MSET: Multi-key operations
- List/Set/Hash data structures

## Related Reading

- [The C10K Problem](http://www.kegel.com/c10k.html)
- [Epoll vs Select](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [Redis Architecture](https://redis.io/docs/reference/internals/)
- [Linux Performance Analysis](https://www.brendangregg.com/linuxperf.html)
```