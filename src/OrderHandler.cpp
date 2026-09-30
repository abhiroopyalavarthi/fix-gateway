#include "OrderHandler.h"

#include <algorithm>
#include <limits>

#include "Price.h"

namespace {

char statusFor(const OrderState& o) {
    if (o.cum >= o.qty) return status::Filled;
    return o.cum > 0 ? status::PartiallyFilled : status::New;
}

const char* statusName(char s) {
    switch (s) {
        case status::New: return "new";
        case status::PartiallyFilled: return "partially filled";
        case status::Filled: return "filled";
        case status::Canceled: return "canceled";
        case status::Rejected: return "rejected";
    }
    return "unknown";
}

}  // namespace

OrderHandler::OrderHandler(const GatewayConfig& cfg, Sink sink)
    : cfg_(cfg), sink_(std::move(sink)), risk_(cfg) {
    for (const auto& [sym, sc] : cfg.symbols) {
        Book b;
        b.book = std::make_unique<FastOrderBook>(sc.minPrice, sc.maxPrice, 1 << 16);
        b.lastTrade = sc.refPrice;
        books_.emplace(sym, std::move(b));
    }
    trades_.reserve(64);
}

// ---------------------------------------------------------------------------
// reports

ExecReport OrderHandler::report(const OrderState& o, char execType) const {
    ExecReport r;
    r.client = o.client;
    r.orderId = "O" + std::to_string(o.id);
    r.execId = "E" + std::to_string(nextExec_++);
    r.clOrdId = o.clOrdId;
    r.execType = execType;
    r.ordStatus = o.status;
    r.symbol = o.symbol;
    r.side = o.side;
    r.kind = o.kind;
    r.price = o.price;
    r.orderQty = o.qty;
    r.cumQty = o.cum;
    r.leavesQty = o.leaves();
    r.avgPx = o.avgPx();
    return r;
}

void OrderHandler::emitReject(const NewOrder& req, const std::string& text, uint64_t id) {
    ExecReport r;
    r.client = req.client;
    r.orderId = id ? "O" + std::to_string(id) : "NONE";
    r.execId = "E" + std::to_string(nextExec_++);
    r.clOrdId = req.clOrdId;
    r.execType = exec::Rejected;
    r.ordStatus = status::Rejected;
    r.symbol = req.symbol;
    r.side = req.side;
    r.kind = req.kind;
    if (auto px = price::parse(req.priceText)) r.price = *px;
    r.orderQty = std::max<int64_t>(0, req.qty);
    r.text = text;
    sink_(r);
}

void OrderHandler::emitCancelReject(const std::string& client, const std::string& clOrdId,
                                    const std::string& origClOrdId, const OrderState* o,
                                    char responseTo, int reason, const std::string& text) {
    CancelReject r;
    r.client = client;
    r.orderId = o ? "O" + std::to_string(o->id) : "NONE";
    r.clOrdId = clOrdId;
    r.origClOrdId = origClOrdId;
    r.ordStatus = o ? o->status : status::Rejected;
    r.responseTo = responseTo;
    r.reason = reason;
    r.text = text;
    sink_(r);
}

// ---------------------------------------------------------------------------
// exposure bookkeeping for the position check

Exposure OrderHandler::exposure(const std::string& client, const std::string& symbol) const {
    auto it = exposure_.find({client, symbol});
    return it == exposure_.end() ? Exposure{} : it->second;
}

void OrderHandler::trackOpen(const OrderState& o, int64_t delta) {
    Exposure& e = exposure_[{o.client, o.symbol}];
    (o.side == Side::Buy ? e.openBuy : e.openSell) += delta;
}

Price OrderHandler::lastTrade(const std::string& symbol) const {
    auto it = books_.find(symbol);
    return it == books_.end() ? 0 : it->second.lastTrade;
}

const OrderState* OrderHandler::find(const std::string& client, const std::string& clOrdId) const {
    auto it = byClOrdId_.find(key(client, clOrdId));
    if (it == byClOrdId_.end()) return nullptr;
    auto o = orders_.find(it->second);
    return o == orders_.end() ? nullptr : &o->second;
}

size_t OrderHandler::openOrderCount() const {
    return static_cast<size_t>(
        std::count_if(orders_.begin(), orders_.end(), [](auto& kv) { return kv.second.live; }));
}

// ---------------------------------------------------------------------------
// matching

void OrderHandler::applyTrade(OrderState& o, Price px, int64_t qty, const std::string& symbol) {
    o.cum += qty;
    o.notional += qty * px;
    o.status = statusFor(o);
    if (o.cum == o.qty) o.live = false;
    exposure_[{o.client, symbol}].position += (o.side == Side::Buy ? qty : -qty);
    ExecReport r = report(o, exec::Trade);
    r.lastQty = qty;
    r.lastPx = px;
    sink_(r);
}

