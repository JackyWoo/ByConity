add_library(limomp INTERFACE)
target_include_directories(limomp INTERFACE ${CPP_JIEBA_LIMONP_INCLUDE_DIR})

add_library(cppjieba INTERFACE)
target_link_libraries(cppjieba INTERFACE limomp)
target_include_directories(cppjieba INTERFACE ${CPP_JIEBA_INCLUDE_DIR})
