#include <gtest/gtest.h>

#include "FixLog.h"

// A hand-written log in QuickFIX's format ('|' instead of SOH to keep it readable).
static const char* kLog = R"(
20260930-14:00:00.000001 : 8=FIX.4.4|9=0|35=A|34=1|49=CLIENT1|56=GATEWAY|98=0|108=30|10=000|
20260930-14:00:00.000002 : 8=FIX.4.4|9=0|35=A|34=1|49=GATEWAY|56=CLIENT1|98=0|108=30|10=000|
20260930-14:00:01.000000 : 8=FIX.4.4|9=0|35=D|34=2|49=CLIENT1|56=GATEWAY|11=ORD-1|55=AAPL|54=1|38=100|40=2|44=255.00|10=000|
20260930-14:00:01.000100 : 8=FIX.4.4|9=0|35=8|34=2|49=GATEWAY|56=CLIENT1|37=O1|17=E1|150=0|39=0|11=ORD-1|55=AAPL|54=1|38=100|44=255.00|14=0|151=100|6=0|10=000|
20260930-14:00:02.000000 : 8=FIX.4.4|9=0|35=D|34=3|49=CLIENT1|56=GATEWAY|11=ORD-2|55=AAPL|54=1|38=90000|40=2|44=255.00|10=000|
20260930-14:00:02.000100 : 8=FIX.4.4|9=0|35=8|34=3|49=GATEWAY|56=CLIENT1|37=O2|17=E2|150=8|39=8|11=ORD-2|55=AAPL|54=1|38=90000|14=0|151=0|6=0|58=order qty 90000 exceeds max 50000|10=000|
20260930-14:00:03.000000 : 8=FIX.4.4|9=0|35=G|34=4|49=CLIENT1|56=GATEWAY|11=ORD-1b|41=ORD-1|55=AAPL|54=1|38=100|40=2|44=255.10|10=000|
20260930-14:00:03.000100 : 8=FIX.4.4|9=0|35=8|34=4|49=GATEWAY|56=CLIENT1|37=O1|17=E3|150=5|39=0|11=ORD-1b|41=ORD-1|55=AAPL|54=1|38=100|44=255.10|14=0|151=100|6=0|10=000|
20260930-14:00:04.000100 : 8=FIX.4.4|9=0|35=8|34=7|49=GATEWAY|56=CLIENT1|37=O1|17=E6|150=F|39=2|11=ORD-1b|55=AAPL|54=1|38=100|44=255.10|32=100|31=255.10|14=100|151=0|6=255.10|10=000|
20260930-14:00:05.000000 : 8=FIX.4.4|9=0|35=2|34=5|49=CLIENT1|56=GATEWAY|7=5|16=6|10=000|
20260930-14:00:05.000100 : 8=FIX.4.4|9=0|35=4|34=5|43=Y|49=GATEWAY|56=CLIENT1|123=Y|36=7|10=000|
20260930-14:00:06.000000 : 8=FIX.4.4|9=0|35=F|34=6|49=CLIENT1|56=GATEWAY|11=CXL-1|41=ORD-1b|55=AAPL|54=1|10=000|
20260930-14:00:06.000100 : 8=FIX.4.4|9=0|35=9|34=8|49=GATEWAY|56=CLIENT1|37=O1|11=CXL-1|41=ORD-1b|39=2|434=1|102=0|58=too late to cancel: order is filled|10=000|
20260930-14:00:07.000000 : 8=FIX.4.4|9=0|35=D|34=7|49=CLIENT1|56=GATEWAY|11=ORD-3|55=AAPL|54=1|38=100|40=2|44=250.00|10=000|
20260930-14:00:07.000100 : 8=FIX.4.4|9=0|35=8|34=9|49=GATEWAY|56=CLIENT1|37=O3|17=E7|150=0|39=0|11=ORD-3|55=AAPL|54=1|38=100|44=250.00|14=0|151=100|6=0|10=000|
20260930-14:00:08.000100 : 8=FIX.4.4|9=0|35=8|34=10|49=GATEWAY|56=CLIENT1|37=O4|17=E8|150=F|39=2|11=ORD-9|55=AAPL|54=1|38=10|44=256.00|32=10|31=255.50|14=10|151=0|6=255.50|10=000|
)";

TEST(FixLog, ParsesQuickFixLines) {
    auto m = fixlog::parseLine("20260930-14:00:01.000000 : 8=FIX.4.4\x01" "35=D\x01" "11=A\x01");
    ASSERT_TRUE(m);
    EXPECT_EQ(m->time, "20260930-14:00:01.000000");
    EXPECT_EQ(m->type(), "D");
    EXPECT_EQ(m->get(11), "A");
    EXPECT_FALSE(fixlog::parseLine("20260930 some event line"));
}

