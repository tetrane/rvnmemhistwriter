cmake_minimum_required(VERSION 3.7)
project(test)

find_package(Boost 1.49 COMPONENTS
  unit_test_framework
)

if(NOT Boost_FOUND)
  message(WARNING "Boost not found, don't build tests")
  return()
endif(NOT Boost_FOUND)

add_executable(test_rvnmemhistwriter
  test_chunk.cpp
  test_slice.cpp
  test_db_writer.cpp
)

target_include_directories(test_rvnmemhistwriter PRIVATE ../include)
target_include_directories(test_rvnmemhistwriter PRIVATE ../src)

target_link_libraries(test_rvnmemhistwriter
  PUBLIC
    Boost::boost

  PRIVATE
    rvnmemhistwriter
    Boost::unit_test_framework
)

target_compile_definitions(test_rvnmemhistwriter PRIVATE "BOOST_TEST_DYN_LINK")

add_test(rvnmemhistwriter test_rvnmemhistwriter)
