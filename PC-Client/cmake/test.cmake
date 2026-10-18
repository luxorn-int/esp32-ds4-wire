SET(SrcDir "${CMAKE_CURRENT_SOURCE_DIR}")

file(GLOB SrcFiles "${SrcDir}/*.cpp")

add_executable(${PROJECT_NAME} ${SrcFiles})

target_include_directories(${PROJECT_NAME}
        PRIVATE
        ${IncludeDir}
        )

target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBRARY_DEPENDS_LIST})

message("TEST: ${PROJECT_NAME}\nDependencies:")
message("\t${LIBRARY_DEPENDS_LIST}\n")

gtest_discover_tests(${PROJECT_NAME})