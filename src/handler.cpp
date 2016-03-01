// Copyright (c) 2016 Cory Fields
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "handler.h"

#include "libbtcnet/handler.h"
#include "libbtcnet/connection.h"

#include "threads.h"
#include "eventtypes.h"
#include "logger.h"
#include "listener.h"
#include "incomingconn.h"
#include "directconn.h"
#include "dnsconn.h"
#include "resolveonly.h"
#include "proxyconn.h"

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/dns.h>
#include <event2/util.h>

#include <assert.h>
#include <string.h>
#include <limits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <ws2tcpip.h>
#else
#include <netinet/tcp.h>
#endif

static constexpr int g_max_simultaneous_connecting = 8;

CConnectionHandlerInt::CConnectionHandlerInt(CConnectionHandler& handler, bool enable_threading)
    : m_interface(handler), m_connection_index(0), m_bytes_read(0), m_bytes_written(0), m_outgoing_conn_count(0), m_incoming_conn_count(0), m_outgoing_conn_limit(0), m_enable_threading(enable_threading), m_shutdown(false)
{
    if (m_enable_threading)
        setup_threads();
}

CConnectionHandlerInt::~CConnectionHandlerInt()
{
}

void CConnectionHandlerInt::Start(int outgoing_limit)
{
    int result = 0;
    m_shutdown = false;

#ifndef NO_THREADS
    if (m_enable_threading)
        m_main_thread = std::this_thread::get_id();
#endif

    assert(m_outgoing_conn_count == 0);
    assert(m_incoming_conn_count == 0);
    assert(!m_event_base);
    assert(!m_dns_base);
    assert(!m_request_event);
    assert(!m_shutdown_event);

    m_outgoing_conn_limit = outgoing_limit;

    event_type<event_config> cfg(event_config_new());
    result = event_config_set_flag(cfg, EVENT_BASE_FLAG_EPOLL_USE_CHANGELIST);
    assert(result == 0);

    m_event_base = event_base_new_with_config(cfg);

    if (m_enable_threading) {
        result = enable_threads_for_handler(m_event_base);
        assert(result == 0);
    }
    cfg.free();

    m_dns_base = evdns_base_new(m_event_base, 1);
    result = evdns_base_set_option(m_dns_base, "randomize-case", "0");
    assert(result == 0);

    m_request_event.reset(m_event_base, EV_PERSIST, std::bind(&CConnectionHandlerInt::RequestOutgoingInt, this));
    m_shutdown_event.reset(m_event_base, 0, std::bind(&CConnectionHandlerInt::ShutdownInt, this));

    m_outgoing_rate_cfg = ev_token_bucket_cfg_new(EV_RATE_LIMIT_MAX, EV_RATE_LIMIT_MAX, EV_RATE_LIMIT_MAX, EV_RATE_LIMIT_MAX, nullptr);
    m_incoming_rate_cfg = ev_token_bucket_cfg_new(EV_RATE_LIMIT_MAX, EV_RATE_LIMIT_MAX, EV_RATE_LIMIT_MAX, EV_RATE_LIMIT_MAX, nullptr);

    m_outgoing_rate_limit = bufferevent_rate_limit_group_new(m_event_base, m_outgoing_rate_cfg);
    m_incoming_rate_limit = bufferevent_rate_limit_group_new(m_event_base, m_incoming_rate_cfg);

    m_request_event.add({0, 500000});
    m_interface.OnStartup();

    m_request_event.active();
}

void CConnectionHandlerInt::ShutdownInt()
{
    assert(IsEventThread());
    if (m_shutdown)
        return;

    DEBUG_PRINT(LOGINFO, "shutdown started");

    std::map<ConnID, std::unique_ptr<ConnectionBase> > disconnecting;
    {
        optional_lock(m_conn_mutex, m_enable_threading);
        disconnecting.swap(m_connected);
    }
    std::map<ConnID, std::unique_ptr<CConnListener> > binds;
    {
        optional_lock(m_bind_mutex, m_enable_threading);
        binds.swap(m_binds);
    }

    m_shutdown = true;

    for (auto iter = disconnecting.begin(); iter != disconnecting.end(); disconnecting.erase(iter++)) {
        if (iter->second->IsOutgoing())
            m_outgoing_conn_count--;
        else
            m_incoming_conn_count--;
        m_interface.OnDisconnected(iter->first, false);
    }

    for (auto iter = m_connecting.begin(); iter != m_connecting.end(); m_connecting.erase(iter++)) {
        if (iter->second->IsOutgoing()) {
            const CConnection& conn = iter->second->GetBaseConnection();
            m_interface.OnConnectionFailure(conn, conn, false);
        }
    }

    m_dns_resolves.clear();
    binds.clear();

    m_dns_base.free();
    m_outgoing_rate_limit.free();
    m_incoming_rate_limit.free();
    m_incoming_rate_cfg.free();
    m_outgoing_rate_cfg.free();
    m_request_event.free();
    m_shutdown_event.free();

    assert(m_connecting.empty());
    assert(disconnecting.empty());
    assert(m_connected.empty());
    assert(binds.empty());
    assert(m_dns_resolves.empty());
    assert(!m_outgoing_conn_count);
    assert(!m_incoming_conn_count);

    DEBUG_PRINT(LOGINFO, "shutdown complete");
    event_base_loopbreak(m_event_base);
}

