#include <gtest/gtest.h>

#include <variant>
#include <vector>

#include "OrderHandler.h"

// OrderHandler without any FIX: requests in, execution reports out.

namespace {

struct Harness {
    GatewayConfig cfg;
    std::vector<Outbound> out;
    std::unique_ptr<OrderHandler> h;

    Harness() {
        cfg.defaults = {10'000, 10'000'000'00, 1'000, 50'000};
        cfg.symbols["AAPL"] = {100'00, 600'00, 250'00};
        h = std::make_unique<OrderHandler>(cfg, [this](const Outbound& o) { out.push_back(o); });
    }

    void add(const std::string& client, const std::string& id, Side side, int64_t qty,
             const std::string& px, Tif tif = Tif::Day, bool possResend = false) {
        NewOrder o;
        o.client = client;
        o.clOrdId = id;
        o.symbol = "AAPL";
        o.side = side;
        o.kind = px.empty() ? OrdKind::Market : OrdKind::Limit;
        o.tif = tif;
        o.priceText = px;
        o.qty = qty;
        o.possResend = possResend;
        h->onNew(o);
    }
    void cancel(const std::string& client, const std::string& id, const std::string& orig) {
        h->onCancel({client, id, orig, "AAPL", Side::Buy});
    }
    void replace(const std::string& client, const std::string& id, const std::string& orig, Side side,
                 int64_t qty, const std::string& px) {
        ReplaceRequest r;
        r.client = client;
        r.clOrdId = id;
        r.origClOrdId = orig;
        r.symbol = "AAPL";
        r.side = side;
        r.qty = qty;
        r.priceText = px;
        h->onReplace(r);
    }

    std::vector<ExecReport> reports(const std::string& client = "") const {
        std::vector<ExecReport> r;
        for (auto& o : out)
            if (auto* e = std::get_if<ExecReport>(&o))
                if (client.empty() || e->client == client) r.push_back(*e);
        return r;
    }
    std::vector<CancelReject> cancelRejects() const {
        std::vector<CancelReject> r;
        for (auto& o : out)
            if (auto* e = std::get_if<CancelReject>(&o)) r.push_back(*e);
        return r;
    }
    ExecReport last() const { return reports().back(); }
};

}  // namespace

TEST(OrderHandler, NewOrderIsAcked) {
    Harness t;
    t.add("A", "1", Side::Buy, 100, "250.00");
    ASSERT_EQ(t.out.size(), 1u);
    auto r = t.last();
    EXPECT_EQ(r.execType, exec::New);
    EXPECT_EQ(r.ordStatus, status::New);
    EXPECT_EQ(r.leavesQty, 100);
    EXPECT_EQ(r.cumQty, 0);
    EXPECT_EQ(r.price, 25000);
    EXPECT_EQ(r.orderId, "O1");
}

TEST(OrderHandler, TwoClientsCrossAndBothGetFills) {
    Harness t;
    t.add("A", "a1", Side::Sell, 100, "250.00");
    t.add("B", "b1", Side::Buy, 100, "250.05");  // crosses, trades at the resting price
    auto a = t.reports("A"), b = t.reports("B");
    ASSERT_EQ(a.size(), 2u);  // New, Trade
    ASSERT_EQ(b.size(), 2u);
    for (auto* r : {&a.back(), &b.back()}) {
        EXPECT_EQ(r->execType, exec::Trade);
        EXPECT_EQ(r->ordStatus, status::Filled);
        EXPECT_EQ(r->lastQty, 100);
        EXPECT_EQ(r->lastPx, 25000);
        EXPECT_EQ(r->cumQty, 100);
        EXPECT_EQ(r->leavesQty, 0);
        EXPECT_DOUBLE_EQ(r->avgPx, 250.00);
    }
    EXPECT_EQ(t.h->lastTrade("AAPL"), 25000);
}

TEST(OrderHandler, CumPlusLeavesEqualsQtyInEveryReport) {
    Harness t;
    t.add("A", "big", Side::Buy, 1000, "250.00");
    int i = 0;
    for (int q : {100, 250, 50, 300, 300}) t.add("B", "s" + std::to_string(i++), Side::Sell, q, "250.00");
    int trades = 0;
    for (auto& r : t.reports("A")) {
        EXPECT_EQ(r.cumQty + r.leavesQty, r.orderQty) << "exec " << r.execId;
        if (r.execType == exec::Trade) ++trades;
    }
    EXPECT_EQ(trades, 5);
    EXPECT_EQ(t.reports("A").back().ordStatus, status::Filled);
    // partial fills in between said PartiallyFilled
    EXPECT_EQ(t.reports("A")[1].ordStatus, status::PartiallyFilled);
}

TEST(OrderHandler, AvgPxAcrossThreePrices) {
    Harness t;
    t.add("S", "s1", Side::Sell, 100, "250.00");
    t.add("S", "s2", Side::Sell, 200, "250.10");
    t.add("S", "s3", Side::Sell, 100, "250.25");
    t.add("B", "b", Side::Buy, 400, "250.25");
    auto r = t.reports("B").back();
    EXPECT_EQ(r.cumQty, 400);
    // (100*250.00 + 200*250.10 + 100*250.25) / 400 = 250.1125
    EXPECT_DOUBLE_EQ(r.avgPx, 250.1125);
}

TEST(OrderHandler, DuplicateClOrdIdRejected) {
    Harness t;
    t.add("A", "1", Side::Buy, 100, "250.00");
    t.add("A", "1", Side::Buy, 100, "250.00");
    auto r = t.last();
    EXPECT_EQ(r.execType, exec::Rejected);
    EXPECT_NE(r.text.find("duplicate ClOrdID"), std::string::npos);
    // the same id from a different client is a different order
    t.add("B", "1", Side::Buy, 100, "250.00");
    EXPECT_EQ(t.last().execType, exec::New);
}

TEST(OrderHandler, PossibleResendIsNotBookedTwice) {
    Harness t;
    t.add("A", "1", Side::Buy, 100, "250.00");
    t.add("A", "1", Side::Buy, 100, "250.00", Tif::Day, true);
    EXPECT_EQ(t.last().execType, exec::OrderStatus);
    EXPECT_EQ(t.last().leavesQty, 100);
    EXPECT_EQ(t.h->openOrderCount(), 1u);
}

TEST(OrderHandler, OffTickPriceRejected) {
    Harness t;
    t.add("A", "1", Side::Buy, 100, "250.005");
    EXPECT_EQ(t.last().execType, exec::Rejected);
    EXPECT_NE(t.last().text.find("tick grid"), std::string::npos);
}

TEST(OrderHandler, UnknownSymbolAndBadQtyRejected) {
    Harness t;
    NewOrder o;
    o.client = "A";
    o.clOrdId = "x";
    o.symbol = "ZZZZ";
    o.priceText = "10.00";
    o.qty = 10;
    t.h->onNew(o);
    EXPECT_NE(t.last().text.find("unknown symbol"), std::string::npos);
    t.add("A", "y", Side::Buy, 0, "250.00");
    EXPECT_NE(t.last().text.find("positive"), std::string::npos);
    t.add("A", "z", Side::Buy, -5, "250.00");
    EXPECT_EQ(t.last().execType, exec::Rejected);
}

TEST(OrderHandler, CancelOpenOrder) {
    Harness t;
    t.add("A", "1", Side::Buy, 100, "249.00");
    t.cancel("A", "c1", "1");
    auto r = t.last();
    EXPECT_EQ(r.execType, exec::Canceled);
    EXPECT_EQ(r.ordStatus, status::Canceled);
    EXPECT_EQ(r.clOrdId, "c1");
    EXPECT_EQ(r.origClOrdId, "1");
    EXPECT_EQ(r.leavesQty, 0);
    EXPECT_EQ(t.h->openOrderCount(), 0u);
    // really gone from the book: a seller doesn't trade with it
    t.add("B", "s", Side::Sell, 100, "249.00");
    EXPECT_EQ(t.reports("B").back().execType, exec::New);
}

TEST(OrderHandler, CancelOfFilledOrderGetsCancelReject) {
    Harness t;
    t.add("A", "1", Side::Buy, 100, "250.00");
    t.add("B", "s", Side::Sell, 100, "250.00");
    size_t reportsBefore = t.reports().size();
    t.cancel("A", "c1", "1");
    ASSERT_EQ(t.cancelRejects().size(), 1u);
    EXPECT_EQ(t.reports().size(), reportsBefore);  // no execution report for it
    auto cr = t.cancelRejects()[0];
    EXPECT_EQ(cr.reason, 0);  // too late
    EXPECT_EQ(cr.ordStatus, status::Filled);
    EXPECT_NE(cr.text.find("too late"), std::string::npos);
}

TEST(OrderHandler, CancelUnknownOrder) {
    Harness t;
    t.cancel("A", "c1", "nope");
    ASSERT_EQ(t.cancelRejects().size(), 1u);
    EXPECT_EQ(t.cancelRejects()[0].reason, 1);
    EXPECT_EQ(t.cancelRejects()[0].orderId, "NONE");
}

TEST(OrderHandler, PartialFillThenCancel) {
    Harness t;
    t.add("A", "1", Side::Buy, 500, "250.00");
    t.add("B", "s", Side::Sell, 200, "250.00");
    t.cancel("A", "c1", "1");
    auto a = t.reports("A");
    ASSERT_EQ(a.size(), 3u);  // New, Trade (partial), Canceled
    EXPECT_EQ(a[0].execType, exec::New);
    EXPECT_EQ(a[1].execType, exec::Trade);
    EXPECT_EQ(a[1].ordStatus, status::PartiallyFilled);
    EXPECT_EQ(a[1].cumQty, 200);
    EXPECT_EQ(a[1].leavesQty, 300);
    EXPECT_EQ(a[2].execType, exec::Canceled);
    EXPECT_EQ(a[2].cumQty, 200);  // keeps what it filled
    EXPECT_EQ(a[2].leavesQty, 0);
}

TEST(OrderHandler, ReplacePriceLosesTimePriority) {
    Harness t;
    t.add("A", "a", Side::Sell, 100, "251.00");
    t.add("B", "b", Side::Sell, 100, "250.50");
    // A moves to 250.50, behind B
    t.replace("A", "a2", "a", Side::Sell, 100, "250.50");
    EXPECT_EQ(t.reports("A").back().execType, exec::Replaced);
    EXPECT_EQ(t.reports("A").back().origClOrdId, "a");
    t.add("C", "buy", Side::Buy, 100, "250.50");
    EXPECT_EQ(t.reports("B").back().execType, exec::Trade);  // B was first
    EXPECT_EQ(t.reports("A").back().execType, exec::Replaced);
}

TEST(OrderHandler, ReplaceQtyDownKeepsTimePriority) {
    Harness t;
    t.add("A", "a", Side::Sell, 300, "250.50");
    t.add("B", "b", Side::Sell, 100, "250.50");
    t.replace("A", "a2", "a", Side::Sell, 200, "250.50");
    auto rep = t.reports("A").back();
    EXPECT_EQ(rep.execType, exec::Replaced);
    EXPECT_EQ(rep.orderQty, 200);
    EXPECT_EQ(rep.leavesQty, 200);
    t.add("C", "buy", Side::Buy, 150, "250.50");
    // A is still first in line and takes all 150
    EXPECT_EQ(t.reports("A").back().execType, exec::Trade);
    EXPECT_EQ(t.reports("A").back().lastQty, 150);
    EXPECT_EQ(t.reports("B").back().execType, exec::New);
}

TEST(OrderHandler, ReplaceQtyUpLosesTimePriority) {
    Harness t;
    t.add("A", "a", Side::Sell, 100, "250.50");
    t.add("B", "b", Side::Sell, 100, "250.50");
    t.replace("A", "a2", "a", Side::Sell, 300, "250.50");
    t.add("C", "buy", Side::Buy, 100, "250.50");
    EXPECT_EQ(t.reports("B").back().execType, exec::Trade);
    EXPECT_EQ(t.reports("A").back().execType, exec::Replaced);
}

TEST(OrderHandler, ReplaceBelowFilledQtyRejected) {
    Harness t;
    t.add("A", "a", Side::Buy, 500, "250.00");
    t.add("B", "s", Side::Sell, 300, "250.00");
    t.replace("A", "a2", "a", Side::Buy, 200, "250.00");
    ASSERT_EQ(t.cancelRejects().size(), 1u);
    EXPECT_EQ(t.cancelRejects()[0].responseTo, '2');
    EXPECT_NE(t.cancelRejects()[0].text.find("below the 300 already filled"), std::string::npos);
    // the original order is untouched
    EXPECT_EQ(t.h->find("A", "a")->leaves(), 200);
}

TEST(OrderHandler, ReplaceDownToFilledQtyCompletesOrder) {
    Harness t;
    t.add("A", "a", Side::Buy, 500, "250.00");
    t.add("B", "s", Side::Sell, 300, "250.00");
    t.replace("A", "a2", "a", Side::Buy, 300, "250.00");
    auto r = t.reports("A").back();
    EXPECT_EQ(r.execType, exec::Replaced);
    EXPECT_EQ(r.ordStatus, status::Filled);
    EXPECT_EQ(r.leavesQty, 0);
    EXPECT_EQ(t.h->openOrderCount(), 0u);
}

TEST(OrderHandler, ReplaceThatCrossesTrades) {
    Harness t;
    t.add("A", "a", Side::Buy, 100, "249.00");
    t.add("B", "s", Side::Sell, 100, "250.00");
    t.replace("A", "a2", "a", Side::Buy, 100, "250.00");
    auto a = t.reports("A");
    EXPECT_EQ(a[a.size() - 2].execType, exec::Replaced);
    EXPECT_EQ(a.back().execType, exec::Trade);
    EXPECT_EQ(a.back().clOrdId, "a2");
}

TEST(OrderHandler, IocRemainderCanceled) {
    Harness t;
    t.add("S", "s", Side::Sell, 100, "250.00");
    t.add("B", "ioc", Side::Buy, 300, "250.00", Tif::IOC);
    auto b = t.reports("B");
    ASSERT_EQ(b.size(), 3u);  // New, Trade 100, Canceled
    EXPECT_EQ(b.back().execType, exec::Canceled);
    EXPECT_EQ(b.back().cumQty, 100);
    EXPECT_EQ(t.h->openOrderCount(), 0u);
}

TEST(OrderHandler, FokThatCantFillIsCanceledWithoutTrading) {
    Harness t;
    t.add("S", "s", Side::Sell, 100, "250.00");
    t.add("B", "fok", Side::Buy, 300, "250.00", Tif::FOK);
    auto b = t.reports("B");
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b.back().execType, exec::Canceled);
    EXPECT_EQ(b.back().cumQty, 0);
    EXPECT_EQ(t.reports("S").size(), 1u);  // seller untouched
}

