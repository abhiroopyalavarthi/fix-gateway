// End-to-end over real sockets: gateway acceptor + FixClient initiators in one process.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <thread>

#include "FixClient.h"
#include "FixLog.h"
#include "Gateway.h"
#include "quickfix/FileLog.h"
#include "quickfix/FileStore.h"
#include "quickfix/SocketInitiator.h"
#include "quickfix/ThreadedSocketAcceptor.h"
#include "quickfix/fix44/NewOrderSingle.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int nextPort() {
    static int port = 20000 + static_cast<int>(getpid() % 20000);
    return port++;
}

class SessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("fixgw-" + std::to_string(getpid()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        port_ = nextPort();

        cfg_.defaults = {10'000, 10'000'000'00, 1'000, 1'000'000};
        cfg_.symbols["AAPL"] = {100'00, 600'00, 255'00};
        gw_ = std::make_unique<Gateway>(cfg_);
        gw_->start();

        FIX::Dictionary d;
        d.setString("ConnectionType", "acceptor");
        d.setString("BeginString", "FIX.4.4");
        d.setString("SenderCompID", "GATEWAY");
        d.setInt("SocketAcceptPort", port_);
        d.setString("SocketReuseAddress", "Y");
        common(d, (dir_ / "gw").string());
        acceptorSettings_.set(d);
        for (const char* c : {"CLIENT1", "CLIENT2"}) {
            FIX::Dictionary s;
            s.setString("TargetCompID", c);
            acceptorSettings_.set(FIX::SessionID("FIX.4.4", "GATEWAY", c), s);
        }
        gwStore_ = std::make_unique<FIX::FileStoreFactory>(acceptorSettings_);
        gwLog_ = std::make_unique<FIX::FileLogFactory>(acceptorSettings_);
        acceptor_ = std::make_unique<FIX::ThreadedSocketAcceptor>(*gw_, *gwStore_, acceptorSettings_, *gwLog_);
        acceptor_->start();
    }

    void TearDown() override {
        clients_.clear();
        acceptor_->stop(true);
        gw_->stop();
        fs::remove_all(dir_);
    }

    void common(FIX::Dictionary& d, const std::string& root) {
        d.setString("StartTime", "00:00:00");
        d.setString("EndTime", "00:00:00");
        d.setInt("HeartBtInt", heartbeat_);
        d.setString("ResetOnLogon", "N");
        d.setString("ResetOnLogout", "N");
        d.setString("ResetOnDisconnect", "N");
        d.setString("UseDataDictionary", "Y");
        d.setString("DataDictionary", SPEC_DIR "/FIX44.xml");
        d.setString("FileStorePath", root + "/store");
        d.setString("FileLogPath", root + "/log");
        d.setString("SocketNodelay", "Y");
    }

    struct Client {
        FIX::SessionSettings settings;
        std::unique_ptr<FixClient> app;
        std::unique_ptr<FIX::FileStoreFactory> store;
        std::unique_ptr<FIX::FileLogFactory> log;
        std::unique_ptr<FIX::SocketInitiator> init;
        ~Client() {
            if (init) init->stop(true);
        }
    };

    Client& connect(const std::string& name) {
        auto c = std::make_unique<Client>();
        FIX::Dictionary d;
        d.setString("ConnectionType", "initiator");
        d.setString("BeginString", "FIX.4.4");
        d.setString("TargetCompID", "GATEWAY");
        d.setString("SocketConnectHost", "127.0.0.1");
        d.setInt("SocketConnectPort", port_);
        d.setInt("ReconnectInterval", 1);
        common(d, (dir_ / name).string());
        c->settings.set(d);
        FIX::Dictionary s;
        s.setString("SenderCompID", name);
        c->settings.set(FIX::SessionID("FIX.4.4", name, "GATEWAY"), s);
        c->app = std::make_unique<FixClient>((dir_ / (name + ".journal")).string());
        c->store = std::make_unique<FIX::FileStoreFactory>(c->settings);
        c->log = std::make_unique<FIX::FileLogFactory>(c->settings);
        c->init = std::make_unique<FIX::SocketInitiator>(*c->app, *c->store, c->settings, *c->log);
        c->init->start();
        EXPECT_TRUE(c->app->waitLogon(10s)) << name << " didn't log on";
        clients_[name] = std::move(c);
        return *clients_[name];
    }

    void disconnect(const std::string& name) {
        clients_[name]->init->stop(true);
        clients_.erase(name);
    }

    std::string gatewayLog(const std::string& client) {
        return (dir_ / "gw" / "log" / ("FIX.4.4-GATEWAY-" + client + ".messages.current.log")).string();
    }

    // the gateway's dump without the client column, only that client's orders
    std::string gatewayView(const std::string& client) {
        std::istringstream in(gw_->dumpOrders());
        std::string line, out;
        while (std::getline(in, line))
            if (line.rfind(client + " ", 0) == 0) out += line.substr(client.size() + 1) + "\n";
        return out;
    }

    fs::path dir_;
    int port_ = 0;
    int heartbeat_ = 30;
    GatewayConfig cfg_;
    std::unique_ptr<Gateway> gw_;
    FIX::SessionSettings acceptorSettings_;
    std::unique_ptr<FIX::FileStoreFactory> gwStore_;
    std::unique_ptr<FIX::FileLogFactory> gwLog_;
    std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor_;
    std::map<std::string, std::unique_ptr<Client>> clients_;
};

class HeartbeatTest : public SessionTest {
protected:
    void SetUp() override {
        heartbeat_ = 1;
        SessionTest::SetUp();
    }
};

}  // namespace

TEST_F(HeartbeatTest, LogonHeartbeatsAndCleanLogout) {
    connect("CLIENT1");
    std::this_thread::sleep_for(3500ms);
    disconnect("CLIENT1");
    std::this_thread::sleep_for(300ms);

    auto msgs = fixlog::load({gatewayLog("CLIENT1")});
    int logon = 0, hbIn = 0, hbOut = 0, logoutIn = 0, logoutOut = 0;
    for (auto& m : msgs) {
        bool in = m.get(49) == "CLIENT1";
        if (m.type() == "A") ++logon;
        if (m.type() == "0") (in ? hbIn : hbOut)++;
        if (m.type() == "5") (in ? logoutIn : logoutOut)++;
    }
    EXPECT_EQ(logon, 2);  // theirs and ours
    EXPECT_GE(hbIn, 2);
    EXPECT_GE(hbOut, 2);
    EXPECT_EQ(logoutIn, 1);
    EXPECT_EQ(logoutOut, 1);
    EXPECT_TRUE(fixlog::checkGaps(msgs).clean());
}

TEST_F(SessionTest, TwoClientsCrossOverFix) {
    auto& a = connect("CLIENT1");
    auto& b = connect("CLIENT2");
    a.app->sendNew("A-1", "AAPL", '1', 300, "255.00");
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->order("A-1").has_value(); }, 5s));
    b.app->sendNew("B-1", "AAPL", '2', 100, "254.90");
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->order("A-1")->cum == 100; }, 5s));
    ASSERT_TRUE(b.app->waitFor([&] { auto o = b.app->order("B-1"); return o && o->status == '2'; }, 5s));
    auto ao = *a.app->order("A-1");
    EXPECT_EQ(ao.status, '1');
    EXPECT_EQ(ao.leaves, 200);
    EXPECT_DOUBLE_EQ(ao.avgPx, 255.00);  // at the resting price
    EXPECT_DOUBLE_EQ(b.app->order("B-1")->avgPx, 255.00);

    // cancel the rest, then a cancel of a filled order gets a cancel reject
    a.app->sendCancel("A-1c", "A-1", "AAPL", '1');
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->order("A-1c").has_value(); }, 5s));
    EXPECT_EQ(a.app->order("A-1c")->status, '4');
    b.app->sendCancel("B-1c", "B-1", "AAPL", '2');
    ASSERT_TRUE(b.app->waitFor([&] { return !b.app->cancelRejects().empty(); }, 5s));
    EXPECT_EQ(b.app->cancelRejects()[0].reason, 0);
}

