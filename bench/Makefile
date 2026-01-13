ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

include $(ROOT)/Makefile

.DEFAULT_GOAL := pgagg-bench

PGAGG_OBJECTS = bench/pgagg.o
HASHAGG_OBJECTS = bench/hashagg.o bench/hashagg_bench.o

ifneq ($(CURDIR),$(ROOT))
pgagg-bench:
	$(MAKE) -C $(ROOT) -f bench/Makefile pgagg-bench
.PHONY: pgagg-bench

hashagg-bench:
	$(MAKE) -C $(ROOT) -f bench/Makefile hashagg-bench
.PHONY: hashagg-bench
else
pgagg-bench: CC = gcc
pgagg-bench: CFLAGS = $(RELEASE_CFLAGS)
pgagg-bench: $(PGAGG_OBJECTS) $(APP_COMMON) obj
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).pgagg.bench $(CORE_OBJECTS) $(APP_COMMON) $(PGAGG_OBJECTS) $(LIBS) $(LDFLAGS)
	./$(TARGET).pgagg.bench

.PHONY: pgagg-bench

hashagg-bench: CC = gcc
hashagg-bench: CFLAGS = $(RELEASE_CFLAGS)
hashagg-bench: $(HASHAGG_OBJECTS) $(APP_COMMON) obj
	$(CC) -include core/def.h $(CFLAGS) -o $(TARGET).hashagg.bench $(CORE_OBJECTS) $(APP_COMMON) $(HASHAGG_OBJECTS) $(LIBS) $(LDFLAGS)
	./$(TARGET).hashagg.bench

.PHONY: hashagg-bench
endif
