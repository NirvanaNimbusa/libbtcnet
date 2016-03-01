// Copyright (c) 2016 Cory Fields
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BTCNET_CONNECTIONBASE_H
#define BTCNET_CONNECTIONBASE_H

#include "libbtcnet/connection.h"
#include "handler.h"
#include "event.h"
#include "eventtypes.h"

struct CConnFailure {
    int type;
    int error;
    const char* what();
};

struct bufferevent;
struct evbuffer_cb_info;
struct evbuffer;
struct ev_token_bucket_cfg;
struct event;

class ConnectionBase
{
public:
    virtual ~ConnectionBase();
    virtual void Connect() = 0;
    virtual void Cancel() = 0;
    virtual bool IsOutgoing() const = 0;
    void Disconnect();
    void DisconnectInt(int reason);
    void DisconnectWhenFinished();
    void DisconnectWhenFinishedInt();
    bool Write(const unsigned char* data, size_t size);
    void CheckWriteBufferInt();
    void SetRateLimit(const CRateLimit& limit);
    void SetRateLimitInt(const CRateLimit& limit);
    void PauseRecv();
    void PauseRecvInt();
    void UnpauseRecv();
    void UnpauseRecvInt();
    void WriteData();
    void Retry(ConnID newId);
    void SetRateLimitGroup(bufferevent_rate_limit_group* group);
    const CConnection& GetBaseConnection() const;

protected:
    ConnectionBase(CConnectionHandlerInt& handler, CConnection&& conn, ConnID id);
    void OnOutgoingConnected(event_type<bufferevent>&& bev, CConnection resolved);
    void OnIncomingConnected(event_type<bufferevent>&& bev, sockaddr* addr, int addrsize);
    void OnConnectionFailure(ConnectionFailureType type, int error, CConnection resolved, bool retry);
    void OnDisconnected();

private:
    void InitConnection();
    static void event_cb(bufferevent* bev, short events, void* ctx);
    static void read_cb(bufferevent* bev, void* ctx);
    static void write_cb(bufferevent* bev, void* ctx);
    static void close_on_finished_writecb(bufferevent* bev, void* ctx);
    static void first_read_cb(bufferevent* bev, void* ctx);
    static void first_write_cb(bufferevent* bev, void* ctx);

    static void read_data(struct evbuffer* buffer, const struct evbuffer_cb_info* info, void* ctx);
    static void wrote_data(struct evbuffer* buffer, const struct evbuffer_cb_info* info, void* ctx);

protected:
    CConnectionHandlerInt& m_handler;
    const event_type<event_base>& m_event_base;
    const CConnection m_connection;

private:
    ConnID m_id;
    unsigned long m_bytes_read;
    unsigned long m_bytes_written;

    event_type<bufferevent> m_bev;
    event_type<ev_token_bucket_cfg> m_rate_cfg;

    CEvent m_reconnect_func;
    CEvent m_disconnect_func;
    CEvent m_disconnect_wait_func;
    CEvent m_check_write_buffer_func;
};

#endif // BTCNET_CONNECTIONBASE_H