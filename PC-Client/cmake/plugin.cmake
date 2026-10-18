# * CMAKE модуль для плагинов

SET(SrcDir "${CMAKE_CURRENT_SOURCE_DIR}/Src")
SET(IncludeDir "${CMAKE_CURRENT_SOURCE_DIR}/Include")

file(GLOB SrcFiles "${SrcDir}/*.cpp")

add_library(${PROJECT_NAME} SHARED
        ${SrcFiles}
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBRARY_DEPENDS_LIST})

target_include_directories(${PROJECT_NAME} PRIVATE ${IncludeDir})

set_target_properties(${PROJECT_NAME} PROPERTIES
        PREFIX ""
        SUFFIX ".xpl"
)