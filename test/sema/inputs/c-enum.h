// RUN: %cx -print-llvm -Iinputs %s | %FileCheck %s

typedef enum {
    FOO = 42
} Foo;
