#include <stdio.h>
#include <stdlib.h>

#include "gtest/gtest.h"
#include "test_env.h"
#include "utilities.h"
#include "common.h"
#include "core.h"

using namespace srt;

void test_cipaddress_pton(const char* peer_ip, int family, const uint32_t (&ip)[4])
{
    const int port = 4200;

    // Peer
    sockaddr_storage ss;
    ss.ss_family = family;

    void* sin_addr = nullptr;
    if (family == AF_INET)
    {
        sockaddr_in* const sa = (sockaddr_in*)&ss;
        sa->sin_port          = htons(port);
        sin_addr              = &sa->sin_addr;
    }
    else // IPv6
    {
        sockaddr_in6* const sa = (sockaddr_in6*)&ss;
        sa->sin6_port          = htons(port);
        sin_addr               = &sa->sin6_addr;
    }

    ASSERT_EQ(inet_pton(family, peer_ip, sin_addr), 1);
    const sockaddr_any peer(ss);

    // HOST
    sockaddr_any host(family);
    host.hport(port);

    srt::CIPAddress::pton(host, ip, peer);
    EXPECT_EQ(peer, host) << "Peer " << peer.str() << " host " << host.str();
}

// Example IPv4 address: 192.168.0.1
TEST(CIPAddress, IPv4_pton)
{
    srt::TestInit srtinit;
    const char*    peer_ip = "192.168.0.1";
    const uint32_t ip[4]   = {htobe32(0xC0A80001), 0, 0, 0};
    test_cipaddress_pton(peer_ip, AF_INET, ip);
}

// Example IPv6 address: 2001:db8:85a3:8d3:1319:8a2e:370:7348
TEST(CIPAddress, IPv6_pton)
{
    srt::TestInit srtinit;
    const char*    peer_ip = "2001:db8:85a3:8d3:1319:8a2e:370:7348";
    const uint32_t ip[4]   = {htobe32(0x20010db8), htobe32(0x85a308d3), htobe32(0x13198a2e), htobe32(0x03707348)};

    test_cipaddress_pton(peer_ip, AF_INET6, ip);
}

// Example IPv4 address: 192.168.0.1
// Maps to IPv6 address: 0:0:0:0:0:FFFF:192.168.0.1
// Simplified:                   ::FFFF:192.168.0.1
TEST(CIPAddress, IPv4_in_IPv6_pton)
{
    srt::TestInit srtinit;
    const char*    peer_ip = "::ffff:192.168.0.1";
    const uint32_t ip[4]   = {0, 0, htobe32(0x0000FFFF), htobe32(0xC0A80001)};

    test_cipaddress_pton(peer_ip, AF_INET6, ip);
}


TEST(Common, CookieContest)
{
    srt::TestInit srtinit;
    using namespace std;

    srt_setloglevel(LOG_NOTICE);

    cout << "TEST 1: two easy comparable values\n";
    EXPECT_EQ(CUDT::computeCookieContest(100, 50), HSD_INITIATOR);
    EXPECT_EQ(CUDT::computeCookieContest(50, 100), HSD_RESPONDER);

    EXPECT_EQ(CUDT::computeCookieContest(-1, -1000), HSD_INITIATOR);
    EXPECT_EQ(CUDT::computeCookieContest(-1000, -1), HSD_RESPONDER);

    EXPECT_EQ(CUDT::computeCookieContest(10055, -10000), HSD_INITIATOR);
    EXPECT_EQ(CUDT::computeCookieContest(-10000, 10055), HSD_RESPONDER);
    // Values from PR 1517
    cout << "TEST 2: Values from PR 1517\n";
    EXPECT_EQ(CUDT::computeCookieContest(811599203, -1480577720), HSD_INITIATOR);
    EXPECT_EQ(CUDT::computeCookieContest(-1480577720, 811599203), HSD_RESPONDER);

    EXPECT_EQ(CUDT::computeCookieContest(2147483647, -2147483648), HSD_INITIATOR);
    EXPECT_EQ(CUDT::computeCookieContest(-2147483648, 2147483647), HSD_RESPONDER);

    cout << "TEST 3: wrong post-fix\n";
    // NOTE: 0x80000001 is a negative number in hex
    EXPECT_EQ(CUDT::computeCookieContest(0x00000001, 0x80000001), HSD_INITIATOR);
    EXPECT_EQ(CUDT::computeCookieContest(0x80000001, 0x00000001), HSD_RESPONDER);
}