TEST(OrderHandler, MarketOrderSweepsAndDropsRest) {
    Harness t;
    t.add("S", "s1", Side::Sell, 100, "250.00");
    t.add("S", "s2", Side::Sell, 100, "250.50");
    t.add("B", "m", Side::Buy, 300, "");
    auto b = t.reports("B");
    EXPECT_EQ(b.back().execType, exec::Canceled);
    EXPECT_EQ(b.back().cumQty, 200);
    EXPECT_DOUBLE_EQ(b.back().avgPx, 250.25);
}

TEST(OrderHandler, RiskRejectHasReasonAndNeverReachesBook) {
    Harness t;
    t.add("A", "big", Side::Buy, 10'001, "250.00");
    auto r = t.last();
    EXPECT_EQ(r.execType, exec::Rejected);
    EXPECT_EQ(r.ordStatus, status::Rejected);
    EXPECT_NE(r.text.find("exceeds max 10000"), std::string::npos);
    t.add("B", "s", Side::Sell, 100, "250.00");
    EXPECT_EQ(t.reports("B").back().execType, exec::New);  // nothing to trade against
}

TEST(OrderHandler, PriceBandUsesLastTrade) {
    Harness t;
    t.add("A", "far", Side::Buy, 10, "280.00");  // >10% above the 250.00 reference
    EXPECT_EQ(t.last().execType, exec::Rejected);
    EXPECT_NE(t.last().text.find("band"), std::string::npos);
    t.add("A", "ok", Side::Buy, 10, "274.00");
    EXPECT_EQ(t.last().execType, exec::New);
}

TEST(OrderHandler, PositionLimitIncludesFillsAndRestingOrders) {
    Harness t;
    t.cfg.defaults.maxPosition = 1000;
    t.add("A", "1", Side::Buy, 600, "250.00");
    t.add("S", "s", Side::Sell, 600, "250.00");  // A is now long 600
    t.add("A", "2", Side::Buy, 300, "249.00");   // 900 worst case
    EXPECT_EQ(t.last().execType, exec::New);
    t.add("A", "3", Side::Buy, 101, "249.00");   // 1001
    EXPECT_EQ(t.last().execType, exec::Rejected);
    t.add("A", "4", Side::Buy, 100, "249.00");   // 1000
    EXPECT_EQ(t.last().execType, exec::New);
}

TEST(OrderHandler, KillSwitchCancelsOnlyThatClient) {
    Harness t;
    t.add("A", "a1", Side::Buy, 100, "249.00");
    t.add("A", "a2", Side::Buy, 100, "248.00");
    t.add("B", "b1", Side::Buy, 100, "249.00");
    t.h->killSwitch({"A", true});
    auto a = t.reports("A");
    EXPECT_EQ(a[a.size() - 1].execType, exec::Canceled);
    EXPECT_EQ(a[a.size() - 2].execType, exec::Canceled);
    EXPECT_EQ(a.back().text, "kill switch");
    EXPECT_FALSE(t.h->find("A", "a1")->live);
    EXPECT_FALSE(t.h->find("A", "a2")->live);
    EXPECT_TRUE(t.h->find("B", "b1")->live);
    // and A can't send new orders until it's turned off
    t.add("A", "a3", Side::Buy, 100, "249.00");
    EXPECT_EQ(t.last().execType, exec::Rejected);
    EXPECT_NE(t.last().text.find("kill switch"), std::string::npos);
    t.h->killSwitch({"A", false});
    t.add("A", "a4", Side::Buy, 100, "249.00");
    EXPECT_EQ(t.last().execType, exec::New);
}

TEST(OrderHandler, ExposureStaysConsistent) {
    // after a mix of everything, open qty in the exposure table must equal what's resting
    Harness t;
    t.add("A", "1", Side::Buy, 500, "250.00");
    t.add("A", "2", Side::Buy, 300, "249.00");
    t.add("B", "s1", Side::Sell, 200, "250.00");
    t.replace("A", "2b", "2", Side::Buy, 100, "249.00");
    t.cancel("A", "c1", "1");
    t.add("A", "3", Side::Sell, 50, "251.00");
    auto e = t.h->exposure("A", "AAPL");
    int64_t openBuy = 0, openSell = 0;
    for (auto& [id, o] : t.h->orders())
        if (o.client == "A" && o.live) (o.side == Side::Buy ? openBuy : openSell) += o.leaves();
    EXPECT_EQ(e.openBuy, openBuy);
    EXPECT_EQ(e.openSell, openSell);
    EXPECT_EQ(e.position, 200);
    EXPECT_EQ(openBuy, 100);
    EXPECT_EQ(openSell, 50);
}
