/**
    Baisc DAOS version test.

    First checked in by xmei@jlab.org on Mar/26/2024.
*/

#include <gtest/gtest.h>
#include <daos.h>

// Test whether DAOS version is 2.4.0.
// This ensures <daos.h> can be correctly compiled and linked to.
TEST(DaosTest, VersionAssertion) {
    EXPECT_EQ(DAOS_VERSION_MAJOR, 2);
    EXPECT_EQ(DAOS_VERSION_MINOR, 4);
    EXPECT_EQ(DAOS_VERSION_FIX, 0);
}