TEST_F(SessionTest, RiskRejectComesBackAsExecutionReport) {
    auto& a = connect("CLIENT1");
    a.app->sendNew("BIG", "AAPL", '1', 50'000, "255.00");
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->order("BIG").has_value(); }, 5s));
    auto o = *a.app->order("BIG");
    EXPECT_EQ(o.status, '8');
    EXPECT_EQ(o.lastExecType, '8');
    EXPECT_NE(o.text.find("exceeds max 10000"), std::string::npos);
}

TEST_F(SessionTest, KillSwitchOverFix) {
    auto& a = connect("CLIENT1");
    auto& b = connect("CLIENT2");
    for (int i = 0; i < 5; ++i) a.app->sendNew("A" + std::to_string(i), "AAPL", '1', 100, "250.00");
    b.app->sendNew("B0", "AAPL", '1', 100, "250.00");
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->orders().size() == 5; }, 5s));
    ASSERT_TRUE(b.app->waitFor([&] { return b.app->order("B0").has_value(); }, 5s));
    gw_->killSwitch("CLIENT1", true);
    ASSERT_TRUE(a.app->waitFor([&] {
        for (auto& [id, o] : a.app->orders()) if (o.status != '4') return false;
        return true;
    }, 5s));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(b.app->order("B0")->status, '0');  // untouched
}

