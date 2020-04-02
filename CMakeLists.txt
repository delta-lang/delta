cmake_minimum_required(VERSION 3.8)
project(delta)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_CXX_FLAGS "-w14062 -wd4244 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING ${CMAKE_CXX_FLAGS}")

    # Use release version of Runtime Library to match prebuilt LLVM.
    # https://gitlab.kitware.com/cmake/community/wikis/FAQ#how-can-i-build-my-msvc-application-with-a-static-runtime
    foreach(flag_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/MDd" "/MD" ${flag_var} "${${flag_var}}")
        string(REGEX REPLACE "/DNDEBUG" "" ${flag_var} "${${flag_var}}")
    endforeach(flag_var)

    if(NOT DEFINED ENV{CI})
        # Disable run-time error checks locally to speed up debug build.
        string(REGEX REPLACE "/RTC[^ ]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REGEX REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GS- /MP /D_ITERATOR_DEBUG_LEVEL=0 /D_SCL_SECURE=0")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /incremental /DEBUG:FASTLINK")
    endif()
else()
    set(CMAKE_CXX_FLAGS "-fno-rtti -fvisibility-inlines-hidden -Wall -Wextra -Wno-unknown-pragmas ${CMAKE_CXX_FLAGS}")

    foreach(flag_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "-DNDEBUG" "" ${flag_var} "${${flag_var}}")
    endforeach(flag_var)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(CMAKE_CXX_FLAGS "-Wsuggest-override ${CMAKE_CXX_FLAGS}")
    endif()

    if(DEFINED ENV{CI})
        set(CMAKE_CXX_FLAGS "-Werror ${CMAKE_CXX_FLAGS}")
    endif()

    if(NOT MINGW)
        set(CMAKE_EXE_LINKER_FLAGS "-rdynamic ${CMAKE_EXE_LINKER_FLAGS}")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "--system-header-prefix=llvm --system-header-prefix=clang ${CMAKE_CXX_FLAGS}")
endif()

find_package(LLVM 10.0 REQUIRED)
find_package(Clang REQUIRED)
set(LLVM_PACKAGE_VERSION "${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}")
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(-DDELTA_ROOT_DIR="${PROJECT_SOURCE_DIR}")

# Define the include path of the Clang builtin headers, to be used by the compiler to avoid errors
# about missing C headers when importing C headers from Delta code. For more information, see e.g.
# http://clang.llvm.org/docs/FAQ.html#i-get-errors-about-some-headers-being-missing-stddef-h-stdarg-h
# CLANG_CMAKE_DIR is defined by ClangConfig.cmake during the above find_package(Clang) call.
add_definitions(-DCLANG_BUILTIN_INCLUDE_PATH="${CLANG_CMAKE_DIR}/../../clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include")

enable_testing()

