#include <gtest/gtest.h>

#include "Price.h"

TEST(Price, ParsesExactly) {
    EXPECT_EQ(price::parse("190.25"), 19025);
    EXPECT_EQ(price::parse("190"), 19000);
    EXPECT_EQ(price::parse("190.2"), 19020);
    EXPECT_EQ(price::parse("190.2500"), 19025);  // zeros past the grid are fine
    EXPECT_EQ(price::parse("0.01"), 1);
    EXPECT_EQ(price::parse(".5"), 50);
}

TEST(Price, RejectsOffGridAndGarbage) {
    EXPECT_FALSE(price::parse("190.255"));  // half a cent
    EXPECT_FALSE(price::parse("190.2501"));
    EXPECT_FALSE(price::parse(""));
    EXPECT_FALSE(price::parse("-5.00"));
    EXPECT_FALSE(price::parse("0"));
    EXPECT_FALSE(price::parse("0.00"));
    EXPECT_FALSE(price::parse("1e3"));
    EXPECT_FALSE(price::parse("12.3.4"));
    EXPECT_FALSE(price::parse("abc"));
    EXPECT_FALSE(price::parse("."));
}

TEST(Price, Formats) {
    EXPECT_EQ(price::format(19025), "190.25");
    EXPECT_EQ(price::format(19005), "190.05");
    EXPECT_EQ(price::format(19000), "190.00");
    EXPECT_EQ(price::format(7), "0.07");
}