const event_type<event_base>& CConnectionHandlerInt::GetEventBase() const
{
    return m_event_base;
}

const event_type<evdns_base>& CConnectionHandlerInt::GetDNSBase() const
{
    return m_dns_base;
}

bool CConnectionHandlerInt::IsEventThread() const
{
#ifdef NO_THREADS
    return true;
#else
    if (!m_enable_threading)
        return true;
    return m_main_thread == std::this_thread::get_id();
#endif
}

void CConnectionHandlerInt::SetSocketOpts(sockaddr* addr, int, evutil_socket_t sock)
{
    evutil_make_socket_nonblocking(sock);
    int set = 1;
#ifdef _WIN32
    typedef char* sockoptptr;
#else
    typedef void* sockoptptr;
#endif

    if (addr->sa_family == AF_INET || addr->sa_family == AF_INET6)
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (sockoptptr)&set, sizeof(int));
}

void CConnectionHandlerInt::OnResolveComplete(ConnID id, const CConnection& conn, std::list<CConnection, std::allocator<CConnection> > resolved)
{
    assert(IsEventThread());
    m_interface.OnDnsResponse(conn, std::move(resolved));
    m_dns_resolves.erase(id);
}

void CConnectionHandlerInt::OnResolveFailure(ConnID id, const CConnection& conn, int error, bool retry)
{
    assert(IsEventThread());
    retry = retry && !m_shutdown;
    m_interface.OnDnsFailure(conn, retry);
    auto it = m_dns_resolves.find(id);
    assert(it != m_dns_resolves.end());
    if (retry)
        it->second->Retry();
    else
        m_dns_resolves.erase(it);
    if (m_request_event)
        m_request_event.active();
}

void CConnectionHandlerInt::OnWriteBufferReady(ConnID id, size_t bufsize)
{
    assert(IsEventThread());
    m_interface.OnWriteBufferReady(id, bufsize);
}

void CConnectionHandlerInt::OnDisconnected(ConnID id, bool reconnect)
{
    assert(IsEventThread());
    std::unique_ptr<ConnectionBase> moved;
    reconnect = reconnect && !m_shutdown;
    {
        optional_lock(m_conn_mutex, m_enable_threading);
        auto it = m_connected.find(id);
        assert(it != m_connected.end());
        moved.swap(it->second);
        m_connected.erase(it);
    }

    bool outgoing = moved->IsOutgoing();
    if (outgoing)
        m_outgoing_conn_count--;
    else
        m_incoming_conn_count--;

    m_interface.OnDisconnected(id, reconnect);
    if (reconnect) {
        ConnID newId = m_connection_index++;
        auto it = m_connecting.emplace_hint(m_connecting.end(), newId, std::move(moved));
        it->second->Retry(newId);
    } else if (m_request_event)
        m_request_event.active();
}

void CConnectionHandlerInt::OnIncomingConnection(const CConnection& bind, evutil_socket_t sock, sockaddr* address, int socklen)
{
    assert(IsEventThread());

    ConnID id = m_connection_index++;
    std::unique_ptr<ConnectionBase> ptr(new CIncomingConn(*this, bind, id, sock, address, socklen));
    {
        auto it = m_connecting.emplace_hint(m_connecting.end(), id, std::move(ptr));
        it->second->Connect();
    }
}

void CConnectionHandlerInt::OnListenFailure(ConnID id, const CConnection& bind)
{
    assert(IsEventThread());
    m_interface.OnBindFailure(bind);
    optional_lock(m_bind_mutex, m_enable_threading);
    m_binds.erase(id);
}

