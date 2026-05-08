/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef RPC_WORKLOAD_APP_H
#define RPC_WORKLOAD_APP_H

#include <map>
#include <vector>

#include "ns3/address.h"
#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/inet-socket-address.h"
#include "ns3/ipv4-address.h"
#include "ns3/packet.h"
#include "ns3/random-variable-stream.h"
#include "ns3/socket.h"
#include "ns3/socket-factory.h"

namespace ns3 {

/**
 * \ingroup applications
 * \defgroup rpc-workload-app RpcWorkloadApp
 *
 * Background RPC workload generator used by paper-aligned sim1.
 *
 * Each host plays both roles:
 * - a client that issues workload-sized requests to peers;
 * - a server that immediately answers each request with a fixed-size response.
 *
 * The application uses two sockets so request and response traffic can be
 * distinguished by port numbers in transport traces.
 */
class RpcWorkloadApp : public Application
{
public:
  static TypeId GetTypeId (void);

  RpcWorkloadApp (Ipv4Address localIp, uint16_t serverPort, uint16_t clientPort);
  ~RpcWorkloadApp () override;

  void Install (Ptr<Node> node, const std::vector<Ipv4Address>& remoteHosts);
  void SetRequestWorkload (double load,
                           const std::map<double, int>& msgSizeCdf,
                           double avgMsgSizePkts);
  void SetResponseBytes (uint32_t responseBytes);
  void Start (Time start);
  void Stop (Time stop);

protected:
  void DoDispose (void) override;

private:
  void StartApplication (void) override;
  void StopApplication (void) override;

  void CancelNextEvent ();
  void ScheduleNextRequest ();
  uint32_t GetNextRequestSizeBytes ();
  void SendRequest ();
  void ReceiveRequest (Ptr<Socket> socket);
  void ReceiveResponse (Ptr<Socket> socket);

  Ptr<Socket> m_serverSocket;
  Ptr<Socket> m_clientSocket;
  TypeId m_tid;
  EventId m_nextSendEvent;

  Ipv4Address m_localIp;
  uint16_t m_serverPort;
  uint16_t m_clientPort;
  std::vector<Ipv4Address> m_remoteHosts;
  std::map<double, int> m_msgSizeCdf;

  Ptr<ExponentialRandomVariable> m_interMsgTime;
  Ptr<UniformRandomVariable> m_msgSizePkts;
  Ptr<UniformRandomVariable> m_remoteHost;

  uint32_t m_maxPayloadSize;
  uint16_t m_totMsgCnt;
  uint16_t m_maxMsgs;
  uint32_t m_responseBytes;
};

} // namespace ns3

#endif /* RPC_WORKLOAD_APP_H */
