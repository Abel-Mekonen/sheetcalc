#!/bin/bash -eu

# Build the sheetcalc library
$CC $CFLAGS -c -I$SRC/include \
    $SRC/src/core.c \
    $SRC/src/parser.c \
    $SRC/src/eval.c \
    $SRC/src/builtins.c \
    $SRC/src/value.c \
    $SRC/src/buf.c \
    $SRC/src/formula.c \
    $SRC/src/validate.c \
    $SRC/src/convert.c \
    $SRC/src/format.c \
    $SRC/src/util.c \
    $SRC/src/io.c \
    $SRC/src/checksum.c \
    $SRC/src/refgraph.c \
    $SRC/src/depsolve.c \
    $SRC/src/pivot.c \
    $SRC/src/sort.c \
    $SRC/src/stats.c

ar rcs libsheetcalc.a *.o

# Build each fuzzer
for f in $SRC/fuzz/*_fuzzer.cc; do
    name=$(basename "$f" .cc)
    $CXX $CXXFLAGS -I$SRC/include \
        "$f" \
        -o $OUT/$name \
        $LIB_FUZZING_ENGINE libsheetcalc.a -lm
done

# Package per-harness seed corpora
for d in $SRC/fuzz/corpus/*/; do
    tgt=$(basename "$d")
    if [ -d "$d" ]; then
        zip -j $OUT/${tgt}_seed_corpus.zip "$d"/* 2>/dev/null || true
    fi
done

# Copy dictionary
if [ -f "$SRC/fuzz/dictionary.txt" ]; then
    for f in $SRC/fuzz/*_fuzzer.cc; do
        name=$(basename "$f" .cc)
        cp "$SRC/fuzz/dictionary.txt" "$OUT/${name}.dict"
    done
fi
