# Running Several Instances

Several rayforce processes can run on one machine. Each process sizes its thread pool and its memory for what it can see, and it does not know about the others. Give every instance its own share of CPUs and memory, its own port and journal, and keep each database written by one process at a time.

## CPUs

Choose the CPUs of each instance when you start it: `taskset -c LIST`, `docker run --cpuset-cpus=LIST`, or `CPUAffinity=` in a systemd unit. Every thread of the instance, and every program it starts with `.sys.exec`, stays within that list.

On Linux an instance sizes its thread pool to its list: one thread per CPU of it, the main thread included. The operating system keeps the threads within the list and moves them between its CPUs as it sees fit; rayforce does not tie a thread to one CPU. On other platforms the pool has one thread per online CPU.

```bash
taskset -c 0,2,4,6,24,26,28,30 rayforce -p 5001 &
taskset -c 8,10,12,14,32,34,36,38 rayforce -p 5002 &
```

**Choosing the lists.** Read the machine's layout from `lscpu -e` (columns `NODE`, `SOCKET`, `CORE`), or from `/sys/devices/system/cpu/cpuN/topology/thread_siblings_list` and `/sys/devices/system/node/nodeN/cpulist`.

- Give each instance whole cores: both hardware threads of a core go to the same instance. Two instances on the two threads of one core slow each other down.
- Keep an instance on one NUMA node when its CPU count fits in one.
- CPU numbering does not follow the layout on every machine. On a two-socket machine that numbers sockets alternately (even CPUs on socket 0, odd on socket 1) with CPU `k` and `k+24` on one core, the ranges `0-7`, `8-15`, … `40-47` put every instance on both sockets, and the last three ranges on the hyperthreads of the first three. For six instances of 8 threads on that machine use `0,2,4,6,24,26,28,30`, `8,10,12,14,32,34,36,38`, `16,18,20,22,40,42,44,46`, `1,3,5,7,25,27,29,31`, `9,11,13,15,33,35,37,39` and `17,19,21,23,41,43,45,47`.

**CPU quota.** `docker run --cpus=N` and systemd `CPUQuota=` limit CPU time, not CPUs: the instance may still run on every CPU. Its default pool then has one thread per CPU of the quota, rounded up. Give it `--cpuset-cpus` or `CPUAffinity=` as well to choose its CPUs.

**Do not:**

- start two instances on overlapping lists: they share those CPUs;
- start instances without a list and leave each at its default size: every instance takes every CPU;
- pass `-c` larger than the list: the threads take turns on its CPUs (rayforce prints a warning).

`(.sys.info)` reports the CPUs of the instance (`cores`) and its pool size (`workers`); `taskset -p PID` shows its list.

## Memory

Each instance treats the machine's whole physical RAM, or the memory limit of its container or cgroup when one is set, as its own. It moves its working set to disk only when its own memory would pass that amount, so several busy instances without limits of their own can together exhaust RAM before any of them spills. Start every instance with `-m SIZE` and keep the sizes, plus room for the page cache, within RAM:

```bash
taskset -c 0-11 rayforce -m 40G -p 5001 &
taskset -c 12-23 rayforce -m 40G -p 5002 &
```

`-m` also bounds the freed memory an idle instance keeps for reuse (a sixteenth of SIZE).

Instances that read the same tables share the operating system's page cache, so the sum of their resident sizes overstates the memory in use.

## Ports, journals and spill files

- **Port:** each instance listens on its own `-p` port. A second instance on a busy port exits with an error.
- **Journal:** each instance needs its own journal base (`-l /path/instance1`). Two processes appending to one journal break its framing, and recovery stops at the first broken entry. Never run `.log.roll`, `.log.snapshot` or `.log.purge` on a journal another process writes.
- **Spill files:** names carry the process id and are removed as soon as they are mapped, so instances never share one. Point `RAY_HEAP_SWAP` at a disk with room for every instance that may spill at once.

## Sharing a database

**Readers.** Any number of instances can read the same splayed or partitioned tables. A reader sees either the whole previous version of a table or the whole new one, and a table it has already loaded stays valid when another process rewrites it.

**One writer per database.** Only one process at a time may write a database: `.db.splayed.set`, a `.csv` or `.parquet` import into an existing root, or a live `insert` into a partitioned table followed by its rollover. All tables and partitions of a database share the root's symbol file.

- When two processes write the same database, a write that races another one fails with a `corrupt` error. Nothing on disk is damaged. If the failing process holds tables it loaded from that database, its later reads and writes of the database fail until it drops them.
- Two processes that update one table by reading it, changing it and writing it back lose each other's updates; the last write wins.
- A partitioned reader fails while a new partition is being written, because the partition directory exists before its schema does. Write a new partition under a dot directory of the root, passing the root's symbol file, and rename it into place; dot directories are skipped when the partitions are listed:

```lisp
(.db.splayed.set "/db/.stage/2026.10.02/t/" t "/db/.sym")
(.sys.exec "mv /db/.stage/2026.10.02 /db/2026.10.02")
```

**Files.** Never change table files in place: no `cp` over them, no `rsync --inplace`, no `truncate`, no editors. Instances that have the table open see the change at once, its stored statistics no longer match the data, and a shortened file crashes them with `SIGBUS`. Replace tables with `.db.splayed.set`, or write a new file and rename it over the old one. The same applies to files written with `write`, `write-bytes` and `hnsw-save`, which rewrite their target in place.

**Versions.** All instances that share a database must run the same rayforce version; upgrade them together.

## Interactive sessions

The REPL rewrites `~/.rayforce_history` when it exits, so with several interactive sessions only the history of the last one to exit is kept.