void CConnectionHandlerInt::OnConnectionFailure(ConnID id, ConnectionFailureType type, int error, CConnection failed, bool retry)
{
    assert(IsEventThread());
    auto it = m_connecting.find(id);
    assert(it != m_connecting.end());
    auto ptr = std::move(it->second);
    m_connecting.erase(it);
    retry = retry && !m_shutdown;

    if (type & ConnectionFailureType::PROXY)
        m_interface.OnProxyFailure(failed, retry);
    else if (type & ConnectionFailureType::RESOLVE)
        m_interface.OnDnsFailure(std::move(failed), retry);
    else
        m_interface.OnConnectionFailure(failed, failed, retry);
    if (retry && !m_shutdown) {
        ConnID newId = m_connection_index++;
        auto newit = m_connecting.emplace_hint(m_connecting.end(), newId, std::move(ptr));
        newit->second->Retry(newId);
    } else if (m_request_event)
        m_request_event.active();
}

void CConnectionHandlerInt::OnIncomingConnected(ConnID id, const CConnection& conn, const CConnection& resolved_conn)
{
    assert(IsEventThread());
    DEBUG_PRINT(LOGVERBOSE, "id:", id);
    auto it = m_connecting.find(id);
    assert(it != m_connecting.end());
    std::unique_ptr<ConnectionBase> moved(nullptr);
    it->second.swap(moved);
    m_connecting.erase(it);

    if (m_interface.OnIncomingConnection(id, conn, resolved_conn)) {
        moved->SetRateLimitGroup(m_incoming_rate_limit);
        {
            optional_lock(m_conn_mutex, m_enable_threading);
            m_connected.emplace_hint(m_connected.end(), id, std::move(moved));
        }
        m_incoming_conn_count++;
    }
}

void CConnectionHandlerInt::OnOutgoingConnected(ConnID id, const CConnection& conn, const CConnection& resolved_conn)
{
    assert(IsEventThread());
    DEBUG_PRINT(LOGVERBOSE, "id:", id);
    auto it = m_connecting.find(id);
    assert(it != m_connecting.end());
    std::unique_ptr<ConnectionBase> moved(nullptr);
    it->second.swap(moved);

    moved->SetRateLimitGroup(m_outgoing_rate_limit);
    m_connecting.erase(it);
    {
        optional_lock(m_conn_mutex, m_enable_threading);
        m_connected.emplace_hint(m_connected.end(), id, std::move(moved));
    }
    m_interface.OnOutgoingConnection(id, conn, resolved_conn);
    m_interface.OnReadyForFirstSend(id);
    m_outgoing_conn_count++;
}

bool CConnectionHandlerInt::OnReceiveMessages(ConnID id, std::list<std::vector<unsigned char> > msgs, size_t totalsize)
{
    assert(IsEventThread());
    return m_interface.OnReceiveMessages(id, std::move(msgs), totalsize);
}

void CConnectionHandlerInt::OnWriteBufferFull(ConnID id, size_t bufsize)
{
    assert(IsEventThread());
    m_interface.OnWriteBufferFull(id, bufsize);
}

bool CConnectionHandlerInt::Bind(CConnection conn)
{
    assert(IsEventThread());
    ConnID id = m_connection_index++;
    std::unique_ptr<CConnListener> listener(new CConnListener(*this, m_event_base, id, std::move(conn)));
    bool ret = listener->Bind();
    if (ret) {
        optional_lock(m_bind_mutex, m_enable_threading);
        auto it = m_binds.emplace_hint(m_binds.end(), id, std::move(listener));
        it->second->Enable();
    }
    return ret;
}

void CConnectionHandlerInt::StartConnection(CConnection&& conn)
{
    assert(IsEventThread());
    ConnID id = m_connection_index++;
    if (conn.IsDNS() && conn.GetOptions().doResolve == CConnectionOptions::RESOLVE_ONLY) {
        if (conn.GetProxy().IsSet()) {
            /* TODO */
        } else {
            std::unique_ptr<CResolveOnly> ptr(new CResolveOnly(*this, std::move(conn), id));
            auto it = m_dns_resolves.emplace_hint(m_dns_resolves.end(), id, std::move(ptr));
            it->second->Resolve();
        }
    } else {
        ConnectionBase* ptr;
        if (conn.GetProxy().IsSet())
            ptr = new CProxyConn(*this, std::move(conn), id);
        else if (conn.IsDNS())
            ptr = new CDNSConnection(*this, std::move(conn), id);
        else
            ptr = new CDirectConnection(*this, std::move(conn), id);
        auto it = m_connecting.emplace_hint(m_connecting.end(), id, std::unique_ptr<ConnectionBase>(ptr));
        it->second->Connect();
    }
}

