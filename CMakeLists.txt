cmake_minimum_required(VERSION 3.16)
project(AssetGen LANGUAGES CXX)

# The Linux build; Windows builds AssetGen.sln. Kept out of the sources so they stay a byte-identical mirror of
# ue-mods/AssetGen.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# A system nlohmann-json (nlohmann-json3-dev, nlohmann-json, a vcpkg toolchain) when there is one, else fetched.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

add_executable(assetgen
  Blueprint.cpp Cpp.cpp Main.cpp Package.cpp Registry.cpp Script.cpp Uncooked.cpp Verify.cpp)
target_include_directories(assetgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(assetgen PRIVATE nlohmann_json::nlohmann_json)
