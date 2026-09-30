#pragma once

// QuickFIX Application for the acceptor side.
//
// Threads: QuickFIX calls fromApp on its session threads. Those callbacks only
// convert the message and push it onto one queue. A single engine thread pops
// the queue and owns the OrderHandler (and so the books), which keeps matching
// deterministic without any locks around the book. Reports are sent back with
// Session::sendToTarget, which QuickFIX makes thread-safe.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

#include "OrderHandler.h"
#include "quickfix/Application.h"
#include "quickfix/MessageCracker.h"
#include "quickfix/SessionID.h"

class Gateway : public FIX::Application, public FIX::MessageCracker {
public:
    explicit Gateway(const GatewayConfig& cfg);
    ~Gateway() override;

    void start();  // engine thread
    void stop();

    // Admin actions, safe to call from any thread. They go through the same
    // queue as orders, so they're ordered with respect to them.
    void killSwitch(const std::string& client, bool on);
    // Runs fn on the engine thread and waits for it (for dumps and tests).
    void inspect(const std::function<void(const OrderHandler&)>& fn);
    // One line per order: client clOrdId orderId status cum leaves avgPx
    std::string dumpOrders();

    // QuickFIX callbacks
    void onCreate(const FIX::SessionID&) override;
    void onLogon(const FIX::SessionID&) override;
    void onLogout(const FIX::SessionID&) override;
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& m, const FIX::SessionID& s) override { crack(m, s); }

    void onMessage(const FIX44::NewOrderSingle&, const FIX::SessionID&) override;
    void onMessage(const FIX44::OrderCancelRequest&, const FIX::SessionID&) override;
    void onMessage(const FIX44::OrderCancelReplaceRequest&, const FIX::SessionID&) override;

private:
    using Inspect = std::pair<std::function<void(const OrderHandler&)>, std::promise<void>*>;
    using Command = std::variant<NewOrder, CancelRequest, ReplaceRequest, KillSwitch, Inspect>;

    void push(Command c);
    void run();
    void send(const Outbound& out);

    OrderHandler handler_;
    std::mutex qMutex_;
    std::condition_variable qCv_;
    std::deque<Command> queue_;
    std::atomic<bool> running_{false};
    std::thread engine_;

    std::mutex sessMutex_;
    std::unordered_map<std::string, FIX::SessionID> sessions_;  // client comp id -> session
};
