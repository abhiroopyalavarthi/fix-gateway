// fix_client: client simulator and latency benchmark.
//
//   fix_client bench  --sender BENCH --orders 100000 [--mode pingpong|burst] [--window 64]
//   fix_client stream --sender CLIENT1 --orders 5000 --side buy --price 338.00 --qty 100
//                     --journal state/CLIENT1.journal [--rate 2000] [--view out.txt]
//   fix_client send   --sender CLIENT1 --id ORD-1 --side buy --qty 100 [--price 338.00]
//                     [--tif 0|3|4] [--symbol AAPL]
//   fix_client cancel --sender CLIENT1 --id CXL-1 --orig ORD-1 --side buy
//   fix_client replace --sender CLIENT1 --id ORD-1b --orig ORD-1 --side buy --qty 200 --price 338.10
//
// Common: --config config/client.cfg (default), --symbol AAPL (default),
//         --store file|memory, --log file|none (bench: measure what persistence costs)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "FixClient.h"
#include "quickfix/FileLog.h"
#include "quickfix/FileStore.h"
#include "quickfix/MessageStore.h"
#include "quickfix/SocketInitiator.h"

using namespace std::chrono_literals;

namespace {

std::map<std::string, std::string> parseArgs(int argc, char** argv, int from) {
    std::map<std::string, std::string> a;
    for (int i = from; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) continue;
        a[k.substr(2)] = (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) ? argv[++i] : "1";
    }
    return a;
}

std::string opt(const std::map<std::string, std::string>& a, const std::string& k, const std::string& d) {
    auto it = a.find(k);
    return it == a.end() ? d : it->second;
}

char sideChar(const std::string& s) { return s == "sell" ? '2' : '1'; }

double pct(std::vector<int64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t i = static_cast<size_t>(std::ceil(p / 100.0 * v.size())) - 1;
    return v[std::min(i, v.size() - 1)] / 1000.0;  // ns -> us
}

void printLatency(std::vector<int64_t> v, double seconds, size_t orders) {
    std::sort(v.begin(), v.end());
    std::printf("acks        %zu\n", v.size());
    std::printf("p50         %.1f us\n", pct(v, 50));
    std::printf("p90         %.1f us\n", pct(v, 90));
    std::printf("p99         %.1f us\n", pct(v, 99));
    std::printf("p99.9       %.1f us\n", pct(v, 99.9));
    std::printf("max         %.1f us\n", v.empty() ? 0.0 : v.back() / 1000.0);
    std::printf("throughput  %.0f orders/sec (%zu orders in %.3f s)\n", orders / seconds, orders, seconds);
}

// Bench orders come in pairs: a buy that rests, then a sell at the same price
// that fills it. Every order goes through risk, the book and a trade, and the
// book stays small, so the numbers don't drift as the run goes on.
void bench(FixClient& c, const std::map<std::string, std::string>& a) {
    const size_t n = std::stoul(opt(a, "orders", "20000"));
    const std::string mode = opt(a, "mode", "pingpong");
    const size_t window = std::stoul(opt(a, "window", "64"));
    const std::string sym = opt(a, "symbol", "AAPL");
    const std::string tag = opt(a, "tag", std::to_string(std::chrono::system_clock::now().time_since_epoch().count() % 100000000));
    const size_t warmup = std::min<size_t>(1000, n / 10);

    auto send = [&](size_t i) {
        c.sendNew("B" + tag + "-" + std::to_string(i), sym, i % 2 == 0 ? '1' : '2', 100, "338.00");
    };
    auto acked = [&] { return c.ackCount(); };

    for (size_t i = 0; i < warmup; ++i) {
        send(i);
        c.waitFor([&] { return acked() > i; }, 5s);
    }
    c.clearLatencies();

    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = warmup; i < warmup + n; ++i) {
        if (mode == "pingpong") {
            size_t before = acked();
            send(i);
            if (!c.waitFor([&] { return acked() > before; }, 5s)) {
                std::cerr << "timed out waiting for ack " << i << "\n";
                return;
            }
        } else {
            size_t done = i - warmup;
            if (done >= window) c.waitFor([&] { return acked() + window > done; }, 5s);
            send(i);
        }
    }
    c.waitFor([&] { return acked() >= n; }, 30s);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("mode        %s%s\n", mode.c_str(),
                mode == "burst" ? (" (window " + std::to_string(window) + ")").c_str() : "");
    printLatency(c.ackLatencies(), secs, n);
}

