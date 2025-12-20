#pragma once
#include <vector>
#include <string_view>
#include <cstddef>

// High-performance RESP parser that works on raw buffer
struct RESPParser
{
    const char* data;
    size_t len;
    size_t pos;
    
    RESPParser(const char* d, size_t l) : data(d), len(l), pos(0) {}
    
    // Fast integer parsing
    inline int parse_int(size_t& p) 
    {
        int result = 0;
        bool negative = false;
        
        if (p < len && data[p] == '-') {
            negative = true;
            p++;
        }
        
        while (p < len && data[p] >= '0' && data[p] <= '9') 
        {
            result = result * 10 + (data[p] - '0');
            p++;
        }
        
        return negative ? -result : result;
    }
    
    // Returns number of bytes consumed, or 0 if incomplete
    size_t try_parse_command(std::vector<std::string_view>& tokens) 
    {
        tokens.clear();
        size_t start = pos;
        
        if (pos >= len) return 0;
        if (data[pos] != '*') return 0;
        pos++;
        
        int array_size = parse_int(pos);
        if (pos + 1 >= len || data[pos] != '\r' || data[pos + 1] != '\n') 
        { 
            pos = start; tokens.clear(); return 0; 
        }
        pos += 2;
        
        for (int i = 0; i < array_size; i++) 
        {
            if (pos >= len || data[pos] != '$') 
            { 
                pos = start; tokens.clear(); return 0; 
            }
            pos++;
            
            int str_len = parse_int(pos);
            if (pos + 1 >= len || data[pos] != '\r' || data[pos + 1] != '\n') 
            { 
                pos = start; tokens.clear(); return 0; 
            }
            pos += 2;
            
            if (pos + str_len + 2 > len) 
            { 
                pos = start; tokens.clear(); return 0; 
            }
            
            // ZERO COPY MAGIC HERE:
            // Instead of making a string, we make a view.
            tokens.emplace_back(data + pos, str_len);
            
            pos += str_len + 2; 
        }
        return pos - start; 
    }
};