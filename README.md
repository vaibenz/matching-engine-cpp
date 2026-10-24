# matching-engine-cpp

A low latency limit order book matching engine in C++20. Price time priority, five order types, and no allocation on the hot path. It's tested event for event against a deliberately naive reference book.

This is a rewrite of my Java [order-book-engine](https://github.com/vaibenz/order-book-engine) with the same semantics but a different design. The two benchmarks share a bit for bit port of `java.util.Random`, so both engines replay the exact same 2 million command stream. On seed 42 they produce the same mix, the same 744,665 trades and the same 19,020,102 lots of volume, which is a nice cross language check that they agree.

## Design

```
OrderBook<Listener>
 ├── bids_, asks_     : vector<Level>   one level per tick, indexed by price ("price ladder")
 ├── bid_bits_, ...   : PriceBitmap     1 bit per price + 1 summary bit per 64 words
 ├── pool_            : OrderPool       slab of Order structs, intrusive free list
 └── ids_             : IdMap           order id -> pool index, open addressing

Level:  head ⇄ order ⇄ order ⇄ tail    FIFO, 32 bit index links, total qty, count
```

* **Price ladder.** Finding the level for a price is one array index, not a tree walk. Prices are integer ticks in `1..max_price`, so matching never touches floating point.
* **Two level bitmap** for "next non empty level above/below p". When the touch empties, the new best is a couple of `ctz`/`clz` instructions per 4,096 ticks instead of a tick by tick scan, so a sparse book costs about the same as a dense one. It's also used for depth and FOK liquidity checks.
* **Order pool.** Orders live in one slab and link to each other with 32 bit indices (half the size of pointers, and the slab can grow without invalidating anything). After warmup nothing on the hot path calls the allocator.
* **Flat id map.** Open addressing with linear probing, Fibonacci hashing (order ids are often sequential) and backward shift deletion, so heavy cancel traffic never leaves tombstones behind.
* **Listener as a template parameter.** Events are delivered through static dispatch, so with the default `NullListener` the whole event path compiles away.
* **Single threaded core,** like most exchange matching engines. Commands are serialised onto one thread, which keeps matching deterministic and lock free. `bench/pipeline.cpp` shows the usual way to feed it: a gateway thread writes 32 byte wire commands into a lock free SPSC ring.

### Order types and commands

| Type | Behaviour |
|---|---|
| `Limit` | matches up to its price, remainder rests |
| `Market` | matches at any price, remainder cancelled |
| `IOC` | matches up to its price, remainder cancelled |
| `FOK` | fills completely up to its price or does nothing (book untouched) |
| `PostOnly` | rests, rejected if it would take liquidity |

`submit`, `cancel` and `modify`. Reducing quantity at the same price keeps queue priority. A price change or a quantity increase loses priority, and the order may trade immediately if it now crosses. Trades always print at the resting order's price.

## Testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/tests
```

26 cases, no external test framework (`tests/check.hpp` is 70 lines).

* **Scenario tests** cover price time priority, partial fills, sweeps, every order type, cancels from the middle of a queue, the modify priority rules and input validation.
* **Differential test.** 300 random streams of 2,000 commands go through both the real book and `ReferenceBook`, which keeps one flat vector and linear scans everything. Events must match after every command, and depth must match every 16 commands. The streams mix every order type with bad prices, bad quantities, duplicate ids, and cancels and modifies on live, filled and unknown orders.
* **Structure fuzz tests** check `IdMap` against `std::unordered_map` and `PriceBitmap` against `std::set`, including word and summary boundaries.
* **SPSC ring:** 2 million values through a 64 slot ring across two threads, every value checked in order. Also run under ThreadSanitizer.
* **Sanitizers:** `-DME_SANITIZE=ON` builds the tests with ASan and UBSan. CI runs gcc and clang in Debug and Release, plus the sanitizer build.

**A test that didn't catch a bug.** I planted a one lot error in the level total on same price modifies to check the differential test would catch it, and it didn't. Target ids were drawn uniformly from every id ever used, so about 98% of modifies hit dead orders. Depth was also only compared at the end of a stream, by which time the bad level had usually emptied. Now targets are biased toward recent orders, some modifies deliberately keep their price, and depth is compared during the stream. The planted bug is caught at seed 14.

## Benchmark

```bash
./build/bench 2000000 42      # single thread, ladder vs std::map baseline
./build/pipeline 2000000 42   # two threads through the SPSC ring
```

The flow is the Java engine's: about 55% limit orders near the touch, 25% cancels, 10% modifies, 7% IOC and 3% market, around a random walk mid price. It's generated up front, so random number generation isn't timed. Latency is measured per command with `rdtscp`. The timer itself costs about 21 ns and is **not** subtracted.

`MapBook` is the textbook design (`std::map` of levels, a `std::list` per level, `std::unordered_map` from id to iterator). It passes the same differential test and exists as a baseline.

Results on a 2 vCPU cloud VM (Xeon @ 2.1 GHz, Firecracker), pinned to one core, three runs:

| 2M commands, seed 42 | throughput (M cmd/s) | p50 | p99 | p99.9 |
|---|---|---|---|---|
| **this engine** | 32 to 34 | 74 to 87 ns | 190 to 245 ns | 390 to 430 ns |
| `std::map` baseline | 10.6 to 11.2 | 118 to 127 ns | 390 to 430 ns | 650 to 710 ns |
| Java engine (same VM, JDK 21) | 9.3 | 129 ns | 435 ns | 774 ns |

How it got there, on the same flow:

| step | throughput |
|---|---|
| ladder book with `std::unordered_map` for ids | 20.7M cmd/s |
| open addressing id map | 35.9M |
| two level bitmap | unchanged on this flow (see below) |

The bitmap doesn't help the dense flow, because levels near the touch are all occupied and a linear scan finds the next one in a step or two. It matters on a **sparse book**. In the bench's sparse case, asks sit 1,000 ticks apart and every command moves the touch. The tick scan managed 0.17M cmd/s with a 27 µs p99. With the bitmap it does about 41M cmd/s with a p99 around 150 to 340 ns.

On that sparse case the `std::map` book has lower throughput (17 to 26M) but a slightly *better* p50 and p99 latency than the ladder. With only 16 live levels spread across a large array, every level the ladder touches is its own cache line, while the map's 16 nodes stay hot. A price ladder trades memory for speed, and the trade is best when the book is dense around the touch, which is the normal case for liquid instruments.

**Two threads through the ring** (`pipeline`):
* **Flood:** about 26M cmd/s end to end, with the same 744,665 trades.
* **Paced** at one command per µs: one way latency from gateway push to command applied is about 420 to 570 ns at p50.

The tail is not meaningful on this machine. p99 swings from 7 µs to 350 µs between runs, because the VM shows non zero steal time and the hypervisor deschedules a vCPU every so often. Measuring it properly needs isolated cores.

The maximum per command latency (tens of µs to about 1 ms) is the same story, plus first touch page faults on a freshly constructed book.

## Files

```
include/me/order_book.hpp   the book
include/me/price_bitmap.hpp next level search
include/me/id_map.hpp       open addressing id -> index
include/me/order_pool.hpp   order slab + free list
include/me/spsc_ring.hpp    lock free single producer / single consumer queue
include/me/wire.hpp         32 byte binary command
include/me/map_book.hpp     std::map baseline
bench/                      flow generator (java.util.Random port), benchmarks
tests/                      scenario, differential and fuzz tests
```

## What I'd add next

* Prefault and `mlock` the ladder and pool so the first commands don't take page faults
* Self trade prevention, and stop and iceberg orders
* A sliding price window, so the ladder covers only a band around the mid and memory doesn't grow with `max_price`
* Run the pipeline on isolated cores to get a real tail latency number

---

If something here is unclear, broken, or just wrong, feel free to open an issue. I'm always happy to chat about it.
