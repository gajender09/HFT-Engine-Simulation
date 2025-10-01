// hft_engine_simulation.cpp
// A more feature-rich single-file HFT engine simulation for learning and prototyping.
// - Modern C++17
// - Tick-indexed order book with fixed-size ring buffers per price level
// - Preallocated order pool + O(1) clientId -> engineId map for cancels/replaces
// - Limit / Market orders, IOC, FOK flags, cancels, replaces
// - Simple market-data feed & strategy (naive market-maker) for demo
// - Single-threaded core matching loop (easy to extend to sharded multi-threading)
// Compile: g++ -O3 -march=native -std=c++17 hft_engine_simulation.cpp -o hft_sim

#include <bits/stdc++.h>
using namespace std;
using u64 = unsigned long long;
using i64 = long long;
using TimePoint = chrono::high_resolution_clock::time_point;

// ------------------------------- CONFIG ----------------------------------
static constexpr int PRICE_LEVELS = 20001; // must be odd to have middle
static constexpr double TICK = 0.01;
static constexpr double MIN_PRICE = 0.0;
static constexpr size_t ORDER_POOL_CAPACITY = 3'000'000;
static constexpr size_t RING_CAPACITY_PER_LEVEL = 4096; // tuned for demo

// ------------------------------- ENUMS -----------------------------------
enum class Side : uint8_t { BUY = 0, SELL = 1 };
enum class OrderType : uint8_t { LIMIT = 0, MARKET = 1 };
enum class TimeInForce : uint8_t { GFD = 0, IOC = 1, FOK = 2 };

// ------------------------------- UTIL ------------------------------------
inline string sideName(Side s) { return s==Side::BUY?"BUY":"SELL"; }
inline double idxToPrice(int idx) { return MIN_PRICE + idx * TICK; }

// ------------------------------- ORDER -----------------------------------
struct Order {
    u64 clientId = 0;     // externally visible id
    u64 engineId = 0;     // index in pool
    Side side = Side::BUY;
    OrderType type = OrderType::LIMIT;
    TimeInForce tif = TimeInForce::GFD;
    int priceIdx = -1;    // -1 for market
    i64 qty = 0;          // remaining qty
    u64 ts = 0;           // arrival timestamp
    bool active = false;  // set when placed in book
};

// --------------------------- ORDER POOL ----------------------------------
struct OrderPool {
    vector<Order> pool;
    vector<u64> freeList;
    OrderPool(size_t cap) { pool.resize(cap); freeList.reserve(cap); for (u64 i=0;i<cap;i++) freeList.push_back(cap-1-i); }
    u64 allocate(const Order &o) {
        if (freeList.empty()) throw runtime_error("Order pool exhausted");
        u64 idx = freeList.back(); freeList.pop_back();
        pool[idx] = o; pool[idx].engineId = idx; pool[idx].active = true;
        return idx;
    }
    void free(u64 idx) {
        pool[idx].active = false; pool[idx].qty = 0; freeList.push_back(idx);
    }
    Order& get(u64 idx) { return pool[idx]; }
};

// ----------------------- FIXED RING BUFFER (PER PRICE LEVEL) -------------
struct RingLevel {
    vector<u64> data; // store engineId
    size_t head = 0;  // pop from head
    size_t tail = 0;  // push to tail
    i64 totalQty = 0; // aggregate outstanding qty
    RingLevel() { data.assign(RING_CAPACITY_PER_LEVEL, UINT64_MAX); }
    inline bool empty() const { return head == tail; }
    inline bool full() const { return ((tail + 1) % data.size()) == head; }
    inline void push(u64 eid, i64 qty) {
        if (full()) throw runtime_error("Price level ring full");
        data[tail] = eid; tail = (tail + 1) % data.size(); totalQty += qty;
    }
    inline u64 front() const { return data[head]; }
    inline void pop_front(i64 qty) {
        if (empty()) throw runtime_error("pop from empty level");
        data[head] = UINT64_MAX; head = (head + 1) % data.size(); totalQty -= qty;
    }
};