// Sends ORDERS orders with ids PREFIX-1..N, resuming after the last id in the
// journal. Used by scripts/recovery_demo.sh, which kill -9's this process halfway.
void stream(FixClient& c, const std::map<std::string, std::string>& a) {
    const size_t n = std::stoul(opt(a, "orders", "2000"));
    const std::string prefix = opt(a, "prefix", opt(a, "sender", "C"));
    const std::string sym = opt(a, "symbol", "AAPL");
    const char side = sideChar(opt(a, "side", "buy"));
    const std::string price = opt(a, "price", "338.00");
    const int64_t qty = std::stoll(opt(a, "qty", "100"));
    const double rate = std::stod(opt(a, "rate", "2000"));

    auto sent = c.journalSent();
    size_t start = 1;
    if (!sent.empty()) {
        const std::string& last = sent.back();
        start = std::stoul(last.substr(last.rfind('-') + 1)) + 1;
        // We may have died between writing the journal and QuickFIX sending
        // it. Send the last one again flagged as a possible resend (97=Y): the
        // gateway answers with its status instead of booking it twice.
        c.sendNew(last, sym, side, qty, price, '0', true);
        std::printf("resuming after %s (re-sent it with PossResend=Y)\n", last.c_str());
    }
    auto gap = std::chrono::nanoseconds(static_cast<int64_t>(1e9 / rate));
    auto next = std::chrono::steady_clock::now();
    for (size_t i = start; i <= n; ++i) {
        c.sendNew(prefix + "-" + std::to_string(i), sym, side, qty, price);
        if (i % 500 == 0) std::printf("sent %zu\n", i), std::fflush(stdout);
        next += gap;
        std::this_thread::sleep_until(next);
    }
    std::printf("all %zu sent\n", n);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fix_client bench|stream|send|cancel [options]\n";
        return 2;
    }
    const std::string cmd = argv[1];
    auto a = parseArgs(argc, argv, 2);
    const std::string sender = opt(a, "sender", "CLIENT1");

    try {
        FixClient client(opt(a, "journal", ""));
        auto settings = FixClient::makeSettings(opt(a, "config", "config/client.cfg"), sender,
                                                opt(a, "state", ""), opt(a, "store", "file") == "memory");
        // --store memory / --log none: see what persistence costs (bench only)
        FIX::FileStoreFactory fileStore(settings);
        FIX::MemoryStoreFactory memStore;
        FIX::FileLogFactory fileLog(settings);
        FIX::MessageStoreFactory& store = opt(a, "store", "file") == "memory"
                                              ? static_cast<FIX::MessageStoreFactory&>(memStore)
                                              : fileStore;
        std::unique_ptr<FIX::SocketInitiator> init =
            opt(a, "log", "file") == "none"
                ? std::make_unique<FIX::SocketInitiator>(client, store, settings)
                : std::make_unique<FIX::SocketInitiator>(client, store, settings, fileLog);
        FIX::SocketInitiator& initiator = *init;
        initiator.start();
        if (!client.waitLogon(10s)) {
            std::cerr << "logon failed (is fix_gateway running?)\n";
            return 1;
        }

        if (cmd == "bench") {
            bench(client, a);
        } else if (cmd == "stream") {
            stream(client, a);
            // wait for the reports to settle, then write this client's view
            size_t last = 0;
            for (int quiet = 0; quiet < 10;) {
                std::this_thread::sleep_for(200ms);
                size_t now = client.reportCount();
                quiet = now == last ? quiet + 1 : 0;
                last = now;
            }
            std::printf("reports %zu, resent by gateway %zu, duplicates dropped %zu, orders %zu\n",
                        client.reportCount(), client.resentReports(), client.duplicateReports(),
                        client.orders().size());
            if (a.count("view")) std::ofstream(a["view"]) << client.view();
        } else if (cmd == "send") {
            const std::string id = opt(a, "id", "ORD-1");
            client.sendNew(id, opt(a, "symbol", "AAPL"), sideChar(opt(a, "side", "buy")),
                           std::stoll(opt(a, "qty", "100")), opt(a, "price", ""),
                           opt(a, "tif", "0")[0]);
            client.waitFor([&] { return client.order(id).has_value(); }, 5s);
            std::this_thread::sleep_for(200ms);  // let any fills arrive too
            if (auto o = client.order(id))
                std::printf("%s  OrdStatus=%c ExecType=%c cum=%lld leaves=%lld avgPx=%.4f %s\n",
                            id.c_str(), o->status, o->lastExecType, (long long)o->cum,
                            (long long)o->leaves, o->avgPx, o->text.c_str());
            else
                std::printf("%s  no report\n", id.c_str());
        } else if (cmd == "cancel") {
            client.sendCancel(opt(a, "id", "CXL-1"), opt(a, "orig", "ORD-1"), opt(a, "symbol", "AAPL"),
                              sideChar(opt(a, "side", "buy")));
            std::this_thread::sleep_for(500ms);
            for (auto& r : client.cancelRejects())
                std::printf("cancel reject %s: %s\n", r.clOrdId.c_str(), r.text.c_str());
            if (auto o = client.order(opt(a, "id", "CXL-1")))
                std::printf("canceled, cum=%lld\n", (long long)o->cum);
        } else if (cmd == "replace") {
            const std::string id = opt(a, "id", "ORD-1b");
            client.sendReplace(id, opt(a, "orig", "ORD-1"), opt(a, "symbol", "AAPL"),
                               sideChar(opt(a, "side", "buy")), std::stoll(opt(a, "qty", "100")),
                               opt(a, "price", "338.00"));
            client.waitFor([&] { return client.order(id).has_value() || !client.cancelRejects().empty(); }, 5s);
            std::this_thread::sleep_for(200ms);
            for (auto& r : client.cancelRejects())
                std::printf("replace refused: %s\n", r.text.c_str());
            if (auto o = client.order(id))
                std::printf("%s  OrdStatus=%c ExecType=%c cum=%lld leaves=%lld\n", id.c_str(), o->status,
                            o->lastExecType, (long long)o->cum, (long long)o->leaves);
        } else {
            std::cerr << "unknown command " << cmd << "\n";
            return 2;
        }
        initiator.stop();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
