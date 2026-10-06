// Tests the asynchronous linger (SRTO_LINGER with SRTO_SNDSYN=false):
// srt_close() returns immediately and the socket goes on with sending
// the data still in its sender buffer.

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "test_env.h"
#include "srt.h"
#include "netinet_any.h"

using namespace std;

class LingerAsync: public srt::Test
{
protected:
    SRTSOCKET m_listener = SRT_INVALID_SOCK;
    SRTSOCKET m_caller = SRT_INVALID_SOCK;
    SRTSOCKET m_accepted = SRT_INVALID_SOCK;

    void setup() override
    {
        m_listener = srt_create_socket();
        ASSERT_NE(m_listener, SRT_INVALID_SOCK);
        m_caller = srt_create_socket();
        ASSERT_NE(m_caller, SRT_INVALID_SOCK);
    }

    void teardown() override
    {
        srt_close(m_caller);
        srt_close(m_accepted);
        srt_close(m_listener);
    }

    void connectPair(int linger_s)
    {
        const int transtype = SRTT_FILE;
        ASSERT_NE(srt_setsockflag(m_listener, SRTO_TRANSTYPE, &transtype, sizeof transtype), SRT_ERROR);
        srt::sockaddr_any sa = srt::CreateAddr("127.0.0.1", 0, AF_INET);
        ASSERT_NE(srt_bind(m_listener, sa.get(), sa.size()), SRT_ERROR);
        ASSERT_NE(srt_getsockname(m_listener, (sa.get()), (&sa.len)), SRT_ERROR);
        ASSERT_NE(srt_listen(m_listener, 1), SRT_ERROR);

        ASSERT_NE(srt_setsockflag(m_caller, SRTO_TRANSTYPE, &transtype, sizeof transtype), SRT_ERROR);
        const linger lg = { 1, linger_s };
        ASSERT_NE(srt_setsockflag(m_caller, SRTO_LINGER, &lg, sizeof lg), SRT_ERROR);
        ASSERT_NE(srt_connect(m_caller, sa.get(), sa.size()), SRT_INVALID_SOCK);
        m_accepted = srt_accept(m_listener, NULL, NULL);
        ASSERT_NE(m_accepted, SRT_INVALID_SOCK);

        const bool no = false;
        ASSERT_NE(srt_setsockflag(m_caller, SRTO_SNDSYN, &no, sizeof no), SRT_ERROR);
    }

    // Fills the sender buffer as much as possible without blocking.
    size_t sendAll(size_t maxsize)
    {
        vector<char> buf(1456 * 50, 'x');
        size_t sent = 0;
        while (sent < maxsize)
        {
            const int n = srt_send(m_caller, buf.data(), int(buf.size()));
            if (n <= 0)
                break;
            sent += n;
        }
        return sent;
    }
};

// All data are delivered after srt_close(), then the peer gets the shutdown.
TEST_F(LingerAsync, DeliversAllData)
{
    connectPair(10);

    future<pair<size_t, int>> reader = async(launch::async, [&]() {
        vector<char> buf(65536);
        size_t total = 0;
        for (;;)
        {
            const int n = srt_recv(m_accepted, buf.data(), int(buf.size()));
            if (n <= 0)
                return make_pair(total, n);
            total += n;
        }
    });

    const size_t sent = sendAll(1456 * 1000);
    ASSERT_GT(sent, 0u);

    const auto start = chrono::steady_clock::now();
    EXPECT_EQ(srt_close(m_caller), SRT_STATUS_OK);
    EXPECT_LT(chrono::steady_clock::now() - start, chrono::milliseconds(500)) << "srt_close() should not linger";
    m_caller = SRT_INVALID_SOCK;

    ASSERT_EQ(reader.wait_for(chrono::seconds(4)), future_status::ready) << "the shutdown was not received";
    const pair<size_t, int> res = reader.get();
    EXPECT_EQ(res.first, sent);
    EXPECT_EQ(res.second, 0) << "the connection should end with the shutdown";
}

// When the peer doesn't read, the socket is closed when the linger expires,
// and the peer gets the shutdown.
TEST_F(LingerAsync, Expires)
{
    connectPair(1);

    // The peer doesn't read: the data stay in the sender buffer.
    const size_t sent = sendAll(1456 * 4000);
    ASSERT_GT(sent, 0u);

    EXPECT_EQ(srt_close(m_caller), SRT_STATUS_OK);
    m_caller = SRT_INVALID_SOCK;

    // Peer idle timeout is 5 s; the shutdown must come earlier.
    const auto deadline = chrono::steady_clock::now() + chrono::seconds(4);
    SRT_SOCKSTATUS st = srt_getsockstate(m_accepted);
    while (st == SRTS_CONNECTED && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(50));
        st = srt_getsockstate(m_accepted);
    }
    EXPECT_EQ(st, SRTS_BROKEN);
}
