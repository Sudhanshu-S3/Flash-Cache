# Socket Wrapper Design Document

## Overview

The Socket class is a lightweight RAII wrapper around POSIX file descriptors. It ensures automatic resource cleanup and prevents common bugs like double-close and resource leaks.

## Problem

### Raw File Descriptor Issues

Using raw `int` file descriptors leads to common problems:

```cpp

int fd = socket(AF_INET, SOCK_STREAM, 0);
// BUG: fd never gets closed if exception occurs
close(fd);

```

Other issues:

1. Resource Leaks - Forget to call close()
2. Double Close - Closing the same fd twice (undefined behaviour)
3. Use After Close - Using fd after closing it
4. Exception Safety - Resources leak during stack unwinding

Example Bug:

```cpp
void handle_connection()
{
    int client_fd = accept( server_fd , ...);

    if (some_error)
    {
        //BUG: Forget to close client_fd
        return;
    }

    process_data(client_fd);
    close(client_fd);
}

```

## Solution

### RAII Wrapper

Resource Acquisition is initialization (RAII):

- Acquire resource in constructor
- Release resource in destructor
- Compiler guarantees destructor runs (even during exceptions)

```cpp
void handle_connection()
{
    Socket client(accept(server_fd, ...));

    if (some_error)
    {
        return ;        // Socket destructor automatically closes fd
    }

    process_data(client.Get());
    // Socket destructor automatically closes fd
}

```

## Implementation Details

### Move Semantics

Sockets cannot be copied (what would it mean to have two ownners?), but they can be moved:
```
Socket s1(5);
Socket s2 = std::move(s1);
```

Move constructor implementation:

```cpp
Socket(Socket&& other) noexcept: fd_(other.fd_)
{
    other.fd_ = -1;
}
```

The noexcept is critical:
- Enables optimizations in STL containers
- Guarantees no exceptions during move

Why Not std::unique_ptr?

We could use std::unique_ptr<int, SokcetDelete>, but:

1. Overhead - unique_ptr stores a deleter object ( extra 8 bytes)
2. Clarity - Socket is more explicit than unique_ptr<int>
3. Size - Socket is sizeof(int), unique_ptr would be larger

Deleted Copy Operations

```cpp
Socket(const Socket&) = delete;
Socket& operator = (const Socket&) = delete;
```

This makes copying a compile time error:

```cpp
Socket s1(5);
Socket s2 = s1;
```

Why delete instead of implement:
- Two objects closing the same fd causes double-close bug
- No clear semantics for socket copying

Destructor Implementation
```cpp
~Socket() 
{
    if (fd_ != -1) 
    {
        close(fd_);
    }
}
```

Why check for -1:

- Moved-from sockets have fd_ = -1
- Default constructed sockets have fd_ = -1
- Calling close(-1) returns EBADF (bad file descriptor)

The destructor is not virtual:

- Socket is not designed for inheritance
- Virtual destructor adds vtable pointer overhead
- Keeps class as small as a raw int

### Usage Patterns

1. Storing in Containers
```std::unordered_map<int, std::unique_ptr<Socket> clients_;```

Why unique_ptr wrapper:
- unordered_map requires movable types
- Allows null values (unique_ptr can be nullptr)
- Clear ownership semantics

2. Returning from Functions

```cpp
Socket CreateServerSocket(int port) 
{
    int raw_fd = socket(AF_INET, SOCK_STREAM, 0);
    // ... setup ...
    return Socket(raw_fd);  // Move semantics apply
}
```

No explicit std::move needed - compiler applies RVO (Return Value Optimization).

## Future Improvements

1. Release Method
Allow transferring ownership out:
```cpp
int Release()
{
    int temp = fd_;
    fd_ = -1;
    return temp;
}
```

2. Reset Method
Replace managed fd:

```cpp
void Reset(int new_fd = -1) 
{
    if (fd_ != -1) 
    {
        close(fd_);
    }
    fd_ = new_fd;
}
```

3. Boolean Conversion
Check validity easily:
```cpp
explicit operator bool() const 
{
    return fd_ != -1;
}

// Usage:
if (socket) {  // Instead of if (socket.Get() != -1)
    // Valid socket
}
```

### Related Patterns

std::unique_ptr
Similar ownership semantics but for heap memory.

File Descriptor Managers
- Boost.Asio uses similar RAII socket wrappers
- Qt's QSocketDescriptor
- std::filesystem::path (RAII for filesystem operations)