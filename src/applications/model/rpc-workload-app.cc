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

#include "rpc-workload-app.h"

#include <algorithm>
#include <cmath>

#include "ns3/boolean.h"
#include "ns3/callback.h"
#include "ns3/double.h"
#include "ns3/homa-socket-factory.h"
#include "ns3/log.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"
#include "ns3/udp-socket-factory.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("RpcWorkloadApp");

NS_OBJECT_ENSURE_REGISTERED (RpcWorkloadApp);

TypeId
RpcWorkloadApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::RpcWorkloadApp")
    .SetParent<Application> ()
    .SetGroupName ("Applications")
    .AddAttribute ("Protocol",
                   "The type of protocol to use. This should be a subclass of ns3::SocketFactory",
                   TypeIdValue (HomaSocketFactory::GetTypeId ()),
                   MakeTypeIdAccessor (&RpcWorkloadApp::m_tid),
                   MakeTypeIdChecker ())
    .AddAttribute ("MaxMsg",
                   "The total number of requests to send. The value zero means that there is no limit.",
                   UintegerValue (0),
                   MakeUintegerAccessor (&RpcWorkloadApp::m_maxMsgs),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("PayloadSize",
                   "MTU for the network interface excluding the header sizes",
                   UintegerValue (1400),
                   MakeUintegerAccessor (&RpcWorkloadApp::m_maxPayloadSize),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("ResponseBytes",
                   "Response size in bytes for each RPC reply.",
                   UintegerValue (20),
                   MakeUintegerAccessor (&RpcWorkloadApp::m_responseBytes),
                   MakeUintegerChecker<uint32_t> ());
  return tid;
}

RpcWorkloadApp::RpcWorkloadApp (Ipv4Address localIp, uint16_t serverPort, uint16_t clientPort)
  : m_serverSocket (0),
    m_clientSocket (0),
    m_localIp (localIp),
    m_serverPort (serverPort),
    m_clientPort (clientPort),
    m_interMsgTime (0),
    m_msgSizePkts (0),
    m_remoteHost (0),
    m_maxPayloadSize (0),
    m_totMsgCnt (0),
    m_maxMsgs (0),
    m_responseBytes (20)
{
  NS_LOG_FUNCTION (this << localIp << serverPort << clientPort);
}

RpcWorkloadApp::~RpcWorkloadApp ()
{
  NS_LOG_FUNCTION (this);
}

void
RpcWorkloadApp::Install (Ptr<Node> node, const std::vector<Ipv4Address>& remoteHosts)
{
  NS_LOG_FUNCTION (this << node);

  node->AddApplication (this);

  m_serverSocket = Socket::CreateSocket (node, m_tid);
  m_serverSocket->Bind (InetSocketAddress (m_localIp, m_serverPort));
  m_serverSocket->SetRecvCallback (MakeCallback (&RpcWorkloadApp::ReceiveRequest, this));

  m_clientSocket = Socket::CreateSocket (node, m_tid);
  m_clientSocket->Bind (InetSocketAddress (m_localIp, m_clientPort));
  m_clientSocket->SetRecvCallback (MakeCallback (&RpcWorkloadApp::ReceiveResponse, this));

  m_remoteHosts.clear ();
  for (Ipv4Address remoteHost : remoteHosts)
    {
      if (remoteHost != m_localIp)
        {
          m_remoteHosts.push_back (remoteHost);
        }
    }

  m_remoteHost = CreateObject<UniformRandomVariable> ();
  m_remoteHost->SetAttribute ("Min", DoubleValue (0));
  m_remoteHost->SetAttribute ("Max", DoubleValue (m_remoteHosts.size ()));
}

void
RpcWorkloadApp::SetRequestWorkload (double load,
                                    const std::map<double, int>& msgSizeCdf,
                                    double avgMsgSizePkts)
{
  NS_LOG_FUNCTION (this << load << avgMsgSizePkts);

  load = std::max (0.0, std::min (load, 1.0));

  Ptr<NetDevice> netDevice = GetNode ()->GetDevice (0);
  uint32_t mtu = netDevice->GetMtu ();

  PointToPointNetDevice* p2pNetDevice = dynamic_cast<PointToPointNetDevice*> (&(*netDevice));
  uint64_t txRate = p2pNetDevice->GetDataRate ().GetBitRate ();

  double avgPktLoadBytes = static_cast<double> (mtu + 64);
  double avgInterMsgTime = (avgMsgSizePkts * avgPktLoadBytes * 8.0) / (static_cast<double> (txRate) * load);

  m_interMsgTime = CreateObject<ExponentialRandomVariable> ();
  m_interMsgTime->SetAttribute ("Mean", DoubleValue (avgInterMsgTime));

  m_msgSizeCdf = msgSizeCdf;

  m_msgSizePkts = CreateObject<UniformRandomVariable> ();
  m_msgSizePkts->SetAttribute ("Min", DoubleValue (0));
  m_msgSizePkts->SetAttribute ("Max", DoubleValue (1));
}

void
RpcWorkloadApp::SetResponseBytes (uint32_t responseBytes)
{
  m_responseBytes = responseBytes;
}

void
RpcWorkloadApp::Start (Time start)
{
  SetStartTime (start);
}

void
RpcWorkloadApp::Stop (Time stop)
{
  SetStopTime (stop);
}

void
RpcWorkloadApp::DoDispose (void)
{
  NS_LOG_FUNCTION (this);

  CancelNextEvent ();
  Application::DoDispose ();
}

void
RpcWorkloadApp::StartApplication ()
{
  NS_LOG_FUNCTION (Simulator::Now ().GetNanoSeconds () << this);

  NS_ASSERT_MSG (m_serverSocket && m_clientSocket && m_remoteHost && m_interMsgTime && m_msgSizePkts,
                 "RpcWorkloadApp must be installed and configured before starting.");
  ScheduleNextRequest ();
}

void
RpcWorkloadApp::StopApplication ()
{
  NS_LOG_FUNCTION (Simulator::Now ().GetNanoSeconds () << this);
  CancelNextEvent ();
}

void
RpcWorkloadApp::CancelNextEvent ()
{
  if (!Simulator::IsExpired (m_nextSendEvent))
    {
      Simulator::Cancel (m_nextSendEvent);
    }
}

void
RpcWorkloadApp::ScheduleNextRequest ()
{
  if (Simulator::IsExpired (m_nextSendEvent))
    {
      m_nextSendEvent = Simulator::Schedule (Seconds (m_interMsgTime->GetValue ()),
                                             &RpcWorkloadApp::SendRequest,
                                             this);
    }
  else
    {
      NS_LOG_WARN ("RpcWorkloadApp tries to schedule the next request before the previous event fired.");
    }
}

uint32_t
RpcWorkloadApp::GetNextRequestSizeBytes ()
{
  int msgSizePkts = -1;
  double rndValue = m_msgSizePkts->GetValue ();
  for (const auto& entry : m_msgSizeCdf)
    {
      if (rndValue <= entry.first)
        {
          msgSizePkts = entry.second;
          break;
        }
    }

  NS_ASSERT (msgSizePkts >= 0);
  msgSizePkts = std::min (0xffff, msgSizePkts);

  if (m_maxPayloadSize > 0)
    {
      return m_maxPayloadSize * static_cast<uint32_t> (msgSizePkts);
    }
  return GetNode ()->GetDevice (0)->GetMtu () * static_cast<uint32_t> (msgSizePkts);
}

void
RpcWorkloadApp::SendRequest ()
{
  int remoteHostIdx = static_cast<int> (std::floor (m_remoteHost->GetValue ()));
  InetSocketAddress receiverAddr (m_remoteHosts.at (remoteHostIdx), m_serverPort);

  uint32_t msgSizeBytes = GetNextRequestSizeBytes ();
  Ptr<Packet> request = Create<Packet> (msgSizeBytes);
  int sentBytes = m_clientSocket->SendTo (request, 0, receiverAddr);
  if (sentBytes > 0)
    {
      ++m_totMsgCnt;
    }

  if (m_maxMsgs == 0 || m_totMsgCnt < m_maxMsgs)
    {
      ScheduleNextRequest ();
    }
}

void
RpcWorkloadApp::ReceiveRequest (Ptr<Socket> socket)
{
  Ptr<Packet> request;
  Address from;
  while ((request = socket->RecvFrom (from)))
    {
      (void) request;
      Ptr<Packet> response = Create<Packet> (m_responseBytes);
      socket->SendTo (response, 0, InetSocketAddress::ConvertFrom (from));
    }
}

void
RpcWorkloadApp::ReceiveResponse (Ptr<Socket> socket)
{
  Ptr<Packet> response;
  Address from;
  while ((response = socket->RecvFrom (from)))
    {
      (void) response;
      (void) from;
    }
}

} // namespace ns3
