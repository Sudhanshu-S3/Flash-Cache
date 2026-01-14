# RESP Design Document

## Overview

The RESPParser implements a zero-copy parser for the Redis Serialization Protocol (RESP). It achieves zero heap allocations by using string_view references into the raw network buffer.

## Problem

### Traditional Parser Overhead

Standard parsers copy data into heap-allocated strings:

```cpp
std::string command = "SET";
std::string key = "mykey";
std::string value = "myvalue";
```
For processing 700,000 requests per second:
- 2.1 million malloc calls per second
- Gigabytes of memory copied needlessly
- Heap fragmentation over time

Real-World impact

Parser overhead for a single SET command:
- Traditional parser: 3 allocations x 30ns = 90ns
- Zero-copy parser: 0 allocations = 0ns overhead

## Solution

### Core Concept
Instead of copying data, create lightweight views:

```cpp
// Raw buffer in memory:
// "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n"

std::string_view cmd("SET");        // Just pointer + length
std::string_view key("mykey");      // No allocation
std::string_view value("myvalue");  // No copying
```

A string_view is just:

```cpp
struct string_view
{
    const char* ptr;    // 8 bytes
    size_t length;      // 8 bytes
}
```

Total: 16 bytes on stack ( no heap involve).

## RESP Protocol Format

### Message Structure
RESP uses simple text encoding:

```
*<count>\r\n        Array marker
$<length>\r\n<data>\r\n Bulk string (repeated <count> times)
```

Example: SET Command
```
Client sends:
*3\r\n$3\r\n\SET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n

Breakdown:
*3              -> Array of 3 elements
$3\r\nSET       -> Bulk string "SET" (3 bytes)
$5\r\nmykey     -> Bulk string "mykey" (5 bytes)
$7\r\nmyvalue   -> Bulk string "myvalue" (7 bytes)

Other message types:

Simple strings : +OK\r\n
Errors         : -ERR unknown command\r\n
Integers       : 1000\r\n
Null           : -1\r\n
```

## Parsing Algorithm

State Machine

```
START → Parse '*' → Parse array length → Parse '\r\n' →
  ↓
  FOR each element:
    Parse '$' → Parse string length → Parse '\r\n' →
    Extract string data → Parse '\r\n' →
  ↓
DONE (return bytes consumed)

```
PseudoCode

```
function TryParseCommand():
    1. Check buffer starts with '*'
    2. Parse number until '\r\n' → array_count
    3. For i = 0 to array_count:
        a. Check next char is '$'
        b. Parse number until '\r\n' → string_length
        c. Create string_view(current_position, string_length)
        d. Skip past string data and '\r\n'
    4. Return total bytes consumed

```

Incomplete Data Handling

If buffer contains parital command:

```
"*3\r\n$3\r\nSET\r\n$5\r\nmyk" -> Incomplete!
```

Parser behavior:
1. Detect missing data
2. Reset position to start
3. Return 0
4. Caller should wait for more data

### Memory Safety

Lifetime Requirements

```cpp
char buffer[1024];
read(socket, buffer, 1024);
RESPParser parser(buffer, 1024);
std::vector<std::string_view> tokens;
parser.TryParserCommand(tokens);

//SAFE: tokens point into buffer
process_command(tokens);

//Danger: Don't use tokens after buffer is freed
memset(buffer, 0, 1024); // Now tokens are invaild!

```
### Pipelining Support

Clients can send multiple commands without waiting:

```
Client sends:
SET key1 val1
GET key1
SET key2 val2
GET key2
```

All in one TCP packet.

Implementation

```cpp
RESPParser parser(buffer, buffer_len);
std::vector<std::string_view> tokens;

while (true) 
{
    size_t consumed = parser.TryParseCommand(tokens);
    
    if (consumed == 0) 
    {
        // No more complete commands
        break;
    }
    
    ProcessCommand(tokens);
    tokens.clear();
}
```

The parser automatically advances through the buffer, handling multiple commands in sequence.

## Edge Cases

Malformed input
Parser gracefully handles:

```
"*3\r\n$3\r\nSE"   // Incomplete command
"*abc\r\n"          // Invalid array length
"$5\r\nSET\r\n"     // Length mismatch (claims 5, only 3 bytes)
```

All return 0 (no bytes consumed) without crashing.

#### Maximum Message Size

Current implementation has no limits. Production system should:

```cpp
const size_t MAX_ARRAY_SIZE = 1024;
const size_t MAX_STRING_LENGTH = 512 * 1024 * 1024;  // 512MB

if (array_len > MAX_ARRAY_SIZE) {
    return error("Array too large");
}
```

#### Null Bulk Strings

RESP allows null values:
```
$-1\r\n
```

Current parser does not handle this. Future improvement needed.

## Future Improvements

#### Error Reporting
Instead of returning 0, return error codes:

```
enum ParseResult
{
    OK,
    INCOMPLETE_DATA,
    INVALID_FORMAT,
    EXCEEDS_LIMITS
};

ParseResult TryParseCommand(std::vector<std::string_view>& tokens);
```

#### Streaming Parser
For giant messages that don't fit in buffer:
```
class StreamingParser{
    void FeedData(const char* chunk, size_t len);
    bool IsComplete();
    std::vector<string_view> GetTokens();
};
```

#### Validation

Add optional strict mode:

```cpp
ParseResult TryParseCommand(tokens, ParseOptions opts);

/**
 * Options :-
 * - STRICT: Reject any protocol violations
 * - PERMISSIVE: Accept slight deviations
 * - VALIDATE_UTF8: Ensure strings are valid UTF-8
 */
```

### Related Reading

- [RESP Protocol Specification](https://redis.io/docs/reference/protocol-spec/)
- [Zero-Copy Networking](https://www.linuxjournal.com/article/6345)
- [std::string_view in C++17](https://en.cppreference.com/w/cpp/string/basic_string_view)