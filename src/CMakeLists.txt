# Configure logging
add_definitions(-DULOG_HAVE_TIME)
add_definitions(-DULOG_NO_COLOR)

# Including 3rdparty folder in include path in order for websocketpp to be correctly found
include_directories(3rdparty)

add_subdirectory(3rdparty)
add_subdirectory(os)
add_subdirectory(pipeline)
add_subdirectory(reporting)
add_subdirectory(util)
