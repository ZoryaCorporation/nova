# Nova Changelog

## [0.2.1] — 2026-05-30

### CSV Parser Enhancements (NDP)

**Performance & Correctness:**
- **197x CSV parse speedup** — eliminated O(n²) GC marking during paused GC phase
- **Per-row hash pre-sizing** — `nova_table_new_hint()` eliminates 0→8→16→32 resize cascade per row (100k rows: 38.5s → 0.18s)
- **Custom integer parser** — bounded, zero-copy fast-path for integer fields; `strtod` only on genuine floats
- **File size cap raised** — 100MB → 2GB for large-scale ETL workloads

**Robustness & Data Handling:**
- **UTF-8 BOM stripping** — Windows/Excel CSV exports now work transparently (no manual cleaning)
- **Columnar output format** — `{columnar: true}` returns `{col: [v0,v1,...]}` instead of row-oriented arrays; zero per-row hash overhead
- **Column validation** — `{strict_cols: true}` (default) errors with exact row/column mismatch on ragged input
- **Whitespace trimming** — `{trim: true}` strips spaces around unquoted field values
- **Schema hints** — `{schema: "int,float,string,bool"}` per-column type specifications skip inference entirely

### Benchmarks

**CSV Parsing (100k rows × 18 columns):**
- Parse time: 0.18 sec
- Throughput: 552,233 rows/sec
- Columnar format: 270,646 rows/sec (with schema hints)

**Full Analytics Suite (bigdata.csv, 100k rows):**
- 13-column descriptive statistics: 0.35s (single-pass, all 4 moments)
- Pearson correlation matrix (8 cols): 0.57s
- Linear regression (5 pairs, 2 passes each): 0.16s
- Category aggregation: 0.08s
- Rolling window statistics: 0.04s
- Return series analysis (Sharpe/drawdown): 0.04s
- Multivariate anomaly detection (1.2M z-scores): 0.31s
- **Total analytics time: 51.5 sec (1,940 rows/sec throughput)**

### Internal Changes

- `nova_table_new_hint(vm, hash_capacity)` — new API for pre-sized tables
- `nova_ndp.c`: Added `NDP_SCHEMA_*` type codes (AUTO, INT, FLOAT, STRING, BOOL)
- `nova_ndp.c`: Added `ndp_parse_int_fast()` — custom bounded integer parser
- `nova_ndp.c`: Added `ndp_parse_schema()` — parses comma-separated type hints
- `nova_ndp.c`: Added `ndp_csv_push_typed_value_ex()` — schema-aware type inference
- `nova_trace.h/c`: Added `NOVA_TRACE_CH_NDP` debug channel (0x0800) for NDP profiling
- `nova_lib_data.c`: Updated `ndp_read_options()` to parse new CSV options

### API (Nova stdlib)

```nova
#import data

-- New CSV options
dec rows = csv.load("file.csv", {
    columnar    = true,          -- {col: [v,...]} format
    schema      = "int,float,string,int",  -- per-column types
    trim        = true,           -- strip field whitespace
    strict_cols = true,           -- error on col mismatch (default)
})

-- Columnar format example:
dec cols = csv.load("data.csv", {columnar = true})
-- Access: cols.price[0], cols.name[50], etc. (zero per-row hash overhead)

-- Schema hints example (fast path, no type inference):
dec fast = csv.load("data.csv", {schema = "int,float,float,string"})
-- Each column parsed directly to specified type or nil on mismatch
```

### Known Limitations

- **Sorting performance**: `table.sort()` comparator overhead visible on large datasets (100k values = 50s for 7 sorts). Numeric work (correlation, regression, anomaly detection) maintains 3.9M–6.25M ops/sec.
- **Memory**: Columnar format trades hash per-row overhead for array memory. For 100k × 18 columns, total VM heap ~40-50MB.

### Thanks

Massive thanks to the optimization work on ECC write barriers (hot_list coalescing) and the DAGGER hash table architecture — they made this speedup possible.
---

## [0.2.0] — 2026-04-15

Initial release. Full Nova language, VM, standard library, NDP codec system.

