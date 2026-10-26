# matching-engine-cpp

A low latency limit order book matching engine in C++20. Price time priority, five order types, and no allocation on the hot path. It's tested event for event against a deliberately naive reference book.

This is a rewrite of my Java [order-book-engine](https://github.com/vaibenz/order-book-engine) with the same semantics but a different design. The two benchmarks share a bit for bit port of `java.util.Random`, so both engines replay the exact same 2 million command stream. On seed 42 they produce the same mix, the same 744,665 trades and the same 19,020,102 lots of volume, which is a nice cross language check that they agree.

It also reads the real thing. `itch_replay` streams a Nasdaq TotalView-ITCH 5.0 day file and rebuilds the full depth, order by order book of every stock on the exchange, and `mm_backtest` runs a passive market making strategy on those books with queue position and latency modelled.

* **ITCH 5.0:** zero copy parsing at about 95M messages a second, and books for every stock rebuilt at about 160 ns per message with millions of live orders.
* **Tested against the engine:** engines for 40 stocks publish an ITCH feed the way an exchange does, and the rebuilt books must match theirs (levels, quantities, order counts and queue order) after every message.
* **Backtest:** fills respect queue position and latency, and every fill is marked out at 100 ms, 1 s and 10 s to measure adverse selection.

## Market data: rebuilding Nasdaq's book from ITCH

```bash
curl -O "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"   # a full trading day, a few GB
./build/itch_replay 01302019.NASDAQ_ITCH50.gz --top 10
./build/mm_backtest 01302019.NASDAQ_ITCH50.gz --symbols AAPL,MSFT,AMZN,NVDA,TSLA --latency-us 50
```

ITCH never sends a book. It sends events: an order was added, part of it executed, part of it cancelled, it was deleted, it was replaced by a new order. Rebuilding the book means applying every event, for every stock at once, in order, which means tracking every live order on the exchange.

```
BookBuilder
 ├── RefIndex        order reference -> slot: pages of 64K entries, allocated and freed as orders come and go
 ├── orders_         slab of BookOrder, linked into one FIFO per price level by 32 bit indices
 └── books_[locate]  per stock: bids and asks as vectors sorted with the best price at the back
```

* **Paged direct index instead of a hash map.** Nasdaq hands out order references in increasing order through the day, so a reference maps straight to a slot through a page table. There's no hashing and no probing, and orders that arrived close together sit close together in memory. A page is freed when its last order goes.
* **Software prefetching.** Most messages touch an order that is not in cache, since millions are live. Messages arrive in large chunks, so while message *i* is applied, the index slot for message *i + 16* and the order for message *i + 8* are already being loaded.
* **Best price at the back.** Almost all activity is at or near the touch, so a level is found by scanning a few entries from the back of a sorted vector, and adding or removing a level there moves only a handful of elements.
* **Streaming.** Files are read in 16 MB chunks, gzip is decompressed on the fly with zlib, and a message split across two reads is carried over. The day never has to fit in memory.

Results on a 30 million message synthetic session (1,000 stocks, about 3 million live orders at the end; `itch_replay --synthetic`), 2 vCPU cloud VM:

| | |
|---|---|
| framing and parsing only | about 95M messages/s |
| full book rebuild with an open addressing hash map (the engine's `IdMap`) | 312 ns per message |
| paged direct index | 245 ns per message |
| plus two stage prefetch | **about 160 ns per message (6.2M messages/s)** |

A third prefetch stage for an order's queue neighbours was measured too and made no difference, so it isn't there.

### How it's tested

`tests/test_itch.cpp` checks the fields of every message type I write, the framer at every split point, and hand written scenarios for partial cancels, executions, replaces and deletes. The main test is differential. `SyntheticSession` runs one matching engine per stock and translates its events into ITCH the way an exchange does:

| engine event | ITCH |
|---|---|
| order rests | `A` or `F` with the resting quantity only |
| resting order is hit | `E` or `C` on the resting order (the aggressor gets nothing) |
| cancel | `D` |
| quantity cut at the same price | `X` for the difference |
| price change that doesn't trade | `U` with a new reference |
| price change that trades | `D`, then `E` per maker, then `A` for any remainder |

The builder sees only the feed. After every message batch, the stock that changed must match its engine: every level, quantity and order count, and the queue order of the top levels (the full queue every 16 steps). That runs for 3 seeds × 60,000 commands over 40 stocks. To check the test itself, I planted two bugs in the builder (ignoring partial cancels, and adding orders to the front of the queue), and each was caught within 70 commands.

## Market making backtest

`include/me/itch/backtest.hpp` quotes one lot at the best bid and one at the best ask, follows the touch, and stops adding to a side at an inventory limit. The strategy is deliberately simple. The point is the fill model, because that's where most backtests of passive strategies flatter themselves:

* **Queue position.** When the quote joins a level, every share already resting there is ahead of it, and those shares only go away when those exact orders (known by arrival sequence) trade or cancel. With nothing left ahead, an execution against an order that arrived after ours means the aggressor would have reached us first, so we fill instead. An execution at a worse price on our side means it swept through our level.
* **Latency.** Every quote, cancel and replace takes effect a configurable time after the decision, and the old quote can still be hit while it's being moved.
* **Markouts.** For each fill, the mid price 100 ms, 1 s and 10 s later shows how much of the fill was adverse selection.

The assumptions are stated, not hidden: our orders are too small to change anyone's behaviour, queues are strict price time (no hidden orders jump ahead), and the maker rebate is flat per share. `tests/test_backtest.cpp` pins the fill model on feeds where the answer can be worked out on paper (queue position, fills from behind, trade throughs, latency, the inventory limit, markouts). Planting a queue position bug or removing latency each breaks those tests.

On synthetic flow the P&L means nothing, because the flow is random. The real numbers come from running it on a Nasdaq day.

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

37 cases, no external test framework (`tests/check.hpp` is 70 lines). The ITCH and backtest tests are described above.

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
include/me/itch/            ITCH 5.0 messages, framer, writer, book builder, file reader, backtest
tools/itch_replay.cpp       rebuild every book from a day file, or write a synthetic session
tools/mm_backtest.cpp       market making backtest on a day file
bench/                      flow generator (java.util.Random port), benchmarks
tests/                      scenario, differential and fuzz tests
```

## What I'd add next

* Run the replay and the backtest on several real Nasdaq days and report the numbers here
* Inventory skew and spread conditions in the strategy, and fees beyond a flat rebate
* An exchange around the engine: binary order entry over TCP and market data over UDP multicast, with gap recovery from a journal

* Prefault and `mlock` the ladder and pool so the first commands don't take page faults
* Self trade prevention, and stop and iceberg orders
* A sliding price window, so the ladder covers only a band around the mid and memory doesn't grow with `max_price`
* Run the pipeline on isolated cores to get a real tail latency number

---

If something here is unclear, broken, or just wrong, feel free to open an issue. I'm always happy to chat about it.
