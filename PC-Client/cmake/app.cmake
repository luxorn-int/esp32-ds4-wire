# * CMAKE модуль для библиотек

SET(SrcDir "${CMAKE_CURRENT_SOURCE_DIR}/Src")
SET(IncludeDir "${CMAKE_CURRENT_SOURCE_DIR}/Include")

file(GLOB SrcFiles "${SrcDir}/*.cpp" "${SrcDir}/*.ui")

add_executable(${PROJECT_NAME}
        ${SrcFiles}
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBRARY_DEPENDS_LIST})

target_include_directories(${PROJECT_NAME} PUBLIC ${IncludeDir})