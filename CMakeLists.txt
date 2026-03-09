cmake_minimum_required(VERSION 3.15)
project(taskman-int LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(taskman-int
    src/main.cpp
    src/http_client.cpp
    src/audio_capture.cpp
    src/prompt_client.cpp
)

find_package(CURL REQUIRED)
if(CURL_FOUND)
    target_include_directories(taskman-int PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(taskman-int PRIVATE ${CURL_LIBRARIES})
endif()

# Link Windows libraries
target_link_libraries(taskman-int PRIVATE gdiplus gdi32 user32)

# Define UNICODE on MSVC
if(MSVC)
    target_compile_definitions(taskman-int PRIVATE UNICODE _UNICODE)
endif()
