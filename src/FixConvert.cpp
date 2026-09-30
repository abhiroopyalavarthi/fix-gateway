#include "FixConvert.h"

#include <charconv>
#include <cstdio>

#include "Price.h"
#include "quickfix/Values.h"

namespace fixconv {
namespace {

std::string get(const FIX::FieldMap& m, int tag) {
    return m.isSetField(tag) ? m.getField(tag) : std::string();
}

// "100" -> 100. "100.0" is accepted, "100.5" isn't (no fractional shares).
bool parseQty(const std::string& s, int64_t& out) {
    std::string_view v = s;
    if (auto dot = v.find('.'); dot != std::string_view::npos) {
        for (char c : v.substr(dot + 1))
            if (c != '0') return false;
        v = v.substr(0, dot);
    }
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc() && p == v.data() + v.size();
}

void readSide(const std::string& s, Side& side, std::string& err) {
    if (s == "1") side = Side::Buy;
    else if (s == "2") side = Side::Sell;
    else if (err.empty()) err = "unsupported side " + s + " (only 1=buy, 2=sell)";
}

void readOrdType(const std::string& s, OrdKind& kind, std::string& err) {
    if (s == "1") kind = OrdKind::Market;
    else if (s == "2") kind = OrdKind::Limit;
    else if (err.empty()) err = "unsupported OrdType " + s + " (only 1=market, 2=limit)";
}

void readQty(const std::string& s, int64_t& qty, std::string& err) {
    if (!parseQty(s, qty) && err.empty()) err = "bad OrderQty '" + s + "'";
}

}  // namespace

bool isResend(const FIX::Message& m) {
    return get(m.getHeader(), FIX::FIELD::PossDupFlag) == "Y" ||
           get(m.getHeader(), FIX::FIELD::PossResend) == "Y";
}

NewOrder toNewOrder(const FIX44::NewOrderSingle& m, const std::string& client) {
    NewOrder o;
    o.client = client;
    o.clOrdId = get(m, FIX::FIELD::ClOrdID);
    o.symbol = get(m, FIX::FIELD::Symbol);
    readSide(get(m, FIX::FIELD::Side), o.side, o.parseError);
    readOrdType(get(m, FIX::FIELD::OrdType), o.kind, o.parseError);
    readQty(get(m, FIX::FIELD::OrderQty), o.qty, o.parseError);
    o.priceText = get(m, FIX::FIELD::Price);  // exact text, parsed to ticks in OrderHandler
    std::string tif = get(m, FIX::FIELD::TimeInForce);
    if (tif.empty() || tif == "0") o.tif = Tif::Day;
    else if (tif == "3") o.tif = Tif::IOC;
    else if (tif == "4") o.tif = Tif::FOK;
    else if (o.parseError.empty()) o.parseError = "unsupported TimeInForce " + tif + " (0, 3 or 4)";
    if (o.kind == OrdKind::Limit && o.priceText.empty() && o.parseError.empty())
        o.parseError = "limit order without a price";
    o.possResend = isResend(m);
    return o;
}

CancelRequest toCancel(const FIX44::OrderCancelRequest& m, const std::string& client) {
    CancelRequest c;
    c.client = client;
    c.clOrdId = get(m, FIX::FIELD::ClOrdID);
    c.origClOrdId = get(m, FIX::FIELD::OrigClOrdID);
    c.symbol = get(m, FIX::FIELD::Symbol);
    std::string err;
    readSide(get(m, FIX::FIELD::Side), c.side, err);
    return c;
}

ReplaceRequest toReplace(const FIX44::OrderCancelReplaceRequest& m, const std::string& client) {
    ReplaceRequest r;
    r.client = client;
    r.clOrdId = get(m, FIX::FIELD::ClOrdID);
    r.origClOrdId = get(m, FIX::FIELD::OrigClOrdID);
    r.symbol = get(m, FIX::FIELD::Symbol);
    readSide(get(m, FIX::FIELD::Side), r.side, r.parseError);
    readOrdType(get(m, FIX::FIELD::OrdType), r.kind, r.parseError);
    readQty(get(m, FIX::FIELD::OrderQty), r.qty, r.parseError);
    r.priceText = get(m, FIX::FIELD::Price);
    return r;
}

FIX44::ExecutionReport toFix(const ExecReport& r) {
    FIX44::ExecutionReport m(FIX::OrderID(r.orderId), FIX::ExecID(r.execId), FIX::ExecType(r.execType),
                             FIX::OrdStatus(r.ordStatus),
                             FIX::Side(r.side == Side::Buy ? FIX::Side_BUY : FIX::Side_SELL),
                             FIX::LeavesQty(static_cast<double>(r.leavesQty)),
                             FIX::CumQty(static_cast<double>(r.cumQty)), FIX::AvgPx(r.avgPx));
    m.set(FIX::ClOrdID(r.clOrdId));
    if (!r.origClOrdId.empty()) m.set(FIX::OrigClOrdID(r.origClOrdId));
    m.set(FIX::Symbol(r.symbol));
    m.set(FIX::OrderQty(static_cast<double>(r.orderQty)));
    m.set(FIX::OrdType(r.kind == OrdKind::Limit ? FIX::OrdType_LIMIT : FIX::OrdType_MARKET));
    // prices go out as exact decimal strings, not through a double
    if (r.price > 0) m.setField(FIX::FIELD::Price, price::format(r.price));
    if (r.execType == exec::Trade) {
        m.set(FIX::LastQty(static_cast<double>(r.lastQty)));
        m.setField(FIX::FIELD::LastPx, price::format(r.lastPx));
    }
    char avg[32];
    std::snprintf(avg, sizeof avg, "%.6f", r.avgPx);
    m.setField(FIX::FIELD::AvgPx, avg);
    if (!r.text.empty()) m.set(FIX::Text(r.text));
    m.set(FIX::TransactTime(FIX::UTCTIMESTAMP::now(), 6));
    return m;
}

FIX44::OrderCancelReject toFix(const CancelReject& r) {
    FIX44::OrderCancelReject m(FIX::OrderID(r.orderId), FIX::ClOrdID(r.clOrdId),
                               FIX::OrigClOrdID(r.origClOrdId), FIX::OrdStatus(r.ordStatus),
                               FIX::CxlRejResponseTo(r.responseTo));
    m.set(FIX::CxlRejReason(r.reason));
    if (!r.text.empty()) m.set(FIX::Text(r.text));
    return m;
}

}  // namespace fixconv