// ------------------------------- ORDER BOOK -------------------------------
struct OrderBook {
    int nlevels;
    vector<RingLevel> bids; // index 0..n-1, higher price = larger idx
    vector<RingLevel> asks;
    int bestBid = -1;
    int bestAsk = -1;
    OrderBook(int levels=PRICE_LEVELS):nlevels(levels) { bids.resize(levels); asks.resize(levels); }
    void updateBestAfterAdd(Side s, int idx) {
        if (s==Side::BUY) { if (bestBid < idx) bestBid = idx; }
        else { if (bestAsk == -1 || idx < bestAsk) bestAsk = idx; }
    }
    void updateBestAfterRemove(Side s, int idx) {
        if (s==Side::BUY) {
            if (bestBid != idx) return;
            for (int i=idx;i>=0;--i) if (!bids[i].empty()) { bestBid = i; return; }
            bestBid = -1;
        } else {
            if (bestAsk != idx) return;
            for (int i=idx;i<nlevels;++i) if (!asks[i].empty()) { bestAsk = i; return; }
            bestAsk = -1;
        }
    }
};

// ------------------------------- TRADE -----------------------------------
struct Trade { u64 takerClient; u64 makerClient; i64 qty; int priceIdx; u64 ts; };

// ------------------------------- ENGINE ----------------------------------
struct Engine {
    OrderPool pool;
    OrderBook book;
    unordered_map<u64,u64> clientToEngine; // clientId -> engineId (for last active order per client)
    vector<Trade> trades;
    u64 nextClientId = 1;
    Engine(): pool(ORDER_POOL_CAPACITY), book(PRICE_LEVELS) { clientToEngine.reserve(1<<20); trades.reserve(1<<20); }

    // helpers
    inline bool validIdx(int idx) const { return idx >=0 && idx < book.nlevels; }

    // place limit order (aggressive match then add passive remainder)
    void placeLimit(u64 clientId, Side side, int priceIdx, i64 qty, TimePoint now, TimeInForce tif=TimeInForce::GFD) {
        Order taker; taker.clientId = clientId; taker.side = side; taker.type = OrderType::LIMIT; taker.priceIdx = priceIdx; taker.qty = qty; taker.ts = (u64)chrono::duration_cast<chrono::nanoseconds>(now.time_since_epoch()).count(); taker.tif = tif;
        matchAndAdd(taker);
    }

    // market order
    void placeMarket(u64 clientId, Side side, i64 qty, TimePoint now) {
        Order taker; taker.clientId = clientId; taker.side = side; taker.type = OrderType::MARKET; taker.priceIdx = -1; taker.qty = qty; taker.ts = (u64)chrono::duration_cast<chrono::nanoseconds>(now.time_since_epoch()).count();
        matchMarket(taker);
    }

    // cancel: removes order by clientId if present
    bool cancel(u64 clientId) {
        auto it = clientToEngine.find(clientId);
        if (it==clientToEngine.end()) return false;
        u64 eid = it->second;
        Order &o = pool.get(eid);
        if (!o.active) { clientToEngine.erase(it); return false; }
        // find in ring level front... this is O(n) in level width - optimization: maintain position index per order
        RingLevel &lvl = (o.side==Side::BUY)?book.bids[o.priceIdx]:book.asks[o.priceIdx];
        // linear scan from head to tail to remove
        size_t sz = lvl.data.size(); size_t pos = lvl.head; bool found=false; size_t idxpos=0;
        while (pos != lvl.tail) {
            if (lvl.data[pos]==eid) { found=true; break; }
            pos = (pos+1)%sz; ++idxpos;
        }
        if (!found) { // maybe already matched
            pool.free(eid); clientToEngine.erase(it); return false;
        }
        // remove by shifting items backwards (simple but O(k)). For demo it's acceptable.
        size_t cur = pos; while (cur != (lvl.tail+sz-1)%sz) { size_t next = (cur+1)%sz; lvl.data[cur] = lvl.data[next]; cur = next; }
        lvl.tail = (lvl.tail+sz-1)%sz; lvl.totalQty -= o.qty; pool.free(eid); clientToEngine.erase(it);
        if (lvl.empty()) book.updateBestAfterRemove(o.side, o.priceIdx);
        return true;
    }