bufferevent_options CConnectionHandlerInt::GetBevOpts() const
{
    assert(IsEventThread());
    return m_enable_threading == true ? bufferevent_options(BEV_OPT_THREADSAFE | BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS) : bufferevent_options(BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
}

void CConnectionHandlerInt::RequestOutgoingInt()
{
    assert(IsEventThread());
    size_t need = (size_t)std::min(g_max_simultaneous_connecting, m_outgoing_conn_limit - m_outgoing_conn_count - (int)m_connecting.size());
    if (need > 0) {
        std::list<CConnection> conns(m_interface.OnNeedOutgoingConnections(need));
        auto end = conns.begin();
        std::advance(end, std::min(conns.size(), static_cast<size_t>(need)));
        for (auto it = conns.begin(); it != end; ++it) {
            if (it->IsSet())
                StartConnection(std::move(*it));
        }
    }
}

bool CConnectionHandlerInt::PumpEvents(bool block)
{
    assert(IsEventThread());
    if (!m_event_base)
        return false;
    if (m_shutdown)
        return false;
    event_base_loop(m_event_base, block ? EVLOOP_ONCE : EVLOOP_NONBLOCK);
    if (m_shutdown) {
        m_event_base.free();
        m_interface.OnShutdown();
        return false;
    }
    return true;
}


void CConnectionHandlerInt::CloseConnection(ConnID id, bool immediately)
{
    optional_lock(m_conn_mutex, m_enable_threading);
    auto it = m_connected.find(id);
    if (it != m_connected.end()) {
        if (immediately)
            it->second->Disconnect();
        else
            it->second->DisconnectWhenFinished();
    }
}

void CConnectionHandlerInt::PauseRecv(ConnID id)
{
    optional_lock(m_conn_mutex, m_enable_threading);
    auto it = m_connected.find(id);
    if (it != m_connected.end())
        it->second->PauseRecv();
}

void CConnectionHandlerInt::UnpauseRecv(ConnID id)
{
    optional_lock(m_conn_mutex, m_enable_threading);
    auto it = m_connected.find(id);
    if (it != m_connected.end())
        it->second->UnpauseRecv();
}

bool CConnectionHandlerInt::Send(ConnID id, const unsigned char* data, size_t size)
{
    optional_lock(m_conn_mutex, m_enable_threading);
    auto it = m_connected.find(id);
    bool ret;
    if (it == m_connected.end())
        ret = false;
    else
        ret = it->second->Write(data, size);
    return ret;
}

void CConnectionHandlerInt::SetRateLimit(ConnID id, const CRateLimit& limit)
{
    optional_lock(m_conn_mutex, m_enable_threading);
    auto it = m_connected.find(id);
    if (it != m_connected.end())
        it->second->SetRateLimit(limit);
}

void CConnectionHandlerInt::SetIncomingRateLimit(const CRateLimit& limit)
{
    event_type<ev_token_bucket_cfg> temp_cfg(ev_token_bucket_cfg_new(limit.nMaxReadRate, limit.nMaxBurstRead, limit.nMaxWriteRate, limit.nMaxBurstWrite, nullptr));
    {
        optional_lock(m_group_rate_mutex, m_enable_threading);
        bufferevent_rate_limit_group_set_cfg(m_incoming_rate_limit, temp_cfg);
        m_incoming_rate_cfg.swap(temp_cfg);
    }
}

void CConnectionHandlerInt::SetOutgoingRateLimit(const CRateLimit& limit)
{
    event_type<ev_token_bucket_cfg> temp_cfg(ev_token_bucket_cfg_new(limit.nMaxReadRate, limit.nMaxBurstRead, limit.nMaxWriteRate, limit.nMaxBurstWrite, nullptr));
    {
        optional_lock(m_group_rate_mutex, m_enable_threading);
        bufferevent_rate_limit_group_set_cfg(m_outgoing_rate_limit, temp_cfg);
        m_outgoing_rate_cfg.swap(temp_cfg);
    }
}

void CConnectionHandlerInt::Shutdown()
{
    m_shutdown_event.active();
}
