// Tests that srt_close() unblocks the other threads blocked on the socket,
// and returns only once they have left the API call.

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

class CloseUnblock: public srt::Test
{
protected:
    SRTSOCKET m_listener = SRT_INVALID_SOCK;
    SRTSOCKET m_caller = SRT_INVALID_SOCK;
    SRTSOCKET m_accepted = SRT_INVALID_SOCK;
    int m_port = 0;

    void setup() override
    {
        m_listener = srt_create_socket();
        ASSERT_NE(m_listener, SRT_INVALID_SOCK);
    }

    void listen(int transtype)
    {
        ASSERT_NE(srt_setsockflag(m_listener, SRTO_TRANSTYPE, &transtype, sizeof transtype), SRT_ERROR);
        // Small receiver buffer, so that a non-reading peer quickly blocks the sender.
        const int fc = 32;
        const int rcvbuf = fc * 1500;
        ASSERT_NE(srt_setsockflag(m_listener, SRTO_FC, &fc, sizeof fc), SRT_ERROR);
        ASSERT_NE(srt_setsockflag(m_listener, SRTO_RCVBUF, &rcvbuf, sizeof rcvbuf), SRT_ERROR);
        srt::sockaddr_any sa = srt::CreateAddr("127.0.0.1", 0, AF_INET);
        ASSERT_NE(srt_bind(m_listener, sa.get(), sa.size()), SRT_ERROR);
        ASSERT_NE(srt_getsockname(m_listener, (sa.get()), (&sa.len)), SRT_ERROR);
        m_port = sa.hport();
        ASSERT_NE(srt_listen(m_listener, 1), SRT_ERROR);
    }

    void teardown() override
    {
        srt_close(m_caller);
        srt_close(m_accepted);
        srt_close(m_listener);
    }

    void connectPair(int transtype)
    {
        listen(transtype);
        m_caller = srt_create_socket();
        ASSERT_NE(m_caller, SRT_INVALID_SOCK);
        ASSERT_NE(srt_setsockflag(m_caller, SRTO_TRANSTYPE, &transtype, sizeof transtype), SRT_ERROR);

        srt::sockaddr_any sa = srt::CreateAddr("127.0.0.1", m_port, AF_INET);
        ASSERT_NE(srt_connect(m_caller, sa.get(), sa.size()), SRT_INVALID_SOCK);
        m_accepted = srt_accept(m_listener, NULL, NULL);
        ASSERT_NE(m_accepted, SRT_INVALID_SOCK);
    }

    // Runs `call` in a thread, closes `sock` once the thread is blocked,
    // and checks that the call has returned when srt_close() returns.
    template <class Fn>
    void closeWhileBlocked(SRTSOCKET sock, size_t nthreads, Fn call, vector<int>& w_results)
    {
        atomic<int> returned(0);
        vector<future<int>> calls;
        for (size_t i = 0; i < nthreads; ++i)
        {
            calls.push_back(async(launch::async, [&]() {
                int r = call();
                ++returned;
                return r;
            }));
        }

        this_thread::sleep_for(chrono::milliseconds(300));
        ASSERT_EQ(returned, 0) << "the calls should be blocked";

        EXPECT_EQ(srt_close(sock), SRT_STATUS_OK);

        for (auto& c: calls)
        {
            ASSERT_EQ(c.wait_for(chrono::seconds(2)), future_status::ready) << "a call stayed blocked";
            w_results.push_back(c.get());
        }
    }
};

TEST_F(CloseUnblock, RecvMessage)
{
    connectPair(SRTT_LIVE);

    vector<int> results;
    closeWhileBlocked(m_caller, 2, [&]() {
        char buf[1500];
        int r = srt_recvmsg(m_caller, buf, sizeof buf);
        return r == SRT_ERROR ? srt_getlasterror(NULL) : r;
    }, (results));

    for (int r: results)
        EXPECT_EQ(r, SRT_ECONNLOST);
}

TEST_F(CloseUnblock, RecvStream)
{
    connectPair(SRTT_FILE);

    // In stream mode, a local close is reported as EOF.
    vector<int> results;
    closeWhileBlocked(m_caller, 2, [&]() {
        char buf[1500];
        int r = srt_recv(m_caller, buf, sizeof buf);
        return r == SRT_ERROR ? srt_getlasterror(NULL) : r;
    }, (results));

    for (int r: results)
        EXPECT_EQ(r, 0);
}

