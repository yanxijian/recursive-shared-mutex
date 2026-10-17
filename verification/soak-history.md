# Soak history

One row per recorded batch. `docs/validation.md` defines the columns, the exit
criteria and how batches are sized; rows are appended by `soak-batch.yml`, which
commits only this file. A red row keeps its seed range so the batch can be
replayed.

| Date | Run | Platform | Shards | Minutes | Threads | Seeds | Ops | Entries | Verdict |
|---|---|---|---|---|---|---|---|---|---|
| 2026-10-17 | 39064917802 | linux | 2 | 2 | 4 | 1000-1003 | 1466298676 | 151236596 | green |
