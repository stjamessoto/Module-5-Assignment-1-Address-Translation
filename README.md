# CS375 A9 – Advanced Segmented, Paged Memory Model Simulator

A C++17 simulator of a **segmented, two-level paged** virtual memory system. It covers
memory protection, demand paging with **FIFO / LRU** replacement, runtime and file-based
configuration, stress testing, batch processing, error logging, simulated latency, a
Gantt-like timeline and a text memory map.

---

## Folder layout

```
Module-5-Assignment-1-Address-Translation/
├── README.md                  <- you are here
├── Makefile                   <- make / make demo / make report / make clean
├── submission/                <- UPLOAD EVERYTHING IN HERE TO CANVAS
│   ├── memory_simulator.cpp   <- the program (single file, no dependencies)
│   ├── report.pdf             <- written report: 15 answers, results, screenshots
│   ├── results.txt            <- random address (stress) test output
│   ├── errors.log             <- detailed fault reasons from the stress test
│   ├── batch_results.txt      <- batch file output
│   ├── latency_history.csv    <- average latency across runs
│   ├── sample_config.txt      <- memory configuration used for all screenshots
│   ├── batch_addresses.txt    <- batch input
│   ├── sparse_config.txt      <- sparse segment, shows the two-level table saving
│   └── sparse_batch.txt       <- batch input for sparse_config.txt
├── report/
│   ├── report.tex             <- LaTeX source of report.pdf
│   └── screenshots/           <- 00_build.png ... 11_two_level.png
└── build/                     <- compiled binary (not submitted, git-ignored)
```

### What to submit

Upload **every file in `submission/`** (10 files):

| File | Why |
|------|-----|
| `memory_simulator.cpp` | the program (required) |
| `report.pdf` | answers to all 15 questions, test results, screenshots, references (required) |
| `results.txt` | random address test output (required) |
| `errors.log`, `batch_results.txt`, `latency_history.csv` | logs the assignment asks the program to save (error logging, batch summary, latency across runs) |
| `sample_config.txt`, `batch_addresses.txt`, `sparse_config.txt`, `sparse_batch.txt` | input files, so the grader can reproduce your runs |

Don't upload `build/` or `report/`. The screenshots are already inside `report.pdf`.

---

## Quick start

```bash
cd ~/Module-5-Assignment-1-Address-Translation
make                      # compiles to build/memory_simulator
make demo                 # runs with sample_config.txt and seed 42
```

Without `make`:

```bash
cd submission
g++ -std=c++17 -Wall -O2 -o memory_simulator memory_simulator.cpp
./memory_simulator                                   # interactive setup
./memory_simulator --seed 42 --config sample_config.txt
```

Command-line options:

| Option | Meaning |
|--------|---------|
| `--seed N` | fixed random seed, so the same run gives the same output |
| `--config FILE` | load memory configuration from a file instead of prompting |
| `--color` / `--no-color` | force or disable colored output |

Run the program **from inside `submission/`**. Output files are written to the current folder.

---

## Using the simulator

On start-up you either load a config file or answer prompts (press **Enter** to accept the
default in `[brackets]`):

```
Number of physical frames [16]:
Page size in bytes [1000]:
Number of segments [4]:
Pages per page-directory entry (2nd-level table size) [4]:
Replacement policy FIFO or LRU [LRU]:
```

The configuration is then validated and the main menu appears:

| # | Action | Assignment item |
|---|--------|-----------------|
| 1 | Translate addresses manually | Part 3 simulation, protection |
| 2 | Stress test → `results.txt` | Challenge 5 |
| 3 | Batch file → `batch_results.txt` | Challenge 7 |
| 4 | Memory map | Challenge 10 |
| 5 | Metrics + latency history | Challenges 6, 8 |
| 6 | Translation timeline (Gantt) | Part 3 output |
| 7 | Compare FIFO vs LRU | Challenge 2, Q9 |
| 8 | Page-size analysis (1000 vs 4000) | Q11 |
| 9 | Change replacement policy | Challenge 2 |
| 10 | Reconfigure memory | Challenge 4 |

### Logical address format

```
seg dir page offset access        e.g.   1 0 2 250 R
```

`access` is `R` / `W`, or `0` = read, `1` = write. Commas and parentheses are fine too:
`(1, 0, 2, 250, W)`. In manual mode, type `m` to show the memory map and `q` (or `-1`) to go back.

### Translation steps

1. **Segment table**: is the segment valid? → otherwise *Segmentation fault*
2. **Bounds**: directory index, page number (segment limit) and offset → *Directory fault*, *Limit violation*, *Offset fault*
3. **Segment protection**: write to a read-only segment → *Protection violation*
4. **Page directory**: an empty entry gets a new page table on demand
5. **Page table**: write to a read-only page → *Protection violation*; present bit 0 → **page fault**: load into a free frame, or evict a victim with FIFO/LRU
6. **Physical address** = `frame × page_size + offset`

Latency: random **1–5 units** per translation, plus **10 units** when a page fault is serviced.

### Config file format (`sample_config.txt`)

```
frames 7
page_size 1000
pages_per_dir 4
policy LRU
# segID  base    limit  protection  pages...
0        0x1000  6      RO          2:RO 5:RO -:RO -:RO
1        0x2000  8      RW          0:RW -:RW 6:RW -:RO 3:RW -:RW
```

- `base` is where the segment's page directory lives (decimal or hex).
- Page token `frame:prot`. Use `-` for a page that is not present. If you leave out the protection, the page takes the segment's protection.
- Directories with no listed pages start **unallocated**.
- The loader rejects: out-of-range sizes, non-sequential segment IDs, more page tokens than the limit, invalid or duplicate frames, and overlapping page directories. Each error includes its line number.

