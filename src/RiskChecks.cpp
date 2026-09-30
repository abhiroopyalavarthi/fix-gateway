#include "RiskChecks.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "Price.h"

std::optional<std::string> RiskChecks::check(const std::string& client, Side side, bool isLimit,
                                             Price refPrice, Price lastTrade, int64_t qty,
                                             const Exposure& e) const {
    if (killed(client)) return "kill switch active for " + client;

    const RiskLimits& lim = cfg_.limitsFor(client);
    if (qty > lim.maxOrderQty)
        return "order qty " + std::to_string(qty) + " exceeds max " + std::to_string(lim.maxOrderQty);

    // qty * price in ticks; __int128 so a silly qty can't overflow the check itself
    __int128 notional = static_cast<__int128>(qty) * refPrice;
    if (notional > lim.maxNotional)
        return "notional " + price::format(static_cast<int64_t>(notional)) + " exceeds max " +
               price::format(lim.maxNotional);

    if (isLimit && lastTrade > 0) {
        // |price - last| / last <= band, kept in integers
        int64_t diff = refPrice > lastTrade ? refPrice - lastTrade : lastTrade - refPrice;
        if (diff * 10'000 > lim.priceBandBps * lastTrade)
            return "price " + price::format(refPrice) + " outside " +
                   std::to_string(lim.priceBandBps) + " bps band around last trade " +
                   price::format(lastTrade);
    }

    // worst case if this order and everything already resting on the same side fills
    if (side == Side::Buy) {
        int64_t worst = e.position + e.openBuy + qty;
        if (worst > lim.maxPosition)
            return "position limit: long " + std::to_string(worst) + " would exceed " +
                   std::to_string(lim.maxPosition);
    } else {
        int64_t worst = e.position - e.openSell - qty;
        if (-worst > lim.maxPosition)
            return "position limit: short " + std::to_string(-worst) + " would exceed " +
                   std::to_string(lim.maxPosition);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// config/risk.cfg:
//
//   max_order_qty = 100000          # defaults for every client
//   max_notional = 10000000.00      # dollars
//   price_band_bps = 1000
//   max_position = 500000
//   client.LP.max_order_qty = 10000000   # per-client override
//   symbol AAPL 100.00 600.00 255.00     # name, book min, book max, reference price

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

int64_t toTicks(const std::string& v, int line) {
    auto t = price::parse(v);
    if (!t) throw std::runtime_error("risk.cfg line " + std::to_string(line) + ": bad price " + v);
    return *t;
}

void setLimit(RiskLimits& lim, const std::string& key, const std::string& val, int line) {
    if (key == "max_order_qty") lim.maxOrderQty = std::stoll(val);
    else if (key == "max_notional") lim.maxNotional = toTicks(val, line);
    else if (key == "price_band_bps") lim.priceBandBps = std::stoll(val);
    else if (key == "max_position") lim.maxPosition = std::stoll(val);
    else throw std::runtime_error("risk.cfg line " + std::to_string(line) + ": unknown key " + key);
}

}  // namespace

GatewayConfig GatewayConfig::parse(const std::string& text) {
    GatewayConfig cfg;
    std::istringstream in(text);
    std::string raw;
    int lineNo = 0;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> overrides;
    while (std::getline(in, raw)) {
        ++lineNo;
        std::string line = trim(raw.substr(0, raw.find('#')));
        if (line.empty()) continue;
        if (line.rfind("symbol ", 0) == 0) {
            std::istringstream ss(line.substr(7));
            std::string name, lo, hi, ref;
            if (!(ss >> name >> lo >> hi >> ref))
                throw std::runtime_error("risk.cfg line " + std::to_string(lineNo) +
                                         ": expected 'symbol NAME MIN MAX REF'");
            cfg.symbols[name] = {toTicks(lo, lineNo), toTicks(hi, lineNo), toTicks(ref, lineNo)};
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("risk.cfg line " + std::to_string(lineNo) + ": expected key = value");
        std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (key.rfind("client.", 0) == 0) {
            auto dot = key.find('.', 7);
            if (dot == std::string::npos)
                throw std::runtime_error("risk.cfg line " + std::to_string(lineNo) + ": bad client key");
            overrides[key.substr(7, dot - 7)].push_back({key.substr(dot + 1), val});
            // applied after the defaults are all read, see below
            continue;
        }
        setLimit(cfg.defaults, key, val, lineNo);
    }
    for (auto& [client, kvs] : overrides) {
        RiskLimits lim = cfg.defaults;
        for (auto& [k, v] : kvs) setLimit(lim, k, v, 0);
        cfg.perClient[client] = lim;
    }
    return cfg;
}

GatewayConfig GatewayConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("can't open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}