void OrderHandler::match(OrderState& o, Book& b, OrderType type) {
    trades_.clear();
    b.book->addOrder(o.id, o.side, type, o.price, static_cast<Quantity>(o.qty - o.cum), trades_);
    for (const Trade& t : trades_) {
        uint64_t otherId = o.side == Side::Buy ? t.sellId : t.buyId;
        applyTrade(o, t.price, t.quantity, o.symbol);
        OrderState& r = orders_.at(otherId);
        trackOpen(r, -static_cast<int64_t>(t.quantity));
        applyTrade(r, t.price, t.quantity, r.symbol);
        b.lastTrade = t.price;
    }
    if (!o.live) return;  // filled
    if (type == OrderType::Limit) {
        trackOpen(o, o.qty - o.cum);  // it's resting now
        return;
    }
    const char* why = type == OrderType::FOK   ? "FOK: not enough liquidity to fill the whole order"
                      : type == OrderType::IOC ? "IOC: unfilled quantity canceled"
                                               : "market order: no more liquidity, remainder canceled";
    closeRemainder(o, why);
}

void OrderHandler::closeRemainder(OrderState& o, const std::string& why) {
    o.live = false;
    o.status = status::Canceled;
    ExecReport r = report(o, exec::Canceled);
    r.text = why;
    sink_(r);
}

// ---------------------------------------------------------------------------
// new order

void OrderHandler::onNew(const NewOrder& req) {
    if (req.clOrdId.empty()) return emitReject(req, "missing ClOrdID", 0);

    const std::string k = key(req.client, req.clOrdId);
    if (auto it = byClOrdId_.find(k); it != byClOrdId_.end()) {
        if (req.possResend) {
            // same order sent again after a reconnect: don't book it twice,
            // just tell the client where it stands
            ExecReport r = report(orders_.at(it->second), exec::OrderStatus);
            r.text = "possible resend of an order already received";
            sink_(r);
            return;
        }
        return emitReject(req, "duplicate ClOrdID " + req.clOrdId, 0);
    }

    const uint64_t id = nextId_++;
    byClOrdId_[k] = id;
    // keep a record even for rejects so the id can't be reused and the log tool can find it
    OrderState o;
    o.id = id;
    o.client = req.client;
    o.clOrdId = req.clOrdId;
    o.symbol = req.symbol;
    o.side = req.side;
    o.kind = req.kind;
    o.tif = req.tif;
    o.qty = req.qty;
    auto reject = [&](const std::string& text) {
        o.live = false;
        o.status = status::Rejected;
        orders_[id] = o;
        emitReject(req, text, id);
    };

    if (!req.parseError.empty()) return reject(req.parseError);
    auto bit = books_.find(req.symbol);
    if (bit == books_.end()) return reject("unknown symbol " + req.symbol);
    Book& b = bit->second;

    if (req.qty <= 0) return reject("order qty must be positive");
    if (req.qty > std::numeric_limits<Quantity>::max()) return reject("order qty too large");

    if (req.kind == OrdKind::Limit) {
        auto px = price::parse(req.priceText);
        if (!px)
            return reject("price '" + req.priceText + "' is not a positive price on the 0.01 tick grid");
        o.price = *px;
        const SymbolConfig& sc = cfg_.symbols.at(req.symbol);
        if (o.price < sc.minPrice || o.price > sc.maxPrice)
            return reject("price " + price::format(o.price) + " outside the book's range " +
                          price::format(sc.minPrice) + "-" + price::format(sc.maxPrice));
    }

    const Price ref = req.kind == OrdKind::Limit ? o.price : b.lastTrade;
    if (auto why = risk_.check(req.client, req.side, req.kind == OrdKind::Limit, ref, b.lastTrade,
                               req.qty, exposure(req.client, req.symbol)))
        return reject(*why);

    OrderState& live = orders_[id] = o;
    sink_(report(live, exec::New));

    OrderType type = OrderType::Market;
    if (req.kind == OrdKind::Limit)
        type = req.tif == Tif::IOC ? OrderType::IOC : req.tif == Tif::FOK ? OrderType::FOK : OrderType::Limit;
    match(live, b, type);
}

// ---------------------------------------------------------------------------
// cancel

void OrderHandler::onCancel(const CancelRequest& req) {
    auto it = byClOrdId_.find(key(req.client, req.origClOrdId));
    if (it == byClOrdId_.end())
        return emitCancelReject(req.client, req.clOrdId, req.origClOrdId, nullptr, '1', 1,
                                "unknown order " + req.origClOrdId);
    OrderState& o = orders_.at(it->second);
    if (req.clOrdId.empty() || byClOrdId_.count(key(req.client, req.clOrdId)))
        return emitCancelReject(req.client, req.clOrdId, req.origClOrdId, &o, '1', 99,
                                "duplicate or missing ClOrdID " + req.clOrdId);
    if (!o.live)
        return emitCancelReject(req.client, req.clOrdId, req.origClOrdId, &o, '1', 0,
                                std::string("too late to cancel: order is ") + statusName(o.status));

    byClOrdId_[key(req.client, req.clOrdId)] = o.id;
    books_.at(o.symbol).book->cancelOrder(o.id);
    trackOpen(o, -(o.qty - o.cum));
    o.live = false;
    o.status = status::Canceled;
    o.clOrdId = req.clOrdId;
    ExecReport r = report(o, exec::Canceled);
    r.origClOrdId = req.origClOrdId;
    sink_(r);
}

