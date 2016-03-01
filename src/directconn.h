// Copyright (c) 2016 Cory Fields
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BTCNET_DIRECTCONN_H
#define BTCNET_DIRECTCONN_H

#include "connectionbase.h"
#include "bareconn.h"
#include "eventtypes.h"

class CConnection;
struct event_base;
struct bufferevent;

class CDirectConnection : public ConnectionBase, public CBareConnection
{
public:
    CDirectConnection(CConnectionHandlerInt& handler, CConnection&& conn, ConnID id);
    ~CDirectConnection() final;
    void Connect() final;
    void Cancel() final;
    bool IsOutgoing() const final;

protected:
    void OnConnectSuccess() final;
    void OnConnectFailure(short type) final;

private:
    int m_retries;
    event_type<bufferevent> m_bev;
};

#endif // BTCNET_DIRECTCONN_H