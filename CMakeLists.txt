cmake_minimum_required(VERSION 3.20)
project(cbtree LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(cbtree
  src/fingerprint.cpp
  src/key_lock_table.cpp
  src/cache_attachment.cpp
  src/ssd_page_store.cpp
  src/node.cpp
  src/tree.cpp
  src/adaptive_policy.cpp
  src/wal_sink.cpp
  src/delete_ops.cpp
)
target_include_directories(cbtree PUBLIC include)

enable_testing()
add_executable(cbtree_tests
  tests/test_types_smoke.cpp
  tests/test_fingerprint.cpp
  tests/test_key_lock_table.cpp
  tests/test_cache_attachment.cpp
  tests/test_ssd_page_store.cpp
  tests/test_tree_basic.cpp
  tests/test_tree_placeholder.cpp
  tests/test_tree_split.cpp
  tests/test_tree_evict.cpp
  tests/test_tree_range.cpp
)
target_link_libraries(cbtree_tests PRIVATE cbtree GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(cbtree_tests)
