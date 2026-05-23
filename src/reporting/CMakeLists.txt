add_library(fdv_reporting STATIC
    pskreporter.cpp
    pskreporter.h
    FreeDVReporter.cpp
    FreeDVReporter.h
    CsvReporter.cpp
    CsvReporter.h
)

target_compile_options(fdv_reporting PRIVATE ${WARNINGS_AS_ERRORS_FLAGS})
target_link_libraries(fdv_reporting PRIVATE yyjson git_version)
target_include_directories(fdv_reporting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
