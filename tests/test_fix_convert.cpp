#include <gtest/gtest.h>

#include "FixConvert.h"
#include "quickfix/DataDictionary.h"

TEST(FixConvert, NewOrderSingleToInternal) {
    FIX44::NewOrderSingle m(FIX::ClOrdID("ORD-1001"), FIX::Side(FIX::Side_BUY), FIX::TransactTime(),
                            FIX::OrdType(FIX::OrdType_LIMIT));
    m.set(FIX::Symbol("AAPL"));
    m.set(FIX::OrderQty(100));
    m.setField(FIX::FIELD::Price, "190.25");
    m.set(FIX::TimeInForce(FIX::TimeInForce_IMMEDIATE_OR_CANCEL));
    NewOrder o = fixconv::toNewOrder(m, "CLIENT1");
    EXPECT_EQ(o.client, "CLIENT1");
    EXPECT_EQ(o.clOrdId, "ORD-1001");
    EXPECT_EQ(o.symbol, "AAPL");
    EXPECT_EQ(o.side, Side::Buy);
    EXPECT_EQ(o.kind, OrdKind::Limit);
    EXPECT_EQ(o.tif, Tif::IOC);
    EXPECT_EQ(o.priceText, "190.25");  // text is kept exact for the tick check
    EXPECT_EQ(o.qty, 100);
    EXPECT_TRUE(o.parseError.empty());
    EXPECT_FALSE(o.possResend);
}

TEST(FixConvert, UnsupportedValuesBecomeParseErrors) {
    FIX44::NewOrderSingle m(FIX::ClOrdID("X"), FIX::Side('5'), FIX::TransactTime(), FIX::OrdType('3'));
    m.set(FIX::OrderQty(10));
    NewOrder o = fixconv::toNewOrder(m, "C");
    EXPECT_NE(o.parseError.find("side"), std::string::npos);

    FIX44::NewOrderSingle stop(FIX::ClOrdID("Y"), FIX::Side('1'), FIX::TransactTime(), FIX::OrdType('3'));
    stop.set(FIX::OrderQty(10));
    EXPECT_NE(fixconv::toNewOrder(stop, "C").parseError.find("OrdType"), std::string::npos);

    FIX44::NewOrderSingle frac(FIX::ClOrdID("Z"), FIX::Side('1'), FIX::TransactTime(), FIX::OrdType('2'));
    frac.setField(FIX::FIELD::OrderQty, "10.5");
    frac.setField(FIX::FIELD::Price, "10.00");
    EXPECT_NE(fixconv::toNewOrder(frac, "C").parseError.find("OrderQty"), std::string::npos);
}

TEST(FixConvert, ResendFlags) {
    FIX44::NewOrderSingle m(FIX::ClOrdID("X"), FIX::Side('1'), FIX::TransactTime(), FIX::OrdType('1'));
    EXPECT_FALSE(fixconv::isResend(m));
    m.getHeader().setField(FIX::PossResend(true));
    EXPECT_TRUE(fixconv::isResend(m));
    FIX44::NewOrderSingle n(FIX::ClOrdID("X"), FIX::Side('1'), FIX::TransactTime(), FIX::OrdType('1'));
    n.getHeader().setField(FIX::PossDupFlag(true));
    EXPECT_TRUE(fixconv::isResend(n));
}

TEST(FixConvert, ExecutionReportFieldsAndValidates) {
    ExecReport r;
    r.orderId = "O7";
    r.execId = "E9";
    r.clOrdId = "ORD-1";
    r.execType = exec::Trade;
    r.ordStatus = status::PartiallyFilled;
    r.symbol = "AAPL";
    r.side = Side::Sell;
    r.price = 19025;
    r.orderQty = 300;
    r.lastQty = 100;
    r.lastPx = 19025;
    r.cumQty = 100;
    r.leavesQty = 200;
    r.avgPx = 190.25;
    auto m = fixconv::toFix(r);
    EXPECT_EQ(m.getField(FIX::FIELD::Price), "190.25");
    EXPECT_EQ(m.getField(FIX::FIELD::LastPx), "190.25");
    EXPECT_EQ(m.getField(FIX::FIELD::ExecType), "F");
    EXPECT_EQ(m.getField(FIX::FIELD::OrdStatus), "1");
    EXPECT_EQ(m.getField(FIX::FIELD::Side), "2");
    EXPECT_EQ(std::stod(m.getField(FIX::FIELD::CumQty)), 100);
    EXPECT_EQ(std::stod(m.getField(FIX::FIELD::LeavesQty)), 200);

    // it has every field FIX 4.4 requires for an ExecutionReport
    FIX::DataDictionary dd(SPEC_DIR "/FIX44.xml");
    m.getHeader().setField(FIX::BeginString("FIX.4.4"));
    m.getHeader().setField(FIX::SenderCompID("GATEWAY"));
    m.getHeader().setField(FIX::TargetCompID("CLIENT1"));
    m.getHeader().setField(FIX::MsgSeqNum(1));
    m.getHeader().setField(FIX::SendingTime());
    try {
        dd.validate(FIX::Message(m.toString(), dd));  // toString fills in 9 and 10
    } catch (const std::exception& e) {
        ADD_FAILURE() << e.what();
    }
}

TEST(FixConvert, CancelRejectValidates) {
    CancelReject c;
    c.orderId = "NONE";
    c.clOrdId = "C1";
    c.origClOrdId = "X";
    c.reason = 1;
    c.text = "unknown order X";
    auto m = fixconv::toFix(c);
    EXPECT_EQ(m.getField(FIX::FIELD::CxlRejReason), "1");
    FIX::DataDictionary dd(SPEC_DIR "/FIX44.xml");
    m.getHeader().setField(FIX::BeginString("FIX.4.4"));
    m.getHeader().setField(FIX::SenderCompID("GATEWAY"));
    m.getHeader().setField(FIX::TargetCompID("CLIENT1"));
    m.getHeader().setField(FIX::MsgSeqNum(1));
    m.getHeader().setField(FIX::SendingTime());
    try {
        dd.validate(FIX::Message(m.toString(), dd));  // toString fills in 9 and 10
    } catch (const std::exception& e) {
        ADD_FAILURE() << e.what();
    }
}
