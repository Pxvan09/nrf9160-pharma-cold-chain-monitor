# Host-side logic tests

Compiles and runs the pure-logic modules on the host with minimal Zephyr
stubs. No hardware or SDK required.

Catches:
- Record layout regressions (sizeof != 40)
- JSON malformation (unbalanced braces)
- Buffer overflow handling (-ENOMEM, never truncate)
- Excursion state machine correctness
- MKT calculation vs reference implementation

## Run

```bash
cd tests
gcc -I. -I../src -DCONFIG_PHARMA_LOG_LEVEL=3 \
    test_logic.c ../src/payload.c ../src/excursion.c \
    -o test_logic -lm
./test_logic
```

Expected: 18/18 PASS

## What it found

The missing `CONFIG_FPU=y` / `CONFIG_NEWLIB_LIBC=y` in prj.conf was
discovered here: `exp()` and `log()` (used for MKT) appeared as undefined
symbols at link time, invisible to code review.
