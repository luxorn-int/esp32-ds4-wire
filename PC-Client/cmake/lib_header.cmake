# * CMAKE модуль для header-only библиотек

SET(IncludeDir "${CMAKE_CURRENT_SOURCE_DIR}/Include")

file(GLOB SrcFiles "${IncludeDir}/*.hpp")

add_library(${PROJECT_NAME} INTERFACE
        ${SrcFiles}
)

target_link_libraries(${PROJECT_NAME} INTERFACE ${LIBRARY_DEPENDS_LIST})

target_include_directories(${PROJECT_NAME} INTERFACE ${IncludeDir})