# * CMAKE модуль для библиотек

SET(SrcDir "${CMAKE_CURRENT_SOURCE_DIR}/Src")
SET(IncludeDir "${CMAKE_CURRENT_SOURCE_DIR}/Include")

file(GLOB SrcFiles "${SrcDir}/*.cpp")

add_library(${PROJECT_NAME} STATIC
        ${SrcFiles}
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBRARY_DEPENDS_LIST})

target_include_directories(${PROJECT_NAME} PUBLIC ${IncludeDir})