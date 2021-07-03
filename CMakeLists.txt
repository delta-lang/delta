cmake_minimum_required(VERSION 3.8)
project(cx)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_CXX_FLAGS "-w14062 -wd4244 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING ${CMAKE_CXX_FLAGS}")

    # Use static release version of Runtime Library to match prebuilt LLVM.
    # https://gitlab.kitware.com/cmake/community/wikis/FAQ#how-can-i-build-my-msvc-application-with-a-static-runtime
    foreach(flag_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/MDd" "/MT" ${flag_var} "${${flag_var}}")
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
    set(CMAKE_CXX_FLAGS "-fno-rtti -fvisibility-inlines-hidden -Wall -Wextra -Wno-unknown-pragmas -Wno-missing-braces -Wno-sign-compare ${CMAKE_CXX_FLAGS}")

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

find_package(LLVM 12)
find_package(Clang REQUIRED)
set(LLVM_PACKAGE_VERSION "${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}")
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
add_definitions(-DCX_ROOT_DIR="${PROJECT_SOURCE_DIR}")

# Define the include path of the Clang builtin headers, to be used by the compiler to avoid errors
# about missing C headers when importing C headers from C* code. For more information, see e.g.
# http://clang.llvm.org/docs/FAQ.html#i-get-errors-about-some-headers-being-missing-stddef-h-stdarg-h
# CLANG_CMAKE_DIR is defined by ClangConfig.cmake during the above find_package(Clang) call.
add_definitions(-DCLANG_BUILTIN_INCLUDE_PATH="${CLANG_CMAKE_DIR}/../../clang/${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}/include")

enable_testing()

file(GLOB_RECURSE CX_SOURCES src/*.h src/*.cpp)
add_executable(cx ${CX_SOURCES})

llvm_map_components_to_libnames(LLVM_LIBS core native linker support)
list(APPEND LLVM_LIBS clangAST clangBasic clangFrontend clangLex clangParse clangSema)
target_link_libraries(cx ${LLVM_LIBS})

add_custom_target(check_lit COMMAND lit --verbose --succinct --incremental ${EXTRA_LIT_FLAGS} ${PROJECT_SOURCE_DIR}/test
    -Dcx_path="$<TARGET_FILE:cx>"
    -Dfilecheck_path="$<TARGET_FILE:FileCheck>"
    -Dtest_helper_scripts_path="${PROJECT_SOURCE_DIR}/test"
    USES_TERMINAL)
add_custom_target(check_examples COMMAND python "${PROJECT_SOURCE_DIR}/examples/build_examples.py" "$<TARGET_FILE:cx>")
add_custom_target(check)
add_custom_target(update_snapshots ${CMAKE_COMMAND} -E env UPDATE_SNAPSHOTS=1 cmake --build "${CMAKE_BINARY_DIR}" --target check)
add_dependencies(check check_lit check_examples)

# Download the LLVM FileCheck utility for tests.
set(FILECHECK_SRC_PATH "llvmorg-${LLVM_PACKAGE_VERSION}/llvm/utils/FileCheck/FileCheck.cpp")
set(FILECHECK_SRC_URL "https://raw.githubusercontent.com/llvm/llvm-project/${FILECHECK_SRC_PATH}")
set(FILECHECK_SRC_FILE "${CMAKE_CURRENT_BINARY_DIR}/${FILECHECK_SRC_PATH}")
if(NOT EXISTS "${FILECHECK_SRC_FILE}")
    message(STATUS "Downloading LLVM FileCheck source")
    file(DOWNLOAD "${FILECHECK_SRC_URL}" "${FILECHECK_SRC_FILE}")
endif()
if(NOT TARGET FileCheck)
    add_executable(FileCheck ${FILECHECK_SRC_FILE})
    llvm_map_components_to_libnames(FILECHECK_NEEDED_LIBS support)
    target_link_libraries(FileCheck ${FILECHECK_NEEDED_LIBS})
endif()

if(MSVC)
    set_target_properties(FileCheck PROPERTIES COMPILE_FLAGS "${LLVM_DEFINITIONS} -w /O2 /GL /MT")
    set_target_properties(cx PROPERTIES COMPILE_FLAGS "/MT")
else()
    set_target_properties(FileCheck PROPERTIES COMPILE_FLAGS "${LLVM_DEFINITIONS} -w -O3")
endif()

add_dependencies(check FileCheck)

set(CMAKE_MODULE_PATH ${CMAKE_BINARY_DIR}/cmake-modules)

file(DOWNLOAD
    "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-${LLVM_PACKAGE_VERSION}/clang-tools-extra/clang-tidy/tool/run-clang-tidy.py"
    "${CMAKE_BINARY_DIR}/run-clang-tidy.py")
add_custom_target(lint COMMAND "${PROJECT_SOURCE_DIR}/scripts/format.sh" "${CMAKE_BINARY_DIR}" --check)
add_custom_target(format COMMAND "${PROJECT_SOURCE_DIR}/scripts/format.sh" "${CMAKE_BINARY_DIR}" --skip-clang-tidy)

if(NOT EXISTS "${CMAKE_BINARY_DIR}/cmake-modules/cotire.cmake")
    message(STATUS "Downloading 'cotire'")
    file(DOWNLOAD "https://github.com/sakra/cotire/raw/master/CMake/cotire.cmake" "${CMAKE_BINARY_DIR}/cmake-modules/cotire.cmake")
endif()
include(cotire)
cotire(cx)
