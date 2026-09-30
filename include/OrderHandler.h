#pragma once

// Order state, risk and the books. Takes the gateway's own request types,
// drives FastOrderBook, and emits an ExecReport (or CancelReject) for every
// state change. No FIX in here, so it's tested directly without sockets.
//
// Not thread-safe on purpose: one thread (the gateway's engine thread) owns it.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "FastOrderBook.h"
#include "RiskChecks.h"
#include "Types.h"

struct OrderState {
    uint64_t id = 0;
    std::string client;
    std::string clOrdId;  // current id (changes on replace)
    std::string symbol;
    Side side = Side::Buy;
    OrdKind kind = OrdKind::Limit;
    Tif tif = Tif::Day;
    Price price = 0;
    int64_t qty = 0;       // total order quantity
    int64_t cum = 0;       // filled so far
    int64_t notional = 0;  // sum of fill qty * fill price, ticks
    char status = status::New;
    bool live = true;  // not filled, canceled or rejected. After matching, live == resting

    int64_t leaves() const { return live ? qty - cum : 0; }
    double avgPx() const { return cum ? static_cast<double>(notional) / cum / 100.0 : 0.0; }
};

class OrderHandler {
public:
    using Sink = std::function<void(const Outbound&)>;

    OrderHandler(const GatewayConfig& cfg, Sink sink);

    void onNew(const NewOrder& req);
    void onCancel(const CancelRequest& req);
    void onReplace(const ReplaceRequest& req);
    // Cancels all of a client's open orders and blocks new ones until turned off.
    void killSwitch(const KillSwitch& req);

    // for tests, the admin console and the recovery check
    const OrderState* find(const std::string& client, const std::string& clOrdId) const;
    const std::unordered_map<uint64_t, OrderState>& orders() const { return orders_; }
    Exposure exposure(const std::string& client, const std::string& symbol) const;
    Price lastTrade(const std::string& symbol) const;
    size_t openOrderCount() const;

private:
    struct Book {
        std::unique_ptr<FastOrderBook> book;
        Price lastTrade = 0;
    };

    static std::string key(const std::string& client, const std::string& clOrdId) {
        return client + '\x01' + clOrdId;
    }

    ExecReport report(const OrderState& o, char execType) const;
    void emitReject(const NewOrder& req, const std::string& text, uint64_t id);
    void emitCancelReject(const std::string& client, const std::string& clOrdId,
                          const std::string& origClOrdId, const OrderState* o, char responseTo,
                          int reason, const std::string& text);
    // runs an order into its book and reports the trades on both sides
    void match(OrderState& o, Book& b, OrderType type);
    void applyTrade(OrderState& o, Price px, int64_t qty, const std::string& symbol);
    void closeRemainder(OrderState& o, const std::string& why);
    void trackOpen(const OrderState& o, int64_t delta);

    const GatewayConfig& cfg_;
    Sink sink_;
    RiskChecks risk_;
    std::map<std::string, Book> books_;
    std::unordered_map<uint64_t, OrderState> orders_;
    std::unordered_map<std::string, uint64_t> byClOrdId_;  // every ClOrdID ever used, per client
    std::map<std::pair<std::string, std::string>, Exposure> exposure_;  // (client, symbol)
    uint64_t nextId_ = 1;
    mutable uint64_t nextExec_ = 1;  // report() hands out exec ids
    std::vector<Trade> trades_;
};