### Output files

| File | Contents |
|------|----------|
| `results.txt` | per-address stress-test log `[start-end] (s,d,p,off,acc) outcome latency` + summary |
| `errors.log` | every fault with its reason, e.g. `Invalid segment 5, max 2`, plus serviced page faults |
| `batch_results.txt` | batch file results + summary |
| `latency_history.csv` | one row per run; the metrics screen shows the average across all runs |

---

## Screenshots and report

The screenshots are in `report/screenshots/`. `report/report.tex` inserts each one into its
section automatically. A missing file shows up as a labelled placeholder box, so the report
always builds.

| File | Shows | Command (run from `submission/`) |
|------|-------|----------------------------------|
| `00_build.png` | clean compile | `cd .. && make clean && make && cd submission` |
| `01_manual_translations.png` | 5 manual translations (required) | (01) below |
| `02_memory_map.png` | memory map after translations (required) | (02) below |
| `03_stress_summary.png` | stress test summary | (03) below |
| `04_results_head.png` | start of `results.txt` (required) | `head -25 results.txt` |
| `05_results_summary.png` | end of `results.txt` (required) | `tail -17 results.txt` |
| `06_errors_log.png` | fault reasons | `head -25 errors.log` (right after 03) |
| `07_batch.png` | batch processing | (07) below |
| `08_timeline.png` | Gantt timeline | (08) below |
| `09_fifo_vs_lru.png` | FIFO vs LRU | (09) below |
| `10_page_size.png` | page size 1000 vs 4000 | (10) below |
| `11_two_level.png` | sparse segment, two-level saving | (11) below |

```bash
# (01)
printf '1\n1 0 2 250 R\n0 0 1 999 W\n2 1 2 40 W\n5 0 0 0 R\n1 1 3 100 R\nq\n0\n' | ../build/memory_simulator --seed 42 --config sample_config.txt --color | sed -n '/MANUAL ADDRESS/,/Average latency/p'
# (02)
printf '1\n1 0 2 250 R\n0 0 1 999 W\n2 1 2 40 W\n5 0 0 0 R\n1 1 3 100 R\nq\n4\n0\n' | ../build/memory_simulator --seed 42 --config sample_config.txt --color | sed -n '/MEMORY MAP/,/single-level/p'
# (03)  creates results.txt and errors.log
printf '2\n100\n0.7\n0\n' | ../build/memory_simulator --seed 42 --config sample_config.txt --color | sed -n '/STRESS TEST/,/errors.log/p'
# (07)
printf '3\nbatch_addresses.txt\n0\n' | ../build/memory_simulator --seed 42 --config sample_config.txt --color | sed -n '/BATCH PROCESSING/,/batch_results.txt/p'
# (08)
printf '3\nbatch_addresses.txt\n6\n13\n0\n' | ../build/memory_simulator --seed 42 --config sample_config.txt --color | sed -n '/TRANSLATION TIMELINE/,/#13 /p'
# (09)
printf '7\n1000\n0\n' | ../build/memory_simulator --seed 42 --config sample_config.txt --color | sed -n '/FIFO vs LRU COMPARISON/,/unchanged/p'
# (10)
printf '8\n1000\n4000\n2000\n16000\n0\n' | ../build/memory_simulator --seed 42 --config sample_config.txt --color | sed -n '/PAGE-SIZE ANALYSIS/,/unchanged/p'
# (11)
clear; printf '3\nsparse_batch.txt\n5\n0\n' | ../build/memory_simulator --seed 42 --config sparse_config.txt --color | sed -n '/BATCH PROCESSING/,/Page-table entries/p'
```

Every run of the program starts a new `errors.log`. Run `clear` before each command so a
screenshot shows only that command's output. Screenshots are taken on Windows with
**Win + Shift + S**. To open the folder in Explorer:
`explorer.exe "$(wslpath -w ~/Module-5-Assignment-1-Address-Translation/report/screenshots)"`

To rebuild the PDF after changing a screenshot or `report.tex`:

```bash
cd ~/Module-5-Assignment-1-Address-Translation && make report
```

This writes `report/report.pdf` and copies it to `submission/report.pdf`. Your name goes on
the `\newcommand{\studentname}{...}` line near the top of `report/report.tex`.

**Cropping.** `\screenshot` takes optional graphics options, e.g.
`\screenshot[width=\linewidth,trim={0 0 400 10},clip]{file.png}{caption}{label}`. The screenshots
are 144 dpi, so a trim of 1 bp removes 2 pixels. Figures 01, 02 and 11 are trimmed this way to
remove empty space and stray output.

---

## Notes and changes from the starter code

- **Physical address** is `frame × page_size + offset`. The starter also added the segment base,
  which relocated twice and made segments overlap. Here `base_address` locates the segment's page directory.
- The starter's page-fault handler set the page's protection to the **access type**. A write fault
  could therefore turn a read-only page into a writable one. Protection is now a fixed property of
  the page and is checked **before** a fault is serviced.
- The starter gave initial pages `rand() % 100` as frame numbers, which ignores the real frame pool.
  Pages are now placed only in real, free frames and every frame has a recorded owner.
- Added: two-level `DirectoryTable`, LRU, owner-aware eviction, validation, batch/stress/timeline,
  error log, latency history, FIFO vs LRU and page-size experiments.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `make: command not found` | `sudo apt install build-essential` |
| `pdflatex: command not found` | `sudo apt install texlive-latex-extra` |
| Colors look like `^[[32m` | add `--no-color` |
| `Cannot open 'sample_config.txt'` | `cd submission` first |
| Different numbers from the report | use `--seed 42` and start with a fresh run |