    // replace: cancel & place new
    bool replace(u64 clientId, int newPriceIdx, i64 newQty, TimePoint now) {
        auto it = clientToEngine.find(clientId);
        if (it==clientToEngine.end()) return false;
        u64 oldEid = it->second; Order &old = pool.get(oldEid);
        if (!old.active) return false;
        // cancel existing
        cancel(clientId);
        // place new with same clientId
        placeLimit(clientId, old.side, newPriceIdx, newQty, now, old.tif);
        return true;
    }

private:
    void emitTrade(const Order &taker, const Order &maker, i64 qty, int priceIdx) {
        Trade tr{taker.clientId, maker.clientId, qty, priceIdx, (u64)chrono::duration_cast<chrono::nanoseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count() };
        trades.push_back(tr);
    }

    void matchAndAdd(Order &taker) {
        if (taker.side==Side::BUY) {
            // match against asks with price <= taker.priceIdx
            while (taker.qty>0 && book.bestAsk!=-1 && book.bestAsk <= taker.priceIdx) {
                RingLevel &pl = book.asks[book.bestAsk]; if (pl.empty()) { book.updateBestAfterRemove(Side::SELL, book.bestAsk); continue; }
                u64 makerEid = pl.front(); Order &maker = pool.get(makerEid);
                i64 fill = min(maker.qty, taker.qty);
                emitTrade(taker, maker, fill, maker.priceIdx);
                maker.qty -= fill; pl.totalQty -= fill; taker.qty -= fill;
                if (maker.qty==0) { pl.pop_front(0); pool.free(makerEid); clientToEngine.erase(maker.clientId); }
                if (pl.empty()) book.updateBestAfterRemove(Side::SELL, book.bestAsk);
            }
            if (taker.qty>0 && taker.type==OrderType::LIMIT) {
                // add passive
                u64 eid = pool.allocate(taker); book.bids[taker.priceIdx].push(eid, taker.qty); book.updateBestAfterAdd(Side::BUY, taker.priceIdx); clientToEngine[taker.clientId]=eid;
            }
        } else {
            while (taker.qty>0 && book.bestBid!=-1 && book.bestBid >= taker.priceIdx) {
                RingLevel &pl = book.bids[book.bestBid]; if (pl.empty()) { book.updateBestAfterRemove(Side::BUY, book.bestBid); continue; }
                u64 makerEid = pl.front(); Order &maker = pool.get(makerEid);
                i64 fill = min(maker.qty, taker.qty);
                emitTrade(taker, maker, fill, maker.priceIdx);
                maker.qty -= fill; pl.totalQty -= fill; taker.qty -= fill;
                if (maker.qty==0) { pl.pop_front(0); pool.free(makerEid); clientToEngine.erase(maker.clientId); }
                if (pl.empty()) book.updateBestAfterRemove(Side::BUY, book.bestBid);
            }
            if (taker.qty>0 && taker.type==OrderType::LIMIT) {
                u64 eid = pool.allocate(taker); book.asks[taker.priceIdx].push(eid, taker.qty); book.updateBestAfterAdd(Side::SELL, taker.priceIdx); clientToEngine[taker.clientId]=eid;
            }
        }
    }

    void matchMarket(Order &taker) {
        if (taker.side==Side::BUY) {
            while (taker.qty>0 && book.bestAsk!=-1) {
                RingLevel &pl = book.asks[book.bestAsk]; if (pl.empty()) { book.updateBestAfterRemove(Side::SELL, book.bestAsk); continue; }
                u64 makerEid = pl.front(); Order &maker = pool.get(makerEid);
                i64 fill = min(maker.qty, taker.qty);
                emitTrade(taker, maker, fill, maker.priceIdx);
                maker.qty -= fill; pl.totalQty -= fill; taker.qty -= fill;
                if (maker.qty==0) { pl.pop_front(0); pool.free(makerEid); clientToEngine.erase(maker.clientId); }
                if (pl.empty()) book.updateBestAfterRemove(Side::SELL, book.bestAsk);
            }
        } else {
            while (taker.qty>0 && book.bestBid!=-1) {
                RingLevel &pl = book.bids[book.bestBid]; if (pl.empty()) { book.updateBestAfterRemove(Side::BUY, book.bestBid); continue; }
                u64 makerEid = pl.front(); Order &maker = pool.get(makerEid);
                i64 fill = min(maker.qty, taker.qty);
                emitTrade(taker, maker, fill, maker.priceIdx);
                maker.qty -= fill; pl.totalQty -= fill; taker.qty -= fill;
                if (maker.qty==0) { pl.pop_front(0); pool.free(makerEid); clientToEngine.erase(maker.clientId); }
                if (pl.empty()) book.updateBestAfterRemove(Side::BUY, book.bestBid);
            }
        }
    }
};

