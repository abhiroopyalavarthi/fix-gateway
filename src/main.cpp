// fix_gateway: the FIX 4.4 acceptor.
//
//   fix_gateway [--config config/gateway.cfg] [--risk config/risk.cfg] [--dump FILE]
//               [--store file|memory] [--log file|none]
//
// --store memory / --log none are only for measuring what persistence costs;
// with them a restart can't resume a session.
//
// Admin commands on stdin:
//   kill CLIENT      cancel all of CLIENT's open orders and block new ones
//   unkill CLIENT    let CLIENT trade again
//   dump [FILE]      write every order's state (to FILE or stdout)
//   status           open orders per symbol, last trades
//   quit
// SIGINT / SIGTERM also shut it down cleanly (writing --dump if given).

#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "Gateway.h"
#include "Price.h"
#include "quickfix/FileLog.h"
#include "quickfix/FileStore.h"
#include "quickfix/MessageStore.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/ThreadedSocketAcceptor.h"

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }
}  // namespace

int main(int argc, char** argv) {
    std::string cfgPath = "config/gateway.cfg", riskPath = "config/risk.cfg", dumpPath;
    std::string storeKind = "file", logKind = "file";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--config") cfgPath = argv[i + 1];
        else if (a == "--risk") riskPath = argv[i + 1];
        else if (a == "--dump") dumpPath = argv[i + 1];
        else if (a == "--store") storeKind = argv[i + 1];
        else if (a == "--log") logKind = argv[i + 1];
        else {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        }
    }

    try {
        GatewayConfig cfg = GatewayConfig::load(riskPath);
        Gateway gw(cfg);
        gw.start();

        FIX::SessionSettings settings(cfgPath);
        FIX::FileStoreFactory fileStore(settings);  // sequence numbers + sent messages survive restarts
        FIX::MemoryStoreFactory memStore;
        FIX::FileLogFactory fileLog(settings);      // what tools/fixlog reads
        FIX::MessageStoreFactory& store =
            storeKind == "memory" ? static_cast<FIX::MessageStoreFactory&>(memStore) : fileStore;
        std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor =
            logKind == "none" ? std::make_unique<FIX::ThreadedSocketAcceptor>(gw, store, settings)
                              : std::make_unique<FIX::ThreadedSocketAcceptor>(gw, store, settings, fileLog);
        acceptor->start();
        std::cout << "gateway up, " << cfg.symbols.size() << " symbols, store=" << storeKind
                  << " log=" << logKind << std::endl;

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        // stdin admin console on its own thread so a signal can still stop us
        std::thread console([&] {
            std::string line;
            while (!g_stop && std::getline(std::cin, line)) {
                std::istringstream ss(line);
                std::string cmd, arg;
                ss >> cmd >> arg;
                if (cmd == "kill" && !arg.empty()) {
                    gw.killSwitch(arg, true);
                    std::cout << "kill switch ON for " << arg << std::endl;
                } else if (cmd == "unkill" && !arg.empty()) {
                    gw.killSwitch(arg, false);
                    std::cout << "kill switch off for " << arg << std::endl;
                } else if (cmd == "dump") {
                    std::string d = gw.dumpOrders();
                    if (arg.empty()) std::cout << d << std::flush;
                    else std::ofstream(arg) << d;
                } else if (cmd == "status") {
                    gw.inspect([](const OrderHandler& h) {
                        std::cout << h.orders().size() << " orders, " << h.openOrderCount() << " open\n";
                    });
                } else if (cmd == "quit") {
                    g_stop = true;
                } else if (!cmd.empty()) {
                    std::cout << "commands: kill C | unkill C | dump [file] | status | quit" << std::endl;
                }
            }
        });
        console.detach();  // blocked in getline if stdin stays open; fine at exit

        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::cout << "shutting down" << std::endl;
        acceptor->stop();
        if (!dumpPath.empty()) std::ofstream(dumpPath) << gw.dumpOrders();
        gw.stop();
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