// ---------------------------------------------------------------------------
// cancel/replace

void OrderHandler::onReplace(const ReplaceRequest& req) {
    auto rej = [&](const OrderState* o, int reason, const std::string& text) {
        emitCancelReject(req.client, req.clOrdId, req.origClOrdId, o, '2', reason, text);
    };
    auto it = byClOrdId_.find(key(req.client, req.origClOrdId));
    if (it == byClOrdId_.end()) return rej(nullptr, 1, "unknown order " + req.origClOrdId);
    OrderState& o = orders_.at(it->second);
    if (req.clOrdId.empty() || byClOrdId_.count(key(req.client, req.clOrdId)))
        return rej(&o, 99, "duplicate or missing ClOrdID " + req.clOrdId);
    if (!o.live) return rej(&o, 0, std::string("too late to replace: order is ") + statusName(o.status));
    if (!req.parseError.empty()) return rej(&o, 99, req.parseError);
    if (req.kind != OrdKind::Limit || o.kind != OrdKind::Limit)
        return rej(&o, 99, "only resting limit orders can be replaced");
    if (req.qty <= 0 || req.qty > std::numeric_limits<Quantity>::max())
        return rej(&o, 99, "order qty must be positive");
    if (req.qty < o.cum)
        return rej(&o, 99, "new qty " + std::to_string(req.qty) + " is below the " +
                               std::to_string(o.cum) + " already filled");
    auto px = price::parse(req.priceText);
    if (!px) return rej(&o, 99, "price '" + req.priceText + "' is not on the 0.01 tick grid");
    const SymbolConfig& sc = cfg_.symbols.at(o.symbol);
    if (*px < sc.minPrice || *px > sc.maxPrice) return rej(&o, 99, "price outside the book's range");

    Book& b = books_.at(o.symbol);
    const int64_t oldLeaves = o.qty - o.cum;
    const int64_t newLeaves = req.qty - o.cum;
    // Risk on the order as it would be after the replace. check() adds the full
    // qty to the resting total for the position test, but only newLeaves can
    // still fill, and this order's current leaves are already in the total.
    if (newLeaves > 0) {
        Exposure e = exposure(o.client, o.symbol);
        (o.side == Side::Buy ? e.openBuy : e.openSell) -= oldLeaves + o.cum;
        if (auto why = risk_.check(o.client, o.side, true, *px, b.lastTrade, req.qty, e))
            return rej(&o, 99, *why);
    }

    byClOrdId_[key(req.client, req.clOrdId)] = o.id;
    const std::string orig = req.origClOrdId;
    o.clOrdId = req.clOrdId;

    if (*px == o.price && req.qty <= o.qty) {
        // quantity down (or nothing changed): same place in the queue
        if (newLeaves == 0) {
            b.book->cancelOrder(o.id);
            o.live = false;
        } else if (newLeaves < oldLeaves) {
            b.book->reduceOrder(o.id, static_cast<Quantity>(newLeaves));
        }
        trackOpen(o, newLeaves - oldLeaves);
        o.qty = req.qty;
        o.status = statusFor(o);
        ExecReport r = report(o, exec::Replaced);
        r.origClOrdId = orig;
        sink_(r);
        return;
    }

    // price change or quantity up: out of the book and back in at the end of the queue
    b.book->cancelOrder(o.id);
    trackOpen(o, -oldLeaves);
    o.price = *px;
    o.qty = req.qty;
    o.status = statusFor(o);
    ExecReport r = report(o, exec::Replaced);
    r.origClOrdId = orig;
    sink_(r);
    match(o, b, OrderType::Limit);
}

// ---------------------------------------------------------------------------
// kill switch

void OrderHandler::killSwitch(const KillSwitch& req) {
    risk_.setKill(req.client, req.on);
    if (!req.on) return;
    std::vector<uint64_t> ids;
    for (auto& [id, o] : orders_)
        if (o.client == req.client && o.live) ids.push_back(id);
    std::sort(ids.begin(), ids.end());  // report in the order they arrived
    for (uint64_t id : ids) {
        OrderState& o = orders_.at(id);
        books_.at(o.symbol).book->cancelOrder(id);
        trackOpen(o, -(o.qty - o.cum));
        o.live = false;
        o.status = status::Canceled;
        ExecReport r = report(o, exec::Canceled);
        r.text = "kill switch";
        sink_(r);
    }
}