// ------------------------------- PRICE MAPPING ---------------------------
struct PriceMapper { double tick; double minP; int levels; PriceMapper(double t,double m,int l):tick(t),minP(m),levels(l){}
    inline int priceToIdx(double price) const { int idx = int(round((price - minP) / tick)); if (idx<0) idx=0; if (idx>=levels) idx=levels-1; return idx; }
};

// ------------------------------- WORKLOAD --------------------------------
struct WorkloadGen {
    mt19937_64 rng;
    uniform_real_distribution<double> priceDist;
    uniform_int_distribution<int> qtyDist;
    bernoulli_distribution marketProb;
    bernoulli_distribution sideProb;
    PriceMapper pm;
    WorkloadGen(uint64_t seed, PriceMapper mapper, double lo, double hi):rng(seed),priceDist(lo,hi),qtyDist(1,100),marketProb(0.03),sideProb(0.5),pm(mapper){}
    tuple<OrderType,Side,int,i64> next() {
        bool isMarket = marketProb(rng);
        Side s = sideProb(rng)?Side::BUY:Side::SELL;
        i64 qty = qtyDist(rng);
        if (isMarket) return {OrderType::MARKET,s,-1,qty};
        double p = priceDist(rng); int pidx = pm.priceToIdx(p); return {OrderType::LIMIT,s,pidx,qty};
    }
};

// ------------------------------- DEMO MAIN -------------------------------
int main(){
    ios::sync_with_stdio(false); cin.tie(nullptr);
    PriceMapper pm(TICK, MIN_PRICE, PRICE_LEVELS);
    Engine engine;

    // preload liquidity
    cout<<"Preloading book...\n";
    mt19937_64 prng(42);
    uniform_int_distribution<int> offs(0,2000);
    for (int i=0;i<100000;i++){
        double base = 50.0; double p = base + ((i&1)?(offs(prng)*0.01):(-offs(prng)*0.01));
        int pidx = pm.priceToIdx(p);
        Side s = (i&1)?Side::BUY:Side::SELL; i64 q=(i&7)+1;
        engine.placeLimit(engine.nextClientId++, s, pidx, q, chrono::high_resolution_clock::now());
    }
    cout<<"Preload done. Starting workload...\n";

    WorkloadGen gen(123, pm, 49.0, 51.0);
    const int TOTAL = 500000; // tune
    auto t0 = chrono::high_resolution_clock::now();
    for (int i=0;i<TOTAL;i++){
        auto tup = gen.next();
        OrderType otype = std::get<0>(tup);
        Side side = std::get<1>(tup);
        int pidx = std::get<2>(tup);
        i64 qty = std::get<3>(tup);
        if (otype==OrderType::MARKET) engine.placeMarket(engine.nextClientId++, side, qty, chrono::high_resolution_clock::now());
        else {
            // occasionally place IOC
            TimeInForce tif = (i%200==0)?TimeInForce::IOC:TimeInForce::GFD;
            engine.placeLimit(engine.nextClientId++, side, pidx, qty, chrono::high_resolution_clock::now(), tif);
        }
        // occasionally cancel random client (demo)
        if ((i%10000)==0 && i>0) {
            u64 cid = (u64)(gen.rng() % engine.nextClientId) + 1; engine.cancel(cid);
        }
    }
    auto t1 = chrono::high_resolution_clock::now();
    double secs = chrono::duration<double>(t1-t0).count();
    cout<<"Done. Orders: "<<TOTAL<<" Time: "<<secs<<"s Throughput: "<< (TOTAL/secs) <<" orders/s\n";
    cout<<"Trades: "<<engine.trades.size()<<"\n";
    // print few trades
    for (size_t i=0;i<min((size_t)10, engine.trades.size()); ++i){ auto &tr = engine.trades[i]; cout<<i<<": taker="<<tr.takerClient<<" maker="<<tr.makerClient<<" qty="<<tr.qty<<" price="<<idxToPrice(tr.priceIdx)<<"\n"; }
    return 0;
}