TEST_F(SessionTest, RecoversAfterDisconnectWithoutLosingOrDuplicating) {
    // CLIENT1 rests 40 buys, drops off, CLIENT2 fills 25 of them while it's gone.
    // When CLIENT1 comes back, the gateway resends the fills it missed.
    auto& a = connect("CLIENT1");
    for (int i = 1; i <= 40; ++i) a.app->sendNew("A-" + std::to_string(i), "AAPL", '1', 100, "255.00");
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->orders().size() == 40; }, 10s));
    disconnect("CLIENT1");

    auto& b = connect("CLIENT2");
    for (int i = 1; i <= 25; ++i) b.app->sendNew("B-" + std::to_string(i), "AAPL", '2', 100, "255.00");
    ASSERT_TRUE(b.app->waitFor([&] { return b.app->orders().size() == 25 &&
                                            b.app->order("B-25")->status == '2'; }, 10s));

    // same store directory and journal: a restarted client
    auto& a2 = connect("CLIENT1");
    ASSERT_TRUE(a2.app->waitFor([&] {
        int filled = 0;
        for (auto& [id, o] : a2.app->orders()) filled += o.status == '2';
        return filled == 25;
    }, 10s)) << a2.app->view();
    std::this_thread::sleep_for(300ms);

    EXPECT_GE(a2.app->resentReports(), 25u);  // came back as PossDup resends
    EXPECT_EQ(a2.app->orders().size(), 40u);
    EXPECT_EQ(a2.app->view(), gatewayView("CLIENT1"));  // client and gateway agree on every order
    int bookedByGateway = 0;
    gw_->inspect([&](const OrderHandler& h) {
        for (auto& [id, o] : h.orders()) bookedByGateway += o.client == "CLIENT1";
    });
    EXPECT_EQ(bookedByGateway, 40);  // nothing booked twice

    auto gaps = fixlog::checkGaps(fixlog::load({gatewayLog("CLIENT1")}));
    EXPECT_TRUE(gaps.clean());
}

TEST_F(SessionTest, PossResendOfKnownOrderIsNotBookedTwice) {
    auto& a = connect("CLIENT1");
    a.app->sendNew("X-1", "AAPL", '1', 100, "250.00");
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->order("X-1").has_value(); }, 5s));
    a.app->sendNew("X-1", "AAPL", '1', 100, "250.00", '0', true);
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->order("X-1")->lastExecType == 'I'; }, 5s));
    gw_->inspect([](const OrderHandler& h) { EXPECT_EQ(h.orders().size(), 1u); });
}

TEST_F(SessionTest, MalformedMessagesDontTakeTheGatewayDown) {
    auto& a = connect("CLIENT1");

    // 1. a NewOrderSingle with no Side: rejected at the session layer (35=3)
    FIX::Message bad;
    bad.getHeader().setField(FIX::MsgType("D"));
    bad.setField(FIX::ClOrdID("NOSIDE"));
    bad.setField(FIX::Symbol("AAPL"));
    bad.setField(FIX::TransactTime());
    bad.setField(FIX::OrdType('2'));
    bad.setField(FIX::OrderQty(100));
    bad.setField(FIX::Price(255));
    FIX::Session::sendToTarget(bad, FIX::SessionID("FIX.4.4", "CLIENT1", "GATEWAY"));
    ASSERT_TRUE(a.app->waitFor([&] { return !a.app->sessionRejects().empty(); }, 5s));
    EXPECT_NE(a.app->sessionRejects()[0].find("Required tag missing"), std::string::npos);

    // 2. garbage and a bad checksum on a separate raw TCP connection
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    std::string junk = "hello\x01" "8=FIX.4.4\x01" "9=5\x01" "35=A\x01" "10=999\x01";
    ASSERT_GT(::send(fd, junk.data(), junk.size(), 0), 0);
    std::this_thread::sleep_for(300ms);
    close(fd);

    // 3. the gateway is still up and still trading
    a.app->sendNew("AFTER", "AAPL", '1', 100, "255.00");
    ASSERT_TRUE(a.app->waitFor([&] { return a.app->order("AFTER").has_value(); }, 5s));
    EXPECT_EQ(a.app->order("AFTER")->status, '0');
    gw_->inspect([](const OrderHandler& h) {
        EXPECT_EQ(h.orders().size(), 1u);  // the bad one never became an order
    });
}
