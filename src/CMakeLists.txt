# Including 3rdparty folder in include path in order for websocketpp to be correctly found
include_directories(3rdparty)

add_subdirectory(3rdparty)
add_subdirectory(os)
add_subdirectory(pipeline)
add_subdirectory(reporting)
add_subdirectory(util)