TEST(FixLog, OrderHistoryFollowsReplaceAndCancel) {
    auto msgs = fixlog::loadText(kLog);
    auto hist = fixlog::orderHistory(msgs, "ORD-1");
    // D, ack, G, replaced, fill, cancel request, cancel reject
    ASSERT_EQ(hist.size(), 7u);
    EXPECT_EQ(hist.front()->type(), "D");
    EXPECT_EQ(hist.back()->type(), "9");
}

TEST(FixLog, ExplainsReject) {
    auto msgs = fixlog::loadText(kLog);
    auto why = fixlog::explain(msgs, "ORD-2");
    EXPECT_NE(why.find("REJECTED"), std::string::npos);
    EXPECT_NE(why.find("exceeds max 50000"), std::string::npos);
}

TEST(FixLog, ExplainsFillAndRefusedCancel) {
    auto msgs = fixlog::loadText(kLog);
    auto why = fixlog::explain(msgs, "ORD-1");
    EXPECT_NE(why.find("FILLED"), std::string::npos);
    EXPECT_NE(why.find("too late to cancel"), std::string::npos);
    EXPECT_NE(why.find("Replace requested"), std::string::npos);
}

TEST(FixLog, ExplainsCancelByTheRightRequest) {
    std::string log =
        "t1 : 8=FIX.4.4|35=D|34=1|49=C|56=G|11=O|54=1|38=100|40=2|44=10.00|55=X|\n"
        "t2 : 8=FIX.4.4|35=8|34=1|49=G|56=C|37=O1|11=O|150=0|39=0|14=0|151=100|55=X|44=10.00|\n"
        "t3 : 8=FIX.4.4|35=F|34=2|49=C|56=G|11=CX1|41=O|55=X|\n"
        "t4 : 8=FIX.4.4|35=8|34=2|49=G|56=C|37=O1|11=CX1|41=O|150=4|39=4|14=0|151=0|55=X|\n"
        "t5 : 8=FIX.4.4|35=F|34=3|49=C|56=G|11=CX2|41=O|55=X|\n"
        "t6 : 8=FIX.4.4|35=9|34=3|49=G|56=C|37=O1|11=CX2|41=O|39=4|434=1|58=too late|\n";
    auto why = fixlog::explain(fixlog::loadText(log), "O");
    EXPECT_NE(why.find("cancel request CX1"), std::string::npos) << why;
}

TEST(FixLog, ExplainsWhyNotFilled) {
    auto msgs = fixlog::loadText(kLog);
    auto why = fixlog::explain(msgs, "ORD-3");
    EXPECT_NE(why.find("STILL WORKING"), std::string::npos);
    EXPECT_NE(why.find("above the 250.00 limit"), std::string::npos);
    EXPECT_NE(fixlog::explain(msgs, "NOPE").find("No message"), std::string::npos);
}

TEST(FixLog, GapFilledByResendIsNotAProblem) {
    auto msgs = fixlog::loadText(kLog);
    auto g = fixlog::checkGaps(msgs);
    // gateway jumped 4 -> 7, then gap-filled 5..6 with PossDup
    EXPECT_TRUE(g.clean()) << g.findings.front();
    bool sawGap = false, sawResend = false;
    for (auto& n : g.notes) {
        sawGap |= n.find("expected 5, got 7") != std::string::npos;
        sawResend |= n.find("asked GATEWAY to resend 5..6") != std::string::npos;
    }
    EXPECT_TRUE(sawGap);
    EXPECT_TRUE(sawResend);
}

TEST(FixLog, UnfilledGapIsAProblem) {
    std::string log =
        "t1 : 8=FIX.4.4|35=0|34=1|49=A|56=B|\n"
        "t2 : 8=FIX.4.4|35=0|34=2|49=A|56=B|\n"
        "t3 : 8=FIX.4.4|35=0|34=6|49=A|56=B|\n";
    auto g = fixlog::checkGaps(fixlog::loadText(log));
    ASSERT_FALSE(g.clean());
    EXPECT_NE(g.findings[0].find("3..5 never arrived"), std::string::npos);
}

TEST(FixLog, ListsRejects) {
    auto r = fixlog::rejects(fixlog::loadText(kLog));
    ASSERT_EQ(r.size(), 2u);
    EXPECT_NE(r[0].find("ORD-2 rejected"), std::string::npos);
    EXPECT_NE(r[1].find("cancel CXL-1 of ORD-1b refused"), std::string::npos);
}
