/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "rate-controlled-flow-app.h"

#include "ns3/data-rate.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/socket.h"
#include "ns3/socket-factory.h"
#include "ns3/string.h"
#include "ns3/switch-flow-id-tag.h"
#include "ns3/nstime.h"
#include "ns3/uinteger.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/simulator.h"

#include <algorithm>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("RateControlledFlowApp");

NS_OBJECT_ENSURE_REGISTERED (RateControlledFlowApp);

TypeId
RateControlledFlowApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::RateControlledFlowApp")
    .SetParent<Application> ()
    .SetGroupName ("Applications")
    .AddConstructor<RateControlledFlowApp> ()
    .AddAttribute ("Remote",
                   "The destination address.",
                   AddressValue (),
                   MakeAddressAccessor (&RateControlledFlowApp::m_peer),
                   MakeAddressChecker ())
    .AddAttribute ("Protocol",
                   "The type of protocol to use.",
                   TypeIdValue (UdpSocketFactory::GetTypeId ()),
                   MakeTypeIdAccessor (&RateControlledFlowApp::m_tid),
                   MakeTypeIdChecker ())
    .AddAttribute ("InitialRate",
                   "The initial sending rate.",
                   DataRateValue (DataRate ("1Gbps")),
                   MakeDataRateAccessor (&RateControlledFlowApp::m_initialRate),
                   MakeDataRateChecker ())
    .AddAttribute ("MinRate",
                   "Minimum sending rate after congestion feedback.",
                   DataRateValue (DataRate ("1Gbps")),
                   MakeDataRateAccessor (&RateControlledFlowApp::m_minRate),
                   MakeDataRateChecker ())
    .AddAttribute ("MaxRate",
                   "Maximum sending rate after additive recovery.",
                   DataRateValue (DataRate ("400Gbps")),
                   MakeDataRateAccessor (&RateControlledFlowApp::m_maxRate),
                   MakeDataRateChecker ())
    .AddAttribute ("AiStep",
                   "Additive rate recovery step.",
                   DataRateValue (DataRate ("1Gbps")),
                   MakeDataRateAccessor (&RateControlledFlowApp::m_aiStep),
                   MakeDataRateChecker ())
    .AddAttribute ("AiInterval",
                   "Additive recovery interval. Zero disables additive recovery.",
                   TimeValue (MicroSeconds (50)),
                   MakeTimeAccessor (&RateControlledFlowApp::m_aiInterval),
                   MakeTimeChecker ())
    .AddAttribute ("IpTos",
                   "IPv4 TOS byte used by generated packets; 0x02 sets ECN ECT(0).",
                   UintegerValue (0x02),
                   MakeUintegerAccessor (&RateControlledFlowApp::m_ipTos),
                   MakeUintegerChecker<uint8_t> ())
    .AddAttribute ("PacketSize",
                   "The size of generated packets.",
                   UintegerValue (4000),
                   MakeUintegerAccessor (&RateControlledFlowApp::m_packetSize),
                   MakeUintegerChecker<uint32_t> (1))
    .AddAttribute ("MaxBytes",
                   "The total number of bytes to send.",
                   UintegerValue (0),
                   MakeUintegerAccessor (&RateControlledFlowApp::m_maxBytes),
                   MakeUintegerChecker<uint64_t> ())
    .AddAttribute ("FlowId",
                   "Scenario-level flow identifier written into each packet.",
                   UintegerValue (0),
                   MakeUintegerAccessor (&RateControlledFlowApp::m_flowId),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("SourceId",
                   "Scenario-level source identifier written into each packet.",
                   UintegerValue (0),
                   MakeUintegerAccessor (&RateControlledFlowApp::m_sourceId),
                   MakeUintegerChecker<uint32_t> ())
    .AddTraceSource ("Tx",
                     "A new packet is created and is sent.",
                     MakeTraceSourceAccessor (&RateControlledFlowApp::m_txTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("RateChange",
                     "Application sending rate update: oldRate, newRate.",
                     MakeTraceSourceAccessor (&RateControlledFlowApp::m_rateTrace),
                     "ns3::TracedCallback::Uint64Uint64")
    .AddTraceSource ("PauseState",
                     "Application pause state: true on pause, false on resume.",
                     MakeTraceSourceAccessor (&RateControlledFlowApp::m_pauseTrace),
                     "ns3::TracedCallback::Bool")
  ;
  return tid;
}

RateControlledFlowApp::RateControlledFlowApp ()
  : m_socket (0),
    m_tid (UdpSocketFactory::GetTypeId ()),
    m_initialRate (DataRate ("1Gbps")),
    m_currentRate (DataRate ("1Gbps")),
    m_minRate (DataRate ("1Gbps")),
    m_maxRate (DataRate ("400Gbps")),
    m_aiStep (DataRate ("1Gbps")),
    m_aiInterval (MicroSeconds (50)),
    m_ipTos (0x02),
    m_packetSize (4000),
    m_maxBytes (0),
    m_totBytes (0),
    m_flowId (0),
    m_sourceId (0),
    m_paused (false)
{
  NS_LOG_FUNCTION (this);
}

RateControlledFlowApp::~RateControlledFlowApp ()
{
  NS_LOG_FUNCTION (this);
}

DataRate
RateControlledFlowApp::GetCurrentRate (void) const
{
  return m_currentRate;
}

uint64_t
RateControlledFlowApp::GetTotalBytes (void) const
{
  return m_totBytes;
}

void
RateControlledFlowApp::ApplyMultiplicativeDecrease (double factor)
{
  NS_LOG_FUNCTION (this << factor);
  if (factor <= 0.0)
    {
      return;
    }

  uint64_t current = m_currentRate.GetBitRate ();
  uint64_t next = static_cast<uint64_t> (current * factor);
  next = std::max<uint64_t> (next, m_minRate.GetBitRate ());
  uint64_t old = m_currentRate.GetBitRate ();
  m_currentRate = DataRate (next);
  if (m_currentRate.GetBitRate () != old)
    {
      m_rateTrace (old, m_currentRate.GetBitRate ());
      if (!m_paused && m_socket != 0)
        {
          CancelSendEvent ();
          ScheduleNextTx ();
        }
    }
}

void
RateControlledFlowApp::ApplyAdditiveIncrease (DataRate step)
{
  NS_LOG_FUNCTION (this << step);
  uint64_t old = m_currentRate.GetBitRate ();
  uint64_t next = std::min<uint64_t> (m_maxRate.GetBitRate (),
                                     old + step.GetBitRate ());
  m_currentRate = DataRate (next);
  if (next != old)
    {
      m_rateTrace (old, next);
    }
}

void
RateControlledFlowApp::PauseFlow (void)
{
  NS_LOG_FUNCTION (this);
  if (m_paused)
    {
      return;
    }
  m_paused = true;
  CancelSendEvent ();
  m_pauseTrace (true);
}

void
RateControlledFlowApp::ResumeFlow (void)
{
  NS_LOG_FUNCTION (this);
  if (!m_paused)
    {
      return;
    }

  m_paused = false;
  m_pauseTrace (false);
  if (m_socket != 0)
    {
      ScheduleNextTx ();
    }
}

bool
RateControlledFlowApp::IsPaused (void) const
{
  return m_paused;
}

void
RateControlledFlowApp::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  CancelSendEvent ();
  CancelAiEvent ();
  m_socket = 0;
  Application::DoDispose ();
}

void
RateControlledFlowApp::StartApplication (void)
{
  NS_LOG_FUNCTION (this);

  m_currentRate = m_initialRate;
  m_maxRate = DataRate (std::max<uint64_t> (m_maxRate.GetBitRate (),
                                           m_initialRate.GetBitRate ()));

  if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode (), m_tid);
      int ret = m_socket->Bind ();
      if (ret == -1)
        {
          NS_FATAL_ERROR ("Failed to bind socket");
        }

      m_socket->Connect (m_peer);
      m_socket->SetIpTos (m_ipTos);
      m_socket->SetAllowBroadcast (true);
      m_socket->ShutdownRecv ();
    }

  CancelSendEvent ();
  if (!m_paused)
    {
      ScheduleNextTx ();
    }
  ScheduleAdditiveIncrease ();
}

