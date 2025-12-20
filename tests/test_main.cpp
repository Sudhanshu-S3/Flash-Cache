#include <gtest/gtest.h>
#include <cstring>
#include "arena.hpp"
#include "parser.hpp" 

// --- Test 1: The Arena Allocator ---
TEST(ArenaTest, AllocationWorks) {
   
    Arena arena(1024);
    
    char* ptr1 = arena.allocate(10);
    ASSERT_NE(ptr1, nullptr); 
    
    std::memcpy(ptr1, "Hello", 5);
    
    char* ptr2 = arena.allocate(10);
    ASSERT_NE(ptr2, nullptr);
    

    EXPECT_EQ(ptr2, ptr1 + 10);
}

TEST(ArenaTest, OutOfMemory) {
    Arena arena(100); 
    char* ptr = arena.allocate(200); 
    EXPECT_EQ(ptr, nullptr); 
}

// --- Test 2: The RESP Parser ---
TEST(ParserTest, ParsesSetCommand) 
{

    std::string raw = "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$3\r\nval\r\n";
    
    RESPParser parser(raw.c_str(), raw.length());
    std::vector<std::string_view> tokens;
    
    size_t consumed = parser.try_parse_command(tokens);
    
    ASSERT_GT(consumed, 0); 
    ASSERT_EQ(tokens.size(), 3);
    EXPECT_EQ(tokens[0], "SET");
    EXPECT_EQ(tokens[1], "key");
    EXPECT_EQ(tokens[2], "val");
}

TEST(ParserTest, HandlesPartialData) 
{

    std::string raw = "*3\r\n$3\r\nSET\r\n";
    
    RESPParser parser(raw.c_str(), raw.length());
    std::vector<std::string_view> tokens;
    
    size_t consumed = parser.try_parse_command(tokens);
    
    EXPECT_EQ(consumed, 0); 
    EXPECT_TRUE(tokens.empty());
}