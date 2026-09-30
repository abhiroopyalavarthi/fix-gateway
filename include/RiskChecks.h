#pragma once

// Pre-trade risk. Every new order and every replace goes through check()
// before it gets near the book; a failure comes back as a reason string that
// ends up in tag 58 of the reject.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Order.h"

struct RiskLimits {
    int64_t maxOrderQty = 100'000;
    int64_t maxNotional = 10'000'000'00;  // ticks (cents): $10M
    int64_t priceBandBps = 1'000;         // limit price within 10% of the last trade
    int64_t maxPosition = 500'000;        // shares, long or short, per symbol
};

struct SymbolConfig {
    Price minPrice = 1;    // band for FastOrderBook's price array
    Price maxPrice = 100'000;
    Price refPrice = 0;    // "last trade" until the first real trade
};

struct GatewayConfig {
    RiskLimits defaults;
    std::map<std::string, RiskLimits> perClient;  // overrides
    std::map<std::string, SymbolConfig> symbols;

    const RiskLimits& limitsFor(const std::string& client) const {
        auto it = perClient.find(client);
        return it == perClient.end() ? defaults : it->second;
    }

    // Reads the simple key = value format in config/risk.cfg. Throws on errors.
    static GatewayConfig load(const std::string& path);
    static GatewayConfig parse(const std::string& text);
};

// What the risk layer needs to know about a client's exposure in one symbol.
struct Exposure {
    int64_t position = 0;  // filled, signed
    int64_t openBuy = 0;   // resting buy shares
    int64_t openSell = 0;  // resting sell shares
};

class RiskChecks {
public:
    explicit RiskChecks(const GatewayConfig& cfg) : cfg_(cfg) {}

    // `refPrice` is the limit price for limit orders, the last trade for market
    // orders. `lastTrade` is what the price band is measured from.
    // Returns nullopt if the order is fine, otherwise the reason.
    std::optional<std::string> check(const std::string& client, Side side, bool isLimit,
                                     Price refPrice, Price lastTrade, int64_t qty,
                                     const Exposure& exposure) const;

    void setKill(const std::string& client, bool on) {
        if (on) killed_.insert(client);
        else killed_.erase(client);
    }
    bool killed(const std::string& client) const { return killed_.count(client) > 0; }

private:
    const GatewayConfig& cfg_;
    std::unordered_set<std::string> killed_;
};