file(GLOB DELTA_SOURCES src/ast/*.h src/ast/*.cpp src/driver/*.h src/driver/*.cpp src/irgen/*.h src/irgen/*.cpp src/package-manager/*.h
    src/package-manager/*.cpp src/parser/*.h src/parser/*.cpp src/sema/*.h src/sema/*.cpp src/support/*.h src/support/*.cpp)
add_executable(delta ${DELTA_SOURCES})

llvm_map_components_to_libnames(LLVM_LIBS core native linker support)
list(APPEND LLVM_LIBS clangAST clangBasic clangFrontend clangLex clangParse clangSema)
target_link_libraries(delta ${LLVM_LIBS})

add_custom_target(check_lit COMMAND lit --verbose --succinct --incremental ${EXTRA_LIT_FLAGS} ${PROJECT_SOURCE_DIR}/test
    -Ddelta_path="$<TARGET_FILE:delta>"
    -Dfilecheck_path="$<TARGET_FILE:FileCheck>"
    -Dnot_path="$<TARGET_FILE:not>"
    -Dtest_helper_scripts_path="${PROJECT_SOURCE_DIR}/test"
    USES_TERMINAL)
add_custom_target(check_examples COMMAND python "${PROJECT_SOURCE_DIR}/examples/build_examples.py" "$<TARGET_FILE:delta>")
add_custom_target(check)
add_custom_target(update_snapshots ${CMAKE_COMMAND} -E env UPDATE_SNAPSHOTS=1 cmake --build "${CMAKE_BINARY_DIR}" --target check)
add_dependencies(check check_lit check_examples)

if(NOT TARGET FileCheck)
    # Download the LLVM FileCheck utility for tests.
    set(FILECHECK_SRC_PATH "llvmorg-${LLVM_PACKAGE_VERSION}/llvm/utils/FileCheck/FileCheck.cpp")
    set(FILECHECK_SRC_URL "https://raw.githubusercontent.com/llvm/llvm-project/${FILECHECK_SRC_PATH}")
    set(FILECHECK_SRC_FILE "${CMAKE_CURRENT_BINARY_DIR}/${FILECHECK_SRC_PATH}")
    if(NOT EXISTS "${FILECHECK_SRC_FILE}")
        message(STATUS "Downloading LLVM FileCheck source")
        file(DOWNLOAD "${FILECHECK_SRC_URL}" "${FILECHECK_SRC_FILE}")
    endif()
    add_executable(FileCheck ${FILECHECK_SRC_FILE})
    llvm_map_components_to_libnames(FILECHECK_NEEDED_LIBS support)
    target_link_libraries(FileCheck PRIVATE ${FILECHECK_NEEDED_LIBS})
endif()

if(NOT TARGET not)
    # Download the LLVM 'not' utility for tests.
    set(NOT_SRC_PATH "llvmorg-${LLVM_PACKAGE_VERSION}/llvm/utils/not/not.cpp")
    set(NOT_SRC_URL "https://raw.githubusercontent.com/llvm/llvm-project/${NOT_SRC_PATH}")
    set(NOT_SRC_FILE "${CMAKE_CURRENT_BINARY_DIR}/${NOT_SRC_PATH}")
    if(NOT EXISTS "${NOT_SRC_FILE}")
        message(STATUS "Downloading LLVM 'not' source")
        file(DOWNLOAD "${NOT_SRC_URL}" "${NOT_SRC_FILE}")
    endif()
    add_executable(not ${NOT_SRC_FILE})
    llvm_map_components_to_libnames(NOT_NEEDED_LIBS support)
    target_link_libraries(not PRIVATE ${NOT_NEEDED_LIBS})
endif()

if(NOT DEFINED ENV{CI})
    if(MSVC)
        set_target_properties(FileCheck PROPERTIES COMPILE_FLAGS "${LLVM_DEFINITIONS} -w /O2 /GL")
        set_target_properties(not PROPERTIES COMPILE_FLAGS "${LLVM_DEFINITIONS} -w /O2 /GL")
    else()
        set_target_properties(FileCheck PROPERTIES COMPILE_FLAGS "${LLVM_DEFINITIONS} -w -O3")
        set_target_properties(not PROPERTIES COMPILE_FLAGS "${LLVM_DEFINITIONS} -w -O3")
    endif()
endif()

add_dependencies(check FileCheck not)

# Add 'coverage' target for code coverage report generation.
file(DOWNLOAD https://github.com/bilke/cmake-modules/raw/9dc1ac2dfb7ed7b0be80eec17127d857d702b2ed/CodeCoverage.cmake
    ${CMAKE_BINARY_DIR}/cmake-modules/CodeCoverage.cmake)
set(CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR}/cmake-modules)
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CodeCoverage)
    set(LCOV_REMOVE_EXTRA '*/v1/*')
    if(LCOV_PATH)
        setup_target_for_coverage(coverage "cmake --build ${CMAKE_BINARY_DIR} --target check" coverage)
    else()
        message(STATUS "lcov not found! Skipping 'coverage' target...")
    endif()
endif()

file(DOWNLOAD https://github.com/llvm/llvm-project/raw/master/clang-tools-extra/clang-tidy/tool/run-clang-tidy.py
    ${CMAKE_BINARY_DIR}/run-clang-tidy.py)
add_custom_target(lint COMMAND "${PROJECT_SOURCE_DIR}/scripts/format.sh" "${CMAKE_BINARY_DIR}" --check)
add_custom_target(format COMMAND "${PROJECT_SOURCE_DIR}/scripts/format.sh" "${CMAKE_BINARY_DIR}" --skip-clang-tidy)

if(NOT EXISTS "${CMAKE_BINARY_DIR}/cmake-modules/cotire.cmake")
    message(STATUS "Downloading 'cotire'")
    file(DOWNLOAD "https://github.com/sakra/cotire/raw/master/CMake/cotire.cmake" "${CMAKE_BINARY_DIR}/cmake-modules/cotire.cmake")
endif()
include(cotire)
cotire(delta)
