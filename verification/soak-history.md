# Soak history

One row per recorded batch. `docs/validation.md` defines the columns, the exit
criteria and how batches are sized; rows are appended by `soak-batch.yml`, which
commits only this file. A red row keeps its seed range so the batch can be
replayed.

| Date | Run | Platform | Shards | Minutes | Threads | Seeds | Ops | Entries | Verdict |
|---|---|---|---|---|---|---|---|---|---|
