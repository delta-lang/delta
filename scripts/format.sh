#!/usr/bin/env bash

set -o pipefail

ROOTDIR=$(cd "$(dirname "$0")/.."; pwd)
FILES=$(echo $ROOTDIR/src/**/*.{h,cpp})
CLANG_TOOLS_VERSION=12

while test $# -gt 0; do
    case "$1" in
        --check) CHECK=1
            ;;
        --skip-clang-tidy) SKIP_CLANG_TIDY=1
            ;;
        --*) echo "bad option $1"
            ;;
        *) BUILD_PATH="$1"
            ;;
    esac
    shift
done

check_version() {
    if ! $1 --version | grep --quiet $CLANG_TOOLS_VERSION; then
        echo "WARNING: Wrong $1 version, expected $CLANG_TOOLS_VERSION."
    fi
}

run_clang_tidy() {
    if [ -n "$SKIP_CLANG_TIDY" ]; then
        return 0
    fi
    python "$BUILD_PATH/run-clang-tidy.py" -header-filter="^$ROOTDIR/src/.*" -quiet "$@" $FILES 2>&1 \
        | sed -E '/^($|clang-tidy|[0-9]+ warnings generated)/d'
}

run_clang_format() {
    ! (clang-format -output-replacements-xml $FILES | grep "<replacement " >/dev/null)
}

check_version clang-format
check_version clang-tidy

if [ -n "$CHECK" ]; then
    run_clang_tidy && run_clang_format

    if [ $? -ne 0 ]; then
        echo "Run the 'format' target to format the code."
        exit 1
    else
        exit 0
    fi
fi

run_clang_tidy -fix -format
clang-format -i $FILES
