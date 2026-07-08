cmake_minimum_required(VERSION 3.16)

project(cso-pak-tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cso-pak-tool
    src/Main.cpp
    src/PakArchive.cpp
    src/SnowCipher.cpp
)

target_include_directories(cso-pak-tool PRIVATE src)

install(TARGETS cso-pak-tool RUNTIME DESTINATION .)

enable_testing()

add_executable(snowcipher_vectors
    tests/SnowCipherVectors.cpp
    src/SnowCipher.cpp
)

target_include_directories(snowcipher_vectors PRIVATE src)

add_test(NAME snowcipher_vectors COMMAND snowcipher_vectors)

add_executable(pakarchive_smoke
    tests/PakArchiveSmoke.cpp
    src/PakArchive.cpp
    src/SnowCipher.cpp
)

target_include_directories(pakarchive_smoke PRIVATE src)

add_test(
    NAME pakarchive_smoke
    COMMAND pakarchive_smoke
        "${CMAKE_CURRENT_BINARY_DIR}/pakarchive_smoke_work"
)

if(MSVC)
    target_compile_options(cso-pak-tool PRIVATE /W4 /permissive-)
    target_compile_options(snowcipher_vectors PRIVATE /W4 /permissive-)
    target_compile_options(pakarchive_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(cso-pak-tool PRIVATE -Wall -Wextra -pedantic)
    target_compile_options(snowcipher_vectors PRIVATE -Wall -Wextra -pedantic)
    target_compile_options(pakarchive_smoke PRIVATE -Wall -Wextra -pedantic)
endif()
