get_filename_component(RVNMEMHISTWRITER_CMAKE_DIR "${CMAKE_CURRENT_LIST_FILE}" PATH)
include(CMakeFindDependencyMacro)

find_dependency(rvnsqlite REQUIRED)

if(NOT TARGET rvnmemhistwriter)
	include("${RVNMEMHISTWRITER_CMAKE_DIR}/rvnmemhistwriter-targets.cmake")
endif()
