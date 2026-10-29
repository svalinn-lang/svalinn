# Benchmark results

CPU seconds (user+sys), best of 3 runs (Python: 1 run), same machine (2 vCPU x86-64, Linux), every program's
output checked against the C version. Run with `bench/run.sh [runs] [langs...]`.

Compilers: `svalinnc -O` (this repo), gcc -O2 -march=native, rustc -C opt-level=3 -C target-cpu=native,
go build, OpenJDK (javac/java), node, CPython 3.

| bench      | svalinn -O0 | **svalinn -O** |     C |  Rust |    Go |  Java |  Node | Python |
|------------|----------:|-------------:|------:|------:|------:|------:|------:|-------:|
| fib        |     0.559 |    **0.218** | 0.101 | 0.126 | 0.219 | 0.214 | 0.744 |  6.897 |
| sieve      |     2.698 |    **1.292** | 1.038 | 0.885 | 1.284 | 1.001 | 1.429 | 24.690 |
| nbody      |     3.125 |    **0.468** | 0.307 | 0.288 | 0.519 | 0.702 | 0.769 | 31.000 |
| spectral   |     3.466 |    **0.342** | 0.196 | 0.199 | 0.268 | 0.728 | 0.624 | 36.145 |
| mandelbrot |     6.258 |    **1.800** | 1.269 | 1.447 | 1.578 | 1.558 | 1.603 | 43.337 |
| fannkuch   |     1.044 |    **0.229** | 0.203 | 0.249 | 0.237 | 0.354 | 0.435 |  7.398 |
| bintrees   |     2.751 |    **2.240** | 2.023 | 2.340 | 5.179 | 1.114 | 2.884 | 11.009 |
| matmul     |     2.976 |    **0.156** | 0.134 | 0.133 | 0.316 | 0.482 | 0.933 | 47.766 |

Geometric mean, relative to C (lower is better):

| svalinn -O0 | **svalinn -O** | C | Rust | Go | Java | Node | Python |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 6.13x | **1.40x** | 1.00x | 1.06x | 1.65x | 1.71x | 2.61x | 54x |

`-O` is 4.4x faster than `-O0` on average. Svalinn keeps every array bounds check that it cannot prove
redundant and never reassociates floating point, so results are bit-identical to `-O0`
(checked by `tests/fuzz`). Where the gap to C remains: fib (gcc turns one recursive call into a loop),
spectral/mandelbrot (gcc and LLVM use FMA via `-march=native` and vectorize the division; Svalinn stays on the
SSE2 baseline), sieve (bounds checks in the inner loop), bintrees (malloc-bound; Java's GC allocator wins).
