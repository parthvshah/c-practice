# C Systems Programming Practice

Interview prep for systems/kernel engineering roles. Problems focus on day-to-day systems C — data structures, memory management, parsing, and bit manipulation.

## Build

```sh
make        # build ./main
make clean  # remove binary and object files
```

Requires a C11-compatible compiler (`cc`). No external dependencies beyond libc.

## Structure

| File | Description |
|---|---|
| `main.c` | Test driver for all implemented modules |
| `problems.md` | Problem set with requirements and talking points |
| `solutions.md` | Reference solutions |

## Problems

| # | Topic | Time |
|---|---|---|
| 1 | Ring buffer (kernel event log) | 25 min |
| 2 | `/proc/meminfo` parser | 20 min |
| 3 | CPU mask / bitmap operations | 20 min |
| 4 | Reference-counted object (`kref`) | 20 min |
| 5 | Find the bug (dynamic array) | 15 min |
| 6 | Hash table with separate chaining | 30 min |
| 7 | Fixed-size memory pool allocator | 25 min |
| 8 | Process exec argument builder | 15 min |

See `problems.md` for full requirements and `solutions.md` for reference implementations.
