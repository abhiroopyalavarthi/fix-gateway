#pragma once

// QuickFIX initiator used by the client simulator, the latency benchmark and
// the integration tests. Keeps the client's own view of every order, built
// only from the execution reports it receives.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/SessionID.h"
#include "quickfix/SessionSettings.h"

struct ClientOrder {
    std::string orderId;
    char status = '?';  // OrdStatus from the latest report
    char lastExecType = '?';
    int64_t cum = 0;
    int64_t leaves = 0;
    double avgPx = 0;
    std::string text;
};

struct CancelRejectSeen {
    std::string clOrdId, origClOrdId, text;
    int reason = 0;
};

class FixClient : public FIX::Application, public FIX::MessageCracker {
public:
    // journalPath: if set, every sent ClOrdID and every report is appended
    // there, and replayed on construction, so a restarted client picks up
    // where it left off.
    explicit FixClient(std::string journalPath = "");

    // Settings with one initiator session: [DEFAULT] from the file, SenderCompID overridden.
    // resetOnLogon: start from sequence 1 each run (only for the in-memory store benchmark)
    static FIX::SessionSettings makeSettings(const std::string& cfgPath, const std::string& sender,
                                             const std::string& storeRoot = "", bool resetOnLogon = false);

    bool waitLogon(std::chrono::milliseconds timeout);
    bool loggedOn() const { return loggedOn_; }

    // `price` in dollars as text ("190.25"); empty = market order. tif: '0' day, '3' IOC, '4' FOK.
    void sendNew(const std::string& clOrdId, const std::string& symbol, char side, int64_t qty,
                 const std::string& price, char tif = '0', bool possResend = false);
    void sendCancel(const std::string& clOrdId, const std::string& origClOrdId,
                    const std::string& symbol, char side);
    void sendReplace(const std::string& clOrdId, const std::string& origClOrdId,
                     const std::string& symbol, char side, int64_t qty, const std::string& price);

    // blocks until pred() is true (checked after every report) or the timeout
    bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout);

    // snapshot accessors (lock internally)
    std::optional<ClientOrder> order(const std::string& clOrdId) const;
    std::map<std::string, ClientOrder> orders() const;
    std::vector<CancelRejectSeen> cancelRejects() const;
    size_t reportCount() const;
    std::vector<std::string> sessionRejects() const;  // Text of every 35=3 we received
    size_t duplicateReports() const;  // resent reports we'd already seen (dropped by ExecID)
    size_t resentReports() const;     // reports that arrived with PossDupFlag=Y
    std::vector<std::string> journalSent() const { return sent_; }
    // order-to-ack latencies in nanoseconds (send -> first ExecutionReport)
    std::vector<int64_t> ackLatencies() const;
    size_t ackCount() const;  // cheap, for polling in the benchmark loop
    void clearLatencies();
    // "clOrdId orderId status cum leaves avgPx" per line, sorted; for comparing with the gateway
    std::string view() const;

    // QuickFIX callbacks
    void onCreate(const FIX::SessionID& s) override { session_ = s; }
    void onLogon(const FIX::SessionID&) override;
    void onLogout(const FIX::SessionID&) override;
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message& m, const FIX::SessionID&) override;
    void fromApp(const FIX::Message& m, const FIX::SessionID& s) override;
    void onMessage(const FIX44::ExecutionReport&, const FIX::SessionID&) override;
    void onMessage(const FIX44::OrderCancelReject&, const FIX::SessionID&) override;

private:
    using Clock = std::chrono::steady_clock;
    void applyReport(const std::string& execId, const std::string& clOrdId,
                     const std::string& origClOrdId, const ClientOrder& r);
    void journal(const std::string& line);

    FIX::SessionID session_;
    std::atomic<bool> loggedOn_{false};
    bool currentIsResend_ = false;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, ClientOrder> orders_;
    std::unordered_set<std::string> seenExec_;
    std::vector<CancelRejectSeen> cxlRejects_;
    std::vector<std::string> sessionRejects_;
    std::unordered_map<std::string, Clock::time_point> pending_;  // clOrdId -> send time
    std::vector<int64_t> latencies_;
    size_t reports_ = 0, dupes_ = 0, resent_ = 0;

    std::string journalPath_;
    std::ofstream journalOut_;
    std::mutex journalMu_;
    std::vector<std::string> sent_;
};