TEST_F(CloseUnblock, Send)
{
    connectPair(SRTT_FILE);

    // The peer doesn't read, so the sender buffer gets full and sending blocks.
    // Fill it in non-blocking mode, then switch to blocking mode.
    linger lin = {0, 0};
    ASSERT_NE(srt_setsockflag(m_caller, SRTO_LINGER, &lin, sizeof lin), SRT_ERROR);
    bool sync = false;
    ASSERT_NE(srt_setsockflag(m_caller, SRTO_SNDSYN, &sync, sizeof sync), SRT_ERROR);

    vector<char> buf(1456 * 16);
    // Repeat until no space is freed anymore (the peer's buffer is full).
    for (int nsent = 1; nsent > 0; )
    {
        nsent = 0;
        while (srt_send(m_caller, buf.data(), int(buf.size())) != SRT_ERROR)
            ++nsent;
        ASSERT_EQ(srt_getlasterror(NULL), SRT_EASYNCSND);
        this_thread::sleep_for(chrono::milliseconds(200));
    }
    sync = true;
    ASSERT_NE(srt_setsockflag(m_caller, SRTO_SNDSYN, &sync, sizeof sync), SRT_ERROR);

    vector<int> results;
    closeWhileBlocked(m_caller, 3, [&]() {
        int r = srt_send(m_caller, buf.data(), int(buf.size()));
        return r == SRT_ERROR ? srt_getlasterror(NULL) : r;
    }, (results));

    for (int r: results)
        EXPECT_EQ(r, SRT_ECONNLOST);
}

TEST_F(CloseUnblock, Accept)
{
    listen(SRTT_LIVE);

    vector<int> results;
    closeWhileBlocked(m_listener, 2, [&]() {
        SRTSOCKET s = srt_accept(m_listener, NULL, NULL);
        return s == SRT_INVALID_SOCK ? srt_getlasterror(NULL) : int(s);
    }, (results));

    for (int r: results)
        EXPECT_NE(r, SRT_SUCCESS);
}

TEST_F(CloseUnblock, EpollWait)
{
    connectPair(SRTT_LIVE);

    const int eid = srt_epoll_create();
    ASSERT_GE(eid, 0);
    const int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    ASSERT_NE(srt_epoll_add_usock(eid, m_caller, &events), SRT_ERROR);

    // The closed socket is removed from the EID, which is then empty: the
    // waiting thread is woken up and gets the "empty EID" error.
    vector<int> results;
    closeWhileBlocked(m_caller, 1, [&]() {
        SRT_EPOLL_EVENT ev[2];
        int n = srt_epoll_uwait(eid, ev, 2, -1);
        return n == -1 ? srt_getlasterror(NULL) : n;
    }, (results));

    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0], int(SRT_EPOLLEMPTY));

    srt_epoll_release(eid);
}

// Closing a listener closes the connections that have not been accepted
// (the peer gets the shutdown) and releases the port.
TEST_F(CloseUnblock, ListenerClosesNonAccepted)
{
    listen(SRTT_LIVE);
    m_caller = srt_create_socket();
    ASSERT_NE(m_caller, SRT_INVALID_SOCK);
    srt::sockaddr_any sa = srt::CreateAddr("127.0.0.1", m_port, AF_INET);
    ASSERT_NE(srt_connect(m_caller, sa.get(), sa.size()), SRT_INVALID_SOCK);
    ASSERT_EQ(srt_getsockstate(m_caller), SRTS_CONNECTED);

    EXPECT_EQ(srt_close(m_listener), SRT_STATUS_OK);
    m_listener = SRT_INVALID_SOCK;

    // Peer idle timeout is 5 s; the shutdown must come much earlier.
    const auto deadline = chrono::steady_clock::now() + chrono::seconds(2);
    SRT_SOCKSTATUS st = srt_getsockstate(m_caller);
    while (st == SRTS_CONNECTED && chrono::steady_clock::now() < deadline)
    {
        this_thread::sleep_for(chrono::milliseconds(20));
        st = srt_getsockstate(m_caller);
    }
    EXPECT_EQ(st, SRTS_BROKEN);

    // The port is released.
    SRTSOCKET again = srt_create_socket();
    ASSERT_NE(again, SRT_INVALID_SOCK);
    EXPECT_NE(srt_bind(again, sa.get(), sa.size()), SRT_ERROR);
    srt_close(again);
}
