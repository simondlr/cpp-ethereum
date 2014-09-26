/*
 This file is part of cpp-ethereum.
 
 cpp-ethereum is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 cpp-ethereum is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
 */
/** @file NetService.h
 * @author Alex Leverington <nessence@gmail.com>
 * @date 2014
 */

#pragma once

#include "NetMsg.h"
#include "NetConnection.h"

namespace dev
{

class NetConnection;

/**
 * @brief NetService class by which connections can be registered in order to handle their messages; data messages received by the connection will be passed to a NetProtocol (T) instance, and by default, service messages will be passed to the service.
 *
 * NetService binds connections to NetProtocol objects which handle network messages and session state. NetService implementations may also be used for non-application specific messages such as broadcasting when a disconnect will occur or distributing notifications across multiple connections.
 *
 */
class NetServiceFace
{
	friend class NetEndpoint;
	friend class NetConnection;

public:
	static NetMsgServiceType serviceId() {};
	
	/// Called by endpoint before connection is started.
	virtual void registerConnection(std::weak_ptr<NetConnection> _conn) = 0;
	
protected:
	/// Called from connection when service message is received.
	virtual void serviceMessageReceived(NetMsg const& _msg, NetConnection* _conn) = 0;
};

template <class T> // protocol
class NetService: public NetServiceFace
{
	friend class NetEndpoint;
	friend class NetConnection;

public:
	static NetMsgServiceType serviceId() { return T::serviceId(); }
	
	void registerConnection(std::weak_ptr<NetConnection> _conn) {
		if (auto cp = _conn.lock())
		{
			NetConnection *c = cp.get();
			m_connState.insert(std::make_pair(_conn, std::unique_ptr<T>(new T(c,this))));
			
			// service messages to the service
			c->setServiceMessageHandler(T::serviceId(), [=](NetMsg const& _msg){
				serviceMessageReceived(_msg, c);
			});
			
			// data messages go to the protocol
			T* protocol = m_connState[_conn].get();
			c->setDataMessageHandler(T::serviceId(), [_conn, protocol](NetMsg const& _msg){
				protocol->receiveMessage(_msg);
			});
		}
	}
	
protected:
	void serviceMessageReceived(NetMsg const& _msg, NetConnection* _conn)
	{
		clog(RPCNote) << "[" << T::serviceId() << "] serviceMessageReceived";
	}
	
	std::map<std::weak_ptr<NetConnection>,std::unique_ptr<T>, std::owner_less<std::weak_ptr<NetConnection>>> m_connState;
};
}
