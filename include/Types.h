#pragma once

// The gateway's own message types. FIX is converted to these at the edge
// (FixConvert.cpp) and back again on the way out, so OrderHandler, RiskChecks
// and the order book never see a FIX tag.

#include <cstdint>
#include <string>
#include <variant>

#include "Order.h"  // Price, Quantity, Side, OrderType from the order book

enum class OrdKind { Market, Limit };
enum class Tif { Day, IOC, FOK };

// FIX 4.4 ExecType (150) and OrdStatus (39) values we use.
namespace exec {
constexpr char New = '0';
constexpr char Canceled = '4';
constexpr char Replaced = '5';
constexpr char Rejected = '8';
constexpr char Trade = 'F';
constexpr char OrderStatus = 'I';
}  // namespace exec

namespace status {
constexpr char New = '0';
constexpr char PartiallyFilled = '1';
constexpr char Filled = '2';
constexpr char Canceled = '4';
constexpr char Rejected = '8';
}  // namespace status

struct NewOrder {
    std::string client;     // SenderCompID of the session it came from
    std::string clOrdId;    // 11
    std::string symbol;     // 55
    Side side = Side::Buy;  // 54
    OrdKind kind = OrdKind::Limit;  // 40
    Tif tif = Tif::Day;     // 59
    std::string priceText;  // 44 as sent on the wire; parsed to ticks in OrderHandler
    int64_t qty = 0;        // 38 (signed so a negative on the wire can be rejected)
    bool possResend = false;  // 97=Y or a session-level resend (43=Y)
    std::string parseError;   // set at the FIX edge for values we don't support
};

struct CancelRequest {
    std::string client;
    std::string clOrdId;      // 11, the cancel's own id
    std::string origClOrdId;  // 41, the order to cancel
    std::string symbol;
    Side side = Side::Buy;
};

struct ReplaceRequest {
    std::string client;
    std::string clOrdId;      // 11, the new id for the order
    std::string origClOrdId;  // 41
    std::string symbol;
    Side side = Side::Buy;
    OrdKind kind = OrdKind::Limit;
    std::string priceText;
    int64_t qty = 0;  // new *total* order quantity, filled shares included (FIX rule)
    std::string parseError;
};

struct KillSwitch {
    std::string client;
    bool on = true;
};

struct ExecReport {
    std::string client;
    std::string orderId;      // 37
    std::string execId;       // 17
    std::string clOrdId;      // 11
    std::string origClOrdId;  // 41 (cancel / replace only)
    char execType = exec::New;
    char ordStatus = status::New;
    std::string symbol;
    Side side = Side::Buy;
    OrdKind kind = OrdKind::Limit;
    Price price = 0;       // limit price in ticks (0 for market)
    int64_t orderQty = 0;  // 38
    int64_t lastQty = 0;   // 32
    Price lastPx = 0;      // 31, ticks
    int64_t cumQty = 0;    // 14
    int64_t leavesQty = 0; // 151
    double avgPx = 0.0;    // 6, dollars
    std::string text;      // 58
};

struct CancelReject {
    std::string client;
    std::string orderId;  // "NONE" if we never found the order
    std::string clOrdId;
    std::string origClOrdId;
    char ordStatus = status::Rejected;
    char responseTo = '1';  // 434: 1 = cancel, 2 = cancel/replace
    int reason = 99;        // 102: 0 too late, 1 unknown order, 99 other
    std::string text;
};

using Outbound = std::variant<ExecReport, CancelReject>;