void
RateControlledFlowApp::StopApplication (void)
{
  NS_LOG_FUNCTION (this);
  CancelSendEvent ();
  CancelAiEvent ();
  if (m_socket != 0)
    {
      m_socket->Close ();
    }
}

void
RateControlledFlowApp::CancelSendEvent (void)
{
  Simulator::Cancel (m_sendEvent);
}

void
RateControlledFlowApp::CancelAiEvent (void)
{
  Simulator::Cancel (m_aiEvent);
}

void
RateControlledFlowApp::ScheduleNextTx (void)
{
  NS_LOG_FUNCTION (this);

  if (m_paused)
    {
      return;
    }

  if (m_maxBytes != 0 && m_totBytes >= m_maxBytes)
    {
      return;
    }

  double bitRate = static_cast<double> (m_currentRate.GetBitRate ());
  Time nextTime = Seconds ((m_packetSize * 8) / bitRate);
  m_sendEvent = Simulator::Schedule (nextTime, &RateControlledFlowApp::SendPacket, this);
}

void
RateControlledFlowApp::ScheduleAdditiveIncrease (void)
{
  if (m_aiInterval.IsZero () || m_aiStep.GetBitRate () == 0)
    {
      return;
    }
  m_aiEvent = Simulator::Schedule (m_aiInterval,
                                   &RateControlledFlowApp::AdditiveIncreaseTick,
                                   this);
}

void
RateControlledFlowApp::AdditiveIncreaseTick (void)
{
  if (m_socket != 0 && !m_paused)
    {
      ApplyAdditiveIncrease (m_aiStep);
    }
  ScheduleAdditiveIncrease ();
}

void
RateControlledFlowApp::SendPacket (void)
{
  NS_LOG_FUNCTION (this);

  if (m_paused || m_socket == 0)
    {
      return;
    }

  if (m_maxBytes != 0 && m_totBytes >= m_maxBytes)
    {
      return;
    }

  uint32_t remaining = m_packetSize;
  if (m_maxBytes != 0)
    {
      uint64_t left = m_maxBytes - m_totBytes;
      remaining = static_cast<uint32_t> (std::min<uint64_t> (remaining, left));
    }

  Ptr<Packet> packet = Create<Packet> (remaining);
  packet->AddPacketTag (SwitchFlowIdTag (m_flowId, m_sourceId));
  SocketIpTosTag ipTosTag;
  ipTosTag.SetTos (m_ipTos);
  packet->ReplacePacketTag (ipTosTag);
  int actual = m_socket->Send (packet);
  if (actual > 0)
    {
      m_totBytes += static_cast<uint32_t> (actual);
      m_txTrace (packet);
    }

  ScheduleNextTx ();
}

} // namespace ns3
