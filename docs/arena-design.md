# Arena Allocator Design Document

## Overview

The Arena allocator is a custom memory management system designed for ultra-low latency scenarios where traditional heap allocation (malloc/free) introduces unacceptable overhead.

## Problem Statement 

### Standard Malloc Overhead

Traditional heap allocation involes:

1. **Free List Traversal** - O(n) search for suitable block
2. **Thread Safety Locks** - Even in single-threaded code, glibc malloc uses locks
3. **Metadata Management** - Bookkeeping for boundary tags, size information
4. **Fragmentation** - External fragmentation reduces cache efficiency

Typical malloc call costs approximately 30 nanoseconds. For a system processing 700,000+ requests per second

### Real-World Impact

For a SET operation storing a 100-byte value:
- **With malloc** : 30ns allocation + actual work
- **With Arena** : <1ns allocation + actual work

## Solution: Linear Arena Allocation

### Core concept

Pre-allocate a large contiguous buffer at startup. Allocation becomes simple pointer arithmetic:

```text
Initial State:
[___________________________] buffer
^
offset = 0

After Allocate(10):
[XXXXXXXXXX_________________] buffer
^
offset = 10

After Allocate(5):
[XXXXXXXXXXYYYYYYYYYYYY______] buffer
^
offset = 15

```

### Algorithm

```cpp
char* Allocate(size_t size)
{
    //Step 1: Bounds Checks

    if(offset + size > capacity)
        return nullptr;

    // Step 2 : Get current pointer
    char* ptr = buffer + offset;

    // Step 3 : Bump pointer (the only "work")
    offset += size;

    return ptr;
}
```

This compiles to approixmately 5 CPU instructions:

1. Load effect
2. Add size to offset
3. Compare with capacity
4. Conditional branch
5. Store new offset

### Memory Model

#### 1. Lifetime Guarantees 

Allocated memory remains valid until:

- Clear() is called
- The Arena object is destroyed

There is NO individual deallocation. This is by design.

#### 2. Cache Efficiency

Contiguous allocations mean:

- Sequential memory access patterns
- Fewer cache line loads
- Better prefetcher prediction

Modern CPUs have hardware prefetchers that detect linear access patterns. Arena allocation produces exactly this pattern.

```
Request 1: SET key1 "value1"
[value1_____________________________]

Request 2: SET key2 "value2" 
[value1value2_______________________]

Request 3: SET key3 "value3"
[value1value2value3_________________]

After Clear():
[___________________________________]
 ^ offset reset to 0
```

### Trade-offs

#### Advantages

1. Performance: 30x faster than malloc
2. Predictability : Constant time allocation (O(1))
3. Cache Friendly : Linear layout improves CPU cache hit rates
4. No Fragmenation : Impossible to fragment since we never have holes

#### Disadvantage

1. No Individual Free : Must clear everything at once
2. Fixed Capacity : Cannot grow dynamically
3. Wasted Space : Memory locked until bulk clear

### When This Works

Perfect for:

1. Request-scoped data (web servers, databases)
2. Temporary buffers that get discarded together
3. High-frequency, short-lived allocations

NOT suitable for:

1. Long-lived objects with varying lifetimes
2. Situations requiring individual deallocation
3. Unpredictable memory usage patterns
4. Implementation Details

#### Why std::vector Instead of Raw malloc?

```cpp
    std::vector<char> buffer_;  // Our choicechar* buffer_;
```

We use std::vector because:

1. RAII: Automatic cleanup in destructor
2. Exception Safety: If allocation fails, std::bad_alloc is thrown
3. No Manual Management: No need to call delete[]
4. Zero Runtime Cost: Same performance as raw pointer
5. Thread Safety: The Arena is not thread-safe. This is intentional.

#### Why:

- FlashCache uses a single-threaded event loop
- Adding locks would defeat the purpose (locks are expensive)
- Multiple threads would need multiple Arena instances

### Memory Alignment

Current implementation does NOT guarantee alignment. Values are packed tightly.

If you need aligned allocations (for SIMD, etc.):

```cpp

char* Allocate(size_t size, size_t alignment = 8) 
{
    size_t aligned_offset = (offset + alignment - 1) & ~(alignment - 1);
    if (aligned_offset + size > buffer.size())
        return nullptr;
    
    offset = aligned_offset + size;
    return buffer.data() + aligned_offset;
}

```

### Future Improvements

#### Slab Allocation

For objects of known sizez, we could implement slav pools:

```

Arena Layout:
[64B blocks][128B blocks][256B blocks][general purpose]

```

This would allow reuse without full Clear();

#### Multiple Arenas

Instead of one 64MB arena, use multiple smaller arenas:

```cpp
std::vector<Arena> arenas_;
int current_arena_ = 0;

char* Allocate(size_t size) 
{
    char* ptr = arenas_[current_arena_].Allocate(size);
    if (!ptr) {
        current_arena_++;
        ptr = arenas_[current_arena_].Allocate(size);
    }
    return ptr;
}
```

This allows partial clearing without losing all data.

#### Compaction

For long-running servers, implement garbage collection:

- Track live allocations
- Copy live data to new arena
- Free old arena
- Update all pointers

This is complex but allows indefinite runtime.

### Related Reading

1. [Memory Allocators 101]( https://www.gingerbill.org/article/2019/02/08/memory-allocation-strategies-002/ )
2. [Cloudflare's arena allocator](https://blog.cloudflare.com/scalable-machine-learning-at-cloudflare/)
3. [Game Programming Patterns: Object Pool](https://gameprogrammingpatterns.com/object-pool.html)
