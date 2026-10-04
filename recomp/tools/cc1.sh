#!/bin/sh
# cc1.sh <outdir> <file.c> : compile one generated file (called in parallel by build.sh)
HERE="$(cd "$(dirname "$0")/.." && pwd)"
exec clang -c -O2 -gline-tables-only -ffp-contract=off -fno-strict-aliasing -I "$HERE/runtime" -I "$1" -o "$1/obj/$(basename "$2" .c).o" "$2"
