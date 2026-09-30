#include "Gateway.h"

#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>

#include "FixConvert.h"
#include "quickfix/Session.h"

Gateway::Gateway(const GatewayConfig& cfg)
    : handler_(cfg, [this](const Outbound& out) { send(out); }) {}

Gateway::~Gateway() { stop(); }

void Gateway::start() {
    if (running_.exchange(true)) return;
    engine_ = std::thread([this] { run(); });
}

void Gateway::stop() {
    if (!running_.exchange(false)) return;
    qCv_.notify_all();
    if (engine_.joinable()) engine_.join();
}

void Gateway::push(Command c) {
    {
        std::lock_guard<std::mutex> lk(qMutex_);
        queue_.push_back(std::move(c));
    }
    qCv_.notify_one();
}

void Gateway::run() {
    std::deque<Command> batch;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(qMutex_);
            qCv_.wait(lk, [&] { return !queue_.empty() || !running_; });
            if (queue_.empty() && !running_) return;
            batch.swap(queue_);  // take everything that's waiting in one go
        }
        for (Command& c : batch) {
            std::visit(
                [this](auto& cmd) {
                    using T = std::decay_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<T, NewOrder>) handler_.onNew(cmd);
                    else if constexpr (std::is_same_v<T, CancelRequest>) handler_.onCancel(cmd);
                    else if constexpr (std::is_same_v<T, ReplaceRequest>) handler_.onReplace(cmd);
                    else if constexpr (std::is_same_v<T, KillSwitch>) handler_.killSwitch(cmd);
                    else {
                        cmd.first(handler_);
                        cmd.second->set_value();
                    }
                },
                c);
        }
        batch.clear();
    }
}

void Gateway::send(const Outbound& out) {
    const std::string& client =
        std::visit([](const auto& r) -> const std::string& { return r.client; }, out);
    FIX::SessionID sid;
    {
        std::lock_guard<std::mutex> lk(sessMutex_);
        auto it = sessions_.find(client);
        if (it == sessions_.end()) {
            std::cerr << "no session for client " << client << ", report dropped\n";
            return;
        }
        sid = it->second;
    }
    try {
        // If the client is disconnected, QuickFIX still stores the message and
        // its sequence number, and resends it when the client logs back on and
        // asks for the gap.
        std::visit([&](const auto& r) {
            auto msg = fixconv::toFix(r);
            FIX::Session::sendToTarget(msg, sid);
        }, out);
    } catch (const FIX::SessionNotFound&) {
        std::cerr << "session not found for " << client << "\n";
    }
}

// ---------------------------------------------------------------------------

void Gateway::onCreate(const FIX::SessionID& sid) {
    std::lock_guard<std::mutex> lk(sessMutex_);
    sessions_[sid.getTargetCompID().getValue()] = sid;
}

void Gateway::onLogon(const FIX::SessionID& sid) {
    std::cout << "logon  " << sid.toString() << std::endl;
}

void Gateway::onLogout(const FIX::SessionID& sid) {
    std::cout << "logout " << sid.toString() << std::endl;
}

void Gateway::onMessage(const FIX44::NewOrderSingle& m, const FIX::SessionID& sid) {
    push(fixconv::toNewOrder(m, sid.getTargetCompID().getValue()));
}

void Gateway::onMessage(const FIX44::OrderCancelRequest& m, const FIX::SessionID& sid) {
    push(fixconv::toCancel(m, sid.getTargetCompID().getValue()));
}

void Gateway::onMessage(const FIX44::OrderCancelReplaceRequest& m, const FIX::SessionID& sid) {
    push(fixconv::toReplace(m, sid.getTargetCompID().getValue()));
}

// ---------------------------------------------------------------------------

void Gateway::killSwitch(const std::string& client, bool on) { push(KillSwitch{client, on}); }

void Gateway::inspect(const std::function<void(const OrderHandler&)>& fn) {
    std::promise<void> done;
    auto f = done.get_future();
    push(Inspect{fn, &done});
    f.wait();
}

std::string Gateway::dumpOrders() {
    std::ostringstream out;
    inspect([&](const OrderHandler& h) {
        // sorted by internal id so two dumps can be diffed
        std::map<uint64_t, const OrderState*> sorted;
        for (auto& [id, o] : h.orders()) sorted[id] = &o;
        char avg[32];
        for (auto& [id, o] : sorted) {
            std::snprintf(avg, sizeof avg, "%.6f", o->avgPx());
            out << o->client << ' ' << o->clOrdId << " O" << id << ' ' << o->status << ' ' << o->cum
                << ' ' << o->leaves() << ' ' << avg << '\n';
        }
    });
    return out.str();
}
