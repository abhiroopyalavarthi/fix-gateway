#include <gtest/gtest.h>

#include "RiskChecks.h"

namespace {
GatewayConfig cfg() {
    GatewayConfig c;
    c.defaults = {1000, 100'000'00, 500, 5000};  // qty, $100k notional, 5% band, 5000 position
    c.symbols["AAPL"] = {100'00, 600'00, 250'00};
    return c;
}
}  // namespace

// each rule: accepted exactly at the limit, rejected one past it

TEST(Risk, MaxOrderQty) {
    auto c = cfg();
    RiskChecks r(c);
    EXPECT_FALSE(r.check("C", Side::Buy, true, 50'00, 50'00, 1000, {}));
    auto why = r.check("C", Side::Buy, true, 50'00, 50'00, 1001, {});
    ASSERT_TRUE(why);
    EXPECT_NE(why->find("exceeds max 1000"), std::string::npos);
}

TEST(Risk, MaxNotional) {
    auto c = cfg();
    c.defaults.maxOrderQty = 1'000'000;
    RiskChecks r(c);
    // 400 x $250.00 = $100,000.00 exactly
    EXPECT_FALSE(r.check("C", Side::Buy, true, 250'00, 250'00, 400, {}));
    // one cent more on the price
    auto why = r.check("C", Side::Buy, true, 250'01, 250'00, 400, {});
    ASSERT_TRUE(why);
    EXPECT_NE(why->find("notional"), std::string::npos);
}

TEST(Risk, PriceBand) {
    auto c = cfg();
    RiskChecks r(c);
    // 5% of 250.00 = 12.50
    EXPECT_FALSE(r.check("C", Side::Buy, true, 262'50, 250'00, 1, {}));
    EXPECT_FALSE(r.check("C", Side::Buy, true, 237'50, 250'00, 1, {}));
    EXPECT_TRUE(r.check("C", Side::Buy, true, 262'51, 250'00, 1, {}));
    EXPECT_TRUE(r.check("C", Side::Sell, true, 237'49, 250'00, 1, {}));
    // market orders have no limit price, so no band
    EXPECT_FALSE(r.check("C", Side::Buy, false, 250'00, 250'00, 1, {}));
}

TEST(Risk, PositionLimitCountsRestingOrders) {
    auto c = cfg();
    c.defaults.maxOrderQty = 1'000'000;
    c.defaults.maxNotional = 1'000'000'000'00;
    RiskChecks r(c);
    Exposure e{4000, 500, 0};  // long 4000, 500 more resting on the bid
    EXPECT_FALSE(r.check("C", Side::Buy, true, 250'00, 250'00, 500, e));  // 5000
    EXPECT_TRUE(r.check("C", Side::Buy, true, 250'00, 250'00, 501, e));   // 5001
    // selling reduces the long, so it's fine even though it's big
    EXPECT_FALSE(r.check("C", Side::Sell, true, 250'00, 250'00, 9000, e));  // -5000
    EXPECT_TRUE(r.check("C", Side::Sell, true, 250'00, 250'00, 9001, e));
}

TEST(Risk, KillSwitch) {
    auto c = cfg();
    RiskChecks r(c);
    r.setKill("C", true);
    auto why = r.check("C", Side::Buy, true, 250'00, 250'00, 1, {});
    ASSERT_TRUE(why);
    EXPECT_NE(why->find("kill switch"), std::string::npos);
    EXPECT_FALSE(r.check("OTHER", Side::Buy, true, 250'00, 250'00, 1, {}));
    r.setKill("C", false);
    EXPECT_FALSE(r.check("C", Side::Buy, true, 250'00, 250'00, 1, {}));
}

TEST(Risk, ConfigFile) {
    auto c = GatewayConfig::parse(R"(
        max_order_qty = 10    # comment
        max_notional = 1000.50
        price_band_bps = 200
        max_position = 30
        client.BIG.max_order_qty = 99
        symbol AAPL 100.00 600.00 255.25
    )");
    EXPECT_EQ(c.defaults.maxOrderQty, 10);
    EXPECT_EQ(c.defaults.maxNotional, 100050);
    EXPECT_EQ(c.limitsFor("BIG").maxOrderQty, 99);
    EXPECT_EQ(c.limitsFor("BIG").maxPosition, 30);  // override starts from the defaults
    EXPECT_EQ(c.limitsFor("ANYONE").maxOrderQty, 10);
    EXPECT_EQ(c.symbols.at("AAPL").refPrice, 25525);
    EXPECT_THROW(GatewayConfig::parse("nonsense = 1"), std::runtime_error);
    EXPECT_THROW(GatewayConfig::parse("symbol AAPL 1.00"), std::runtime_error);
}

TEST(Risk, ShippedConfigLoads) {
    auto c = GatewayConfig::load(CONFIG_DIR "/risk.cfg");
    EXPECT_GE(c.symbols.size(), 5u);
    EXPECT_GT(c.limitsFor("LP").maxOrderQty, c.defaults.maxOrderQty);
}
