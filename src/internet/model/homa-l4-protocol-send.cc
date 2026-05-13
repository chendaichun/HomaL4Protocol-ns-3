/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2020 Stanford University
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Serhat Arslan <sarslan@stanford.edu>
 */

#include <algorithm>
#include <numeric>
#include <sstream>

#include "ns3/log.h"
#include "ns3/assert.h"
#include "ns3/packet.h"
#include "ns3/node.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/object-vector.h"
#include "ns3/uinteger.h"

#include "ns3/point-to-point-net-device.h"
#include "ns3/ppp-header.h"
#include "ns3/ipv4-route.h"
#include "ipv4-end-point-demux.h"
#include "ipv4-end-point.h"
#include "ipv4-l3-protocol.h"
#include "homa-l4-protocol.h"
#include "homa-socket-factory.h"
#include "homa-socket.h"


namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("HomaL4ProtocolSend");

void
// 应用层发送入口：构造 OutboundMsg 并交给发送调度器分配 txMsgId 与后续发送时机。
HomaL4Protocol::Send (Ptr<Packet> message, 
                     Ipv4Address saddr, Ipv4Address daddr, 
                     uint16_t sport, uint16_t dport)
{
  NS_LOG_FUNCTION (this << message << saddr << daddr << sport << dport);
    
  Send(message, saddr, daddr, sport, dport, 0);
}
    
void
// 应用层发送入口：构造 OutboundMsg 并交给发送调度器分配 txMsgId 与后续发送时机。
HomaL4Protocol::Send (Ptr<Packet> message, 
                     Ipv4Address saddr, Ipv4Address daddr, 
                     uint16_t sport, uint16_t dport, Ptr<Ipv4Route> route)
{
  NS_LOG_FUNCTION (this << message << saddr << daddr << sport << dport << route);
  
  Ptr<HomaOutboundMsg> outMsg = CreateObject<HomaOutboundMsg> (message, saddr, daddr, 
                                                               sport, dport, this);
  // Route is usually null and resolved by IPv4, but source-routed callers can fill it here.
  outMsg->SetRoute (route);
    
  int txMsgId = m_sendScheduler->ScheduleNewMsg(outMsg);
    
  if (txMsgId >= 0)
    m_msgBeginTrace(message, saddr, daddr, sport, dport, txMsgId);
}

/******************************************************************************/

// 注册并返回 HomaOutboundMsg 的 TypeId。
TypeId HomaOutboundMsg::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaOutboundMsg")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}

/*
 * This method creates a new outbound message with the given information.
 */
// 构造发送侧消息状态：分片、初始化可发送窗口、以及 SIRD 首包授权等待策略。
HomaOutboundMsg::HomaOutboundMsg (Ptr<Packet> message, 
                                  Ipv4Address saddr, Ipv4Address daddr, 
                                  uint16_t sport, uint16_t dport, 
                                  Ptr<HomaL4Protocol> homa)
    : m_route(0),
      m_prio(0),
      m_prioSetByReceiver(false),
  m_waitForFirstGrant(false),
  m_initialCreditRequestSent(false),
      m_isExpired(false)
{
  NS_LOG_FUNCTION (this);
      
  m_saddr = saddr;
  m_daddr = daddr;
  m_sport = sport;
  m_dport = dport;
  m_homa = homa;
    
  m_msgSizeBytes = message->GetSize ();
  // The remaining undelivered message size equals to the total message size in the beginning
  m_remainingBytes = m_msgSizeBytes;
    
  HomaHeader homah;
  Ipv4Header ipv4h;
  m_maxPayloadSize = m_homa->GetMtu () - homah.GetSerializedSize () - ipv4h.GetSerializedSize ();
    
  // Packetize the message into MTU sized packets and store the corresponding state
  uint32_t unpacketizedBytes = m_msgSizeBytes;
  uint16_t numPkts = 0;
  uint32_t nextPktSize;
  Ptr<Packet> nextPkt;
  while (unpacketizedBytes > 0)
  {
    nextPktSize = std::min(unpacketizedBytes, m_maxPayloadSize);

    if (m_homa->UsesOptimizedMemory ())
    {
      m_pktSizes.push_back(nextPktSize);
    }
    else
    {
      nextPkt = message->CreateFragment (m_msgSizeBytes - unpacketizedBytes, nextPktSize);
      m_packets.push_back(nextPkt);
    }

    m_pktTxQ.push(numPkts);

    unpacketizedBytes -= nextPktSize;
    numPkts++;
  } 
  NS_ASSERT(numPkts == m_msgSizeBytes / m_maxPayloadSize + (m_msgSizeBytes % m_maxPayloadSize != 0));
  
  if (numPkts == 0)
  {
    m_maxGrantedIdx = 0;
  }
  else if (m_homa->IsSirdEnabled ())
  {
    uint16_t unschThresholdPkts = std::max<uint16_t> (1, m_homa->GetSirdUnschThresholdPkts ());
    if (numPkts > unschThresholdPkts)
    {
      // For long messages, require explicit GRANT before the first data packet.
      m_maxGrantedIdx = 0;
      m_waitForFirstGrant = true;
    }
    else
    {
      m_maxGrantedIdx = std::min((uint16_t)(m_homa->GetBdp () - 1), (uint16_t)(numPkts - 1));
    }
  }
  else
  {
    m_maxGrantedIdx = std::min((uint16_t)(m_homa->GetBdp () -1), numPkts);
  }
          
  // Sender-side retransmission/garbage-collection is intentionally disabled here.
  // The original code scheduled an outbound timeout, but current Homa logic relies on
  // receiver-driven control and explicit ACK cleanup instead.
  // m_rtxEvent = Simulator::Schedule (m_homa->GetOutboundRtxTimeout (), 
  //                                   &HomaOutboundMsg::ExpireRtxTimeout, 
  //                                   this, m_maxGrantedIdx);
}

// 析构函数：当前无额外清理逻辑，保留日志便于调试生命周期。
HomaOutboundMsg::~HomaOutboundMsg ()
{
  NS_LOG_FUNCTION_NOARGS ();
}

// 记录消息可选路由信息，供发送下发时透传给 IPv4。
void HomaOutboundMsg::SetRoute(Ptr<Ipv4Route> route)
{
  m_route = route;
}
    
// 返回消息携带的显式路由；大多数情况下为空。
Ptr<Ipv4Route> HomaOutboundMsg::GetRoute ()
{
  return m_route;
}
    
// 返回尚未被 ACK 确认的消息字节数。
uint32_t HomaOutboundMsg::GetRemainingBytes()
{
  return m_remainingBytes;
}

// 返回原始消息大小（单位：byte）。
uint32_t HomaOutboundMsg::GetMsgSizeBytes()
{
  return m_msgSizeBytes;
}
// 返回分片后的总 packet 数。
uint16_t HomaOutboundMsg::GetMsgSizePkts()
{
  return m_msgSizeBytes / m_maxPayloadSize + (m_msgSizeBytes % m_maxPayloadSize != 0);
}
    
// 返回消息源地址。
Ipv4Address HomaOutboundMsg::GetSrcAddress ()
{
  return m_saddr;
}
    
// 返回消息目的地址。
Ipv4Address HomaOutboundMsg::GetDstAddress ()
{
  return m_daddr;
}

// 返回消息源端口。
uint16_t HomaOutboundMsg::GetSrcPort ()
{
  return m_sport;
}
    
// 返回消息目的端口。
uint16_t HomaOutboundMsg::GetDstPort ()
{
  return m_dport;
}
    
// 返回发送端目前见到的最大 grant offset。
uint16_t HomaOutboundMsg::GetMaxGrantedIdx ()
{
  return m_maxGrantedIdx;
}
    
// 返回该消息是否已经被标记为过期，需要由调度器回收。
bool HomaOutboundMsg::IsExpired ()
{
  return m_isExpired;
}
    
// 计算给指定 packet offset 使用的网络优先级。
//
// 在接收端尚未显式指定优先级前，这里使用一个本地启发式规则区分短消息和长消息。
uint8_t HomaOutboundMsg::GetPrio (uint16_t pktOffset)
{
  if (!m_prioSetByReceiver)
  {
//     if (this->GetMsgSizePkts () < m_homa->GetBdp ())
    if (this->GetMsgSizePkts () < 13) // Based on heuristics
        return 0;
    else
      return m_homa->GetNumUnschedPrioBands () - 1;
    // TODO: Determine priority of unscheduled packet (index = pktOffset)
    //       according to the distribution of message sizes.
  }
  return m_prio;
}
    
// 返回最近一次记录的发送侧重传事件。
EventId HomaOutboundMsg::GetRtxEvent ()
{
  return m_rtxEvent;
}
    
// 从发送队列挑选当前可发分片：需同时满足未发送且不超过授权上界。
bool HomaOutboundMsg::TryGetNextPktOffset (uint16_t &pktOffset)
{
  NS_LOG_FUNCTION (this);

  if (m_waitForFirstGrant)
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this
                 << ") is waiting for first explicit GRANT before data transmission.");
    return false;
  }
    
  if (!m_pktTxQ.empty ())
  {
    uint16_t nextPktOffset = m_pktTxQ.top();
    
    if (nextPktOffset <= m_maxGrantedIdx && nextPktOffset < this->GetMsgSizePkts ())
    {
      if (m_homa->IsSirdEnabled () &&
          !m_sirdCreditAvailableTimes.empty () &&
          m_sirdCreditAvailableTimes.front () > Simulator::Now ())
      {
        NS_LOG_LOGIC("HomaOutboundMsg (" << this
                     << ") has scheduled credit, but its sender launch delay has not expired.");
        return false;
      }
      // The head of m_pktTxQ is the smallest unsent packet offset. Requiring
      // `nextPktOffset <= m_maxGrantedIdx` ensures that the sender never launches
      // DATA beyond the receiver-visible grant window.
      NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                   << ") can send packet " << nextPktOffset << " next.");
      pktOffset = nextPktOffset;
      return true;
    }
  }
  NS_LOG_LOGIC("HomaOutboundMsg (" << this 
               << ") doesn't have any packet to send!");
  return false;
}

// 判断长消息是否需要先发零负载 DATA 作为首轮信用请求。
bool HomaOutboundMsg::NeedsInitialCreditRequest (void) const
{
  return m_waitForFirstGrant && !m_initialCreditRequestSent;
}

// 标记首轮信用请求已发送，避免重复请求。
void HomaOutboundMsg::MarkInitialCreditRequestSent (void)
{
  m_initialCreditRequestSent = true;
}

// 构造零负载 DATA 分组，请求接收端显式发放第一笔授权。
Ptr<Packet> HomaOutboundMsg::GenerateInitialCreditRequest (uint16_t txMsgId)
{
  HomaHeader homaHeader;
  homaHeader.SetDstPort (m_dport);
  homaHeader.SetSrcPort (m_sport);
  homaHeader.SetTxMsgId (txMsgId);
  homaHeader.SetFlags (HomaHeader::Flags_t::DATA);
  homaHeader.SetMsgSize (m_msgSizeBytes);
  homaHeader.SetPktOffset (0);
  homaHeader.SetGrantOffset (0);
  homaHeader.SetPayloadSize (0);
  homaHeader.SetFeedbackFlags (HomaHeader::FeedbackFlags_t::FEEDBACK_NONE);
  homaHeader.SetPrio (0);

  Ptr<Packet> p = Create<Packet> ();
  SocketIpTosTag ipTosTag;
  // Keep priority in DSCP bits, ECN in low bits.
  ipTosTag.SetTos (static_cast<uint8_t> ((0u << 2) | static_cast<uint8_t> (Ipv4Header::ECN_ECT0)));
  p->ReplacePacketTag (ipTosTag);
  p->AddHeader (homaHeader);
  return p;
}
    
// 弹出并返回指定偏移的数据分片，同时清理重复排队项避免冗余发送。
Ptr<Packet> HomaOutboundMsg::RemoveNextPktFromTxQ (uint16_t pktOffset)
{
  NS_LOG_FUNCTION (this << pktOffset);
    
  NS_ASSERT_MSG(!m_pktTxQ.empty (), 
                "HomaOutboundMsg can't send a pkt if its TX queue is empty!");
  NS_ASSERT_MSG(m_pktTxQ.top() == pktOffset,
                "HomaOutboundMsg can only send the packet at the head of TX queue!");
  
  /*
   * In case a pktOffset was added multiple times to the tx queue,
   * we remove all of them at once to prevent redundant transmissions.
   */
  while(m_pktTxQ.top() == pktOffset && !m_pktTxQ.empty ())
  {
    m_pktTxQ.pop();
  }
   
  if (m_homa->UsesOptimizedMemory ())
    return Create<Packet> (m_pktSizes[pktOffset]);
  else
    return m_packets[pktOffset]->Copy();
}

uint16_t
HomaOutboundMsg::GetAccumulatedCreditPkts (void) const
{
  if (m_waitForFirstGrant || m_pktTxQ.empty ())
    {
      return 0;
    }

  uint16_t nextPktOffset = m_pktTxQ.top ();
  uint16_t msgSizePkts = m_msgSizeBytes / m_maxPayloadSize + (m_msgSizeBytes % m_maxPayloadSize != 0);
  if (msgSizePkts == 0)
    {
      return 0;
    }

  uint16_t highestGranted = std::min<uint16_t> (m_maxGrantedIdx, static_cast<uint16_t> (msgSizePkts - 1));
  if (nextPktOffset > highestGranted)
    {
      return 0;
    }
  return highestGranted - nextPktOffset + 1;
}

uint16_t
HomaOutboundMsg::GetAvailableSirdCreditPkts (void) const
{
  uint16_t availablePkts = 0;
  Time now = Simulator::Now ();
  for (const auto& availableAt : m_sirdCreditAvailableTimes)
    {
      if (availableAt <= now)
        {
          ++availablePkts;
        }
    }
  return availablePkts;
}

void
HomaOutboundMsg::AddSirdCreditAvailability (uint16_t creditPkts)
{
  if (creditPkts == 0 || !m_homa->IsSirdEnabled ())
    {
      return;
    }

  Time availableAt = Simulator::Now () + m_homa->GetSirdSenderCreditLaunchDelay ();
  for (uint16_t i = 0; i < creditPkts; ++i)
    {
      m_sirdCreditAvailableTimes.push_back (availableAt);
    }
}

void
HomaOutboundMsg::ConsumeSirdCredit (void)
{
  if (!m_homa->IsSirdEnabled () || m_sirdCreditAvailableTimes.empty ())
    {
      return;
    }

  NS_ASSERT_MSG (m_sirdCreditAvailableTimes.front () <= Simulator::Now (),
                 "Trying to consume scheduled credit before sender launch delay expired.");
  m_sirdCreditAvailableTimes.pop_front ();
}

bool
HomaOutboundMsg::GetNextSirdCreditDelay (Time &delay) const
{
  if (!m_homa->IsSirdEnabled () ||
      m_waitForFirstGrant ||
      m_pktTxQ.empty () ||
      m_sirdCreditAvailableTimes.empty ())
    {
      return false;
    }

  uint16_t nextPktOffset = m_pktTxQ.top ();
  uint16_t msgSizePkts = m_msgSizeBytes / m_maxPayloadSize + (m_msgSizeBytes % m_maxPayloadSize != 0);
  if (nextPktOffset > m_maxGrantedIdx || nextPktOffset >= msgSizePkts)
    {
      return false;
    }

  Time now = Simulator::Now ();
  if (m_sirdCreditAvailableTimes.front () <= now)
    {
      return false;
    }

  delay = m_sirdCreditAvailableTimes.front () - now;
  return true;
}
    
/*
 * This method updates the state for the corresponding outbound message
 * upon receival of a Grant or RESEND. The state is updated only if the  
 * granted packet index is larger than the highest grant index received 
 * so far. This allows reordered Grants to be ignored when more recent 
 * ones are received.
 */
// 处理接收端返回的 GRANT/RESEND 授权信息。
//
// 这里做三件关键事情：
// 1. 推进 m_maxGrantedIdx，扩大发送端可见的授权窗口；
// 2. 记录接收端指定的优先级；
// 3. 把新增授权转化为 sender-side available credit，供后续 DATA 真正起飞。
void HomaOutboundMsg::HandleGrantOffset (HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << homaHeader);
    
  uint16_t grantOffset = homaHeader.GetGrantOffset();
  NS_ASSERT_MSG(grantOffset < this->GetMsgSizePkts (), 
                "HomaOutboundMsg shouldn't be granted after it is already fully granted!");
  
  bool wasWaitingForFirstGrant = m_waitForFirstGrant;
  uint16_t oldMaxGrantedIdx = m_maxGrantedIdx;
  bool firstGrant = wasWaitingForFirstGrant && grantOffset >= m_maxGrantedIdx;
  if (m_waitForFirstGrant && grantOffset >= m_maxGrantedIdx)
  {
    m_waitForFirstGrant = false;
  }

  if (grantOffset > m_maxGrantedIdx)
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                 << ") is increasing the Grant index to "
                 << grantOffset << ".");
      
    m_maxGrantedIdx = grantOffset;
      
    uint8_t prio = homaHeader.GetPrio();
    NS_LOG_LOGIC("HomaOutboundMsg (" << this << ") is setting priority to "
                 << (uint16_t) prio << ".");
    m_prio = prio;
    m_prioSetByReceiver = true;
      
    /*
     * Homa 并不会为每个 DATA 显式回 ACK。这里的 remainingBytes 不是“真实已送达”，
     * 而是基于 receiver 始终维持约 1 BDP in-flight 的协议假设做出来的发送侧估计。
     */
    m_remainingBytes = m_msgSizeBytes - (m_maxGrantedIdx+1 - m_homa->GetBdp ()) * m_maxPayloadSize;
    uint16_t newCreditPkts = wasWaitingForFirstGrant ?
                             static_cast<uint16_t> (grantOffset + 1) :
                             static_cast<uint16_t> (grantOffset - oldMaxGrantedIdx);
    AddSirdCreditAvailability (newCreditPkts);
    m_homa->TraceSirdSenderCreditState (m_saddr,
                                        m_daddr,
                                        homaHeader.GetTxMsgId (),
                                        this->GetAccumulatedCreditPkts (),
                                        1);
  }
  else if (firstGrant)
  {
    uint8_t prio = homaHeader.GetPrio();
    NS_LOG_LOGIC("HomaOutboundMsg (" << this << ") is setting priority to "
                 << (uint16_t) prio << " for the first Grant.");
    m_prio = prio;
    m_prioSetByReceiver = true;
    AddSirdCreditAvailability (static_cast<uint16_t> (grantOffset + 1));
    m_homa->TraceSirdSenderCreditState (m_saddr,
                                        m_daddr,
                                        homaHeader.GetTxMsgId (),
                                        this->GetAccumulatedCreditPkts (),
                                        1);
  }
  else
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                 << ") has received an out-of-order Grant. State is not updated!");
  }
}
    
// 处理 RESEND 请求：将缺失分片重新入队，并在必要时提升授权上界。
void HomaOutboundMsg::HandleResend (HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << homaHeader);
    
  NS_ASSERT(homaHeader.GetFlags() & HomaHeader::Flags_t::RESEND);
  
  m_pktTxQ.push(homaHeader.GetPktOffset ());
  m_homa->TraceSirdSenderCreditState (m_saddr,
                                      m_daddr,
                                      homaHeader.GetTxMsgId (),
                                      this->GetAccumulatedCreditPkts (),
                                      2);

  uint16_t grantOffset = homaHeader.GetGrantOffset();
  NS_ASSERT_MSG(grantOffset < this->GetMsgSizePkts (), 
                "HomaOutboundMsg shouldn't be granted after it is already fully granted!");
  
  if (grantOffset > m_maxGrantedIdx)
  {
    NS_LOG_LOGIC("HomaOutboundMsg (" << this 
                 << ") is increasing the Grant index to "
                 << grantOffset << ".");
      
    m_maxGrantedIdx = grantOffset;
      
    uint8_t prio = homaHeader.GetPrio();
    NS_LOG_LOGIC("HomaOutboundMsg (" << this << ") is setting priority to "
                 << (uint16_t) prio << ".");
    m_prio = prio;
    m_prioSetByReceiver = true;
  }
}
    
// 处理 ACK：校验完整确认号后将消息剩余字节置零。
void HomaOutboundMsg::HandleAck (HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << homaHeader);
    
  NS_ASSERT(homaHeader.GetFlags() & HomaHeader::Flags_t::ACK);
    
  NS_ASSERT(homaHeader.GetPktOffset () == this->GetMsgSizePkts ());
  m_remainingBytes = 0;
}
    
// 构造 BUSY 控制包，告知对端当前发送端暂不可立即服务该消息。
Ptr<Packet> HomaOutboundMsg::GenerateBusy (uint16_t targetTxMsgId)
{
  NS_LOG_FUNCTION (this << targetTxMsgId);
    
  uint16_t pktOffset;
  if (!m_pktTxQ.empty ())
    pktOffset = m_pktTxQ.top();
  else
    pktOffset = this->GetMsgSizePkts ();
  
  HomaHeader homaHeader;
  homaHeader.SetSrcPort (m_sport); 
  homaHeader.SetDstPort (m_dport);
  homaHeader.SetTxMsgId (targetTxMsgId);
  homaHeader.SetMsgSize (m_msgSizeBytes);
  // BUSY 只是在反向控制路径上传递“我现在优先服务另一条消息”的信号，
  // 这些字段主要用于对端调试和状态对齐，不会驱动新的数据发送语义。
  homaHeader.SetPktOffset (pktOffset);
  homaHeader.SetGrantOffset (m_maxGrantedIdx);
  homaHeader.SetPrio (m_prio);
  homaHeader.SetPayloadSize (0);
  homaHeader.SetFlags (HomaHeader::Flags_t::BUSY);
    
  Ptr<Packet> busyPacket = Create<Packet> ();
  busyPacket->AddHeader (homaHeader);
    
  SocketIpTosTag ipTosTag;
  ipTosTag.SetTos (0); // Busy packets have the highest priority
  // This packet may already have a SocketIpTosTag (see HomaSocket)
  busyPacket->ReplacePacketTag (ipTosTag);
    
  return busyPacket;
}
    
// 发送侧超时处理：若长时间无授权进展则将消息标记为过期。
void HomaOutboundMsg::ExpireRtxTimeout(uint16_t lastRtxGrntIdx)
{
  NS_LOG_FUNCTION(this << lastRtxGrntIdx);
    
  if (m_remainingBytes == 0) // Fully delivered (ACK received)
  {
    return;
  }
  
  if (lastRtxGrntIdx < m_maxGrantedIdx)
  {
    m_rtxEvent = Simulator::Schedule (m_homa->GetOutboundRtxTimeout (), 
                                      &HomaOutboundMsg::ExpireRtxTimeout, 
                                      this, m_maxGrantedIdx);
  }
  else
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () << 
                " HomaOutboundMsg (" << this << ") has timed-out.");
    m_isExpired = true;
  }
}
    
/******************************************************************************/
    
const uint16_t HomaSendScheduler::MAX_N_MSG = 2560;

// 注册并返回 HomaSendScheduler 的 TypeId。
TypeId HomaSendScheduler::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaSendScheduler")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}
    
// 构造发送调度器并初始化可分配 txMsgId 自由列表。
HomaSendScheduler::HomaSendScheduler (Ptr<HomaL4Protocol> homaL4Protocol)
{
  NS_LOG_FUNCTION (this);
      
  m_homa = homaL4Protocol;
  
  // Initially, all the txMsgId values between 0 and MAX_N_MSG are listed as free
  m_txMsgIdFreeList.resize(MAX_N_MSG);
  std::iota(m_txMsgIdFreeList.begin(), m_txMsgIdFreeList.end(), 0);
}

// 析构时检查是否仍有未完成消息，便于发现发送流程异常。
HomaSendScheduler::~HomaSendScheduler ()
{
  NS_LOG_FUNCTION_NOARGS ();
    
  int numIncmpltMsg = m_outboundMsgs.size();
  if (numIncmpltMsg > 0)
  {
    NS_LOG_ERROR("ERROR: HomaSendScheduler (" << this <<
                 ") couldn't completely deliver " << 
                 numIncmpltMsg << " outbound messages!");
  }
}

/*
 * This method is called upon receiving a new message from the application layer.
 * It inserts the message into the list of pending outbound messages and updates
 * the scheduler's state accordingly.
 */
// 接收新消息并分配 txMsgId；若发送器空闲则安排首次发送事件。
int HomaSendScheduler::ScheduleNewMsg (Ptr<HomaOutboundMsg> outMsg)
{
  NS_LOG_FUNCTION (this << outMsg);
    
  uint16_t txMsgId;
  if (m_txMsgIdFreeList.size() > 0)
  {
    // Assign a unique txMsgId which will persist while the message lasts
    txMsgId = m_txMsgIdFreeList.front ();
    NS_LOG_LOGIC("HomaSendScheduler allocating txMsgId: " << txMsgId);
    m_txMsgIdFreeList.pop_front ();
      
    m_outboundMsgs[txMsgId] = outMsg;
      
    /* 
     * HomaSendScheduler can send a packet if it hasn't done so
     * recently. Otwerwise a txEvent should already been scheduled
     * which iterates over m_outboundMsgs to decide which packet 
     * to send next. 
     */
    if(m_txEvent.IsExpired()) 
      m_txEvent = Simulator::Schedule (m_homa->GetTxQueueDrainDelay (), 
                                       &HomaSendScheduler::TxDataPacket, this);
  }
  else
  {
    NS_LOG_ERROR(Simulator::Now ().GetNanoSeconds () << 
                 " Error: HomaSendScheduler ("<< this << 
                 ") could not allocate a new txMsgId for message (" << 
                 outMsg << ")" );
    return -1;
  }
  return (int)txMsgId;
}
   
/*
 * This method determines the txMsgId of the highest priority 
 * message that is ready to send some packets into the network.
 * See the nested if statements for the algorithm to choose 
 * highest priority outbound message.
 */
// 在所有活跃消息中选择下一条最应优先发送的消息，并清理已过期状态。
bool HomaSendScheduler::SelectNextSendableMsgId (uint16_t &txMsgId)
{
  NS_LOG_FUNCTION (this);
  
  Ptr<HomaOutboundMsg> currentMsg;
  Ptr<HomaOutboundMsg> candidateMsg;
  std::list<uint16_t> expiredMsgIds;
  uint32_t minRemainingBytes = std::numeric_limits<uint32_t>::max();
  uint16_t pktOffset;
  bool msgSelected = false;
  /*
   * Iterate over all pending outbound messages and select the one 
   * that has the smallest remainingBytes, granted but not transmitted 
   * packets, and a receiver that is not listed as busy.
   */
  for (auto& it: m_outboundMsgs) 
  {
    currentMsg = it.second;
      
    if (!currentMsg->IsExpired ())
    { 
      uint32_t curRemainingBytes = currentMsg->GetRemainingBytes();
      // Accept current msg if the remainig size is smaller than minRemainingBytes
      if (curRemainingBytes < minRemainingBytes)
      {
        // Accept current msg if it has a sendable data packet or needs a
        // zero-payload initial credit request for long SIRD messages.
        if (currentMsg->NeedsInitialCreditRequest () ||
            currentMsg->TryGetNextPktOffset(pktOffset))
        {
          candidateMsg = currentMsg;
          txMsgId = it.first;
          minRemainingBytes = curRemainingBytes;
          msgSelected = true;
        }
      }
    }
    else // Expired messages should be removed from the state
    {
      expiredMsgIds.push_back(it.first); 
    }
  }
    
  /*
   * We only record the txMsgId of the expired msgs above and only remove them
   * after the for loop above finishes because the ClearStateForMsg function
   * also iterates over m_outboundMsgs and erases an element which may cause the
   * iterator to skip elements in the for loop above.
   */
  for (const auto& expiredTxMsgId : expiredMsgIds)
  {
    this->ClearStateForMsg (expiredTxMsgId);
  }
    
  return msgSelected;
}
    
// 选择下一条消息并直接构造下一待发分组（可能是信用请求或真实 DATA）。
bool HomaSendScheduler::GetNextPacket (uint16_t &txMsgId, Ptr<Packet> &p)
{
  NS_LOG_FUNCTION (this);

  if (!this->SelectNextSendableMsgId (txMsgId))
  {
    return false;
  }

  uint16_t pktOffset;
  Ptr<HomaOutboundMsg> candidateMsg = m_outboundMsgs[txMsgId];

  if (candidateMsg->NeedsInitialCreditRequest ())
  {
    p = candidateMsg->GenerateInitialCreditRequest (txMsgId);
    candidateMsg->MarkInitialCreditRequestSent ();
    return true;
  }
    
  uint32_t accumulatedCreditPkts = this->GetAccumulatedCreditPkts ();
  if (candidateMsg->TryGetNextPktOffset(pktOffset))
  {
    p = candidateMsg->RemoveNextPktFromTxQ(pktOffset);
    if (m_homa->IsSirdEnabled ())
    {
      // CSN reflects sender-wide credit buildup, not just this single message.
      // Capture the aggregate backlog before we consume one message-local credit.
      candidateMsg->ConsumeSirdCredit ();
      m_homa->TraceSirdSenderCreditState (candidateMsg->GetSrcAddress (),
                                          candidateMsg->GetDstAddress (),
                                          txMsgId,
                                          candidateMsg->GetAccumulatedCreditPkts (),
                                          3);
    }
      
    HomaHeader homaHeader;
    homaHeader.SetDstPort (candidateMsg->GetDstPort ());
    homaHeader.SetSrcPort (candidateMsg->GetSrcPort ());
    homaHeader.SetTxMsgId (txMsgId);
    homaHeader.SetFlags (HomaHeader::Flags_t::DATA); 
    homaHeader.SetMsgSize (candidateMsg->GetMsgSizeBytes ());
    homaHeader.SetPktOffset (pktOffset);
    homaHeader.SetGrantOffset (candidateMsg->GetMaxGrantedIdx ()); // For monitoring purposes
    homaHeader.SetPayloadSize (p->GetSize ());
    if (m_homa->IsSirdEnabled () && accumulatedCreditPkts > m_homa->GetSirdSenderCsnThreshold ())
    {
      homaHeader.SetFeedbackFlags (HomaHeader::FeedbackFlags_t::FEEDBACK_CSN);
    }
    else
    {
      homaHeader.SetFeedbackFlags (HomaHeader::FeedbackFlags_t::FEEDBACK_NONE);
    }
    
    // NOTE: Use the following SocketIpTosTag append strategy when 
    //       sending packets out. This allows us to set the priority
    //       of the packets correctly for the PfifoHomaQueueDisc way of 
    //       priority queueing in the network.
    SocketIpTosTag ipTosTag;
    uint8_t dataPrio = candidateMsg->GetPrio (pktOffset);
    uint8_t ecn = m_homa->IsSirdEnabled () ? static_cast<uint8_t> (Ipv4Header::ECN_ECT0)
                         : static_cast<uint8_t> (Ipv4Header::ECN_NotECT);
    ipTosTag.SetTos (static_cast<uint8_t> ((dataPrio << 2) | ecn));
    // This packet may already have a SocketIpTosTag (see HomaSocket)
    p->ReplacePacketTag (ipTosTag);
      
    /*
     * The priority of packets are actually carried on the packet tags as
     * shown above. The priority field on the homaHeader field is actually
     * used by control packets to signal the requested priority from receivers
     * to the senders, so that they can set their data packet priorities 
     * accordingly.
     *
     * Setting the priority field on a data packet is just for monitoring reasons.
     */
    homaHeader.SetPrio (candidateMsg->GetPrio (pktOffset));
      
    p->AddHeader (homaHeader);
    NS_LOG_DEBUG (Simulator::Now ().GetNanoSeconds () << 
                  " HomaL4Protocol sending: " << p->ToString ());
    
    return true;
  }
  else
  {
    return false;
  }
}

uint32_t
HomaSendScheduler::GetAccumulatedCreditPkts (void) const
{
  uint32_t accumulatedCreditPkts = 0;
  for (const auto& kv : m_outboundMsgs)
    {
      accumulatedCreditPkts += kv.second->GetAccumulatedCreditPkts ();
    }
  return accumulatedCreditPkts;
}
 
/*
 * This method is called either when a new packet to send is found after
 * an idle time period or when the serialization of the previous packets 
 * finish. This allows HomaSendScheduler to choose the most recent highest 
 * priority packet just before sending it.
 */
void
// 发送调度主循环：按链路空闲时刻触发，选包并下发后继续自调度。
HomaSendScheduler::TxDataPacket ()
{
  NS_LOG_FUNCTION (this);
    
  NS_ASSERT(m_txEvent.IsExpired());
    
  Time timeToDrainTxQ = m_homa->GetTxQueueDrainDelay();
  if (timeToDrainTxQ != Time(0))
  {
    m_txEvent = Simulator::Schedule (timeToDrainTxQ, 
                                     &HomaSendScheduler::TxDataPacket, this);
    return;
  }
    
  Ptr<Packet> p;
  uint16_t nextTxMsgID;
  if (this->GetNextPacket (nextTxMsgID, p))
  {   
    NS_LOG_LOGIC("HomaSendScheduler (" << this <<
                  ") will transmit a packet from msg " << nextTxMsgID);
    
    m_homa->SendDown(p, 
                     m_outboundMsgs[nextTxMsgID]->GetSrcAddress (), 
                     m_outboundMsgs[nextTxMsgID]->GetDstAddress (), 
                     m_outboundMsgs[nextTxMsgID]->GetRoute ());
    
    m_txEvent = Simulator::Schedule (m_homa->GetTxQueueDrainDelay(), 
                                     &HomaSendScheduler::TxDataPacket, this);
  }
  else
  {
    NS_LOG_LOGIC("HomaSendScheduler doesn't have any packet to send!");
    Time minCreditDelay;
    bool haveDelayedCredit = false;
    for (const auto& kv : m_outboundMsgs)
      {
        Time creditDelay;
        if (kv.second->GetNextSirdCreditDelay (creditDelay) &&
            (!haveDelayedCredit || creditDelay < minCreditDelay))
          {
            minCreditDelay = creditDelay;
            haveDelayedCredit = true;
          }
      }
    if (haveDelayedCredit)
      {
        m_txEvent = Simulator::Schedule (minCreditDelay,
                                         &HomaSendScheduler::TxDataPacket,
                                         this);
      }
  }
}
   
/*
 * This method is called when a control packet is received that interests
 * an outbound message.
 */
// 处理发送侧关注的控制包（GRANT/RESEND/ACK），并驱动状态机前进。
void HomaSendScheduler::HandleControlPacketForOutboundMsg(Ipv4Header const &ipv4Header,
                                                          HomaHeader const &homaHeader)
{
  NS_LOG_FUNCTION (this << ipv4Header << homaHeader);
    
  uint16_t targetTxMsgId = homaHeader.GetTxMsgId();
  if(m_outboundMsgs.find(targetTxMsgId) == m_outboundMsgs.end())
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                " HomaSendScheduler (" << this <<
                ") received a " << homaHeader.FlagsToString(homaHeader.GetFlags()) << 
                " packet for an unknown txMsgId (" << 
                targetTxMsgId << ").");
    return;
  }
    
  if (m_outboundMsgs[targetTxMsgId]->IsExpired ())
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                " HomaSendScheduler (" << this <<
                ") received a " << homaHeader.FlagsToString(homaHeader.GetFlags()) << 
                " packet for an expired txMsgId (" << 
                targetTxMsgId << ").");
    this->ClearStateForMsg (targetTxMsgId);
    return;
  }
    
  Ptr<HomaOutboundMsg> targetMsg = m_outboundMsgs[targetTxMsgId];
  // Verify that the TxMsgId indeed matches the 4 tuple
  NS_ASSERT( (targetMsg->GetSrcAddress() == ipv4Header.GetDestination ()) &&
             (targetMsg->GetDstAddress() == ipv4Header.GetSource ()) && 
             (targetMsg->GetSrcPort() == homaHeader.GetDstPort ()) &&
             (targetMsg->GetDstPort() == homaHeader.GetSrcPort ()) );
  
  uint8_t ctrlFlag = homaHeader.GetFlags();
  if (ctrlFlag & HomaHeader::Flags_t::GRANT)
  {
    targetMsg->HandleGrantOffset (homaHeader);
  }
  else if (ctrlFlag & HomaHeader::Flags_t::RESEND)
  {
    targetMsg->HandleGrantOffset (homaHeader);
    targetMsg->HandleResend (homaHeader);
      
    uint16_t nextTxMsgID = targetTxMsgId;
    if (this->SelectNextSendableMsgId (nextTxMsgID) &&
        nextTxMsgID != targetTxMsgId) 
    {
      // Incoming packet doesn't belong to the highest priority outboung message.
      NS_LOG_LOGIC("HomaSendScheduler (" << this 
                   << ") needs to send a BUSY packet for " << targetTxMsgId);
      
      m_homa->SendDown(targetMsg->GenerateBusy (nextTxMsgID), 
                       targetMsg->GetSrcAddress (), 
                       targetMsg->GetDstAddress (), 
                       targetMsg->GetRoute ());
    }
  }
  else if (ctrlFlag & HomaHeader::Flags_t::ACK)
  {
    NS_LOG_LOGIC("The HomaOutboundMsg (" << targetMsg << ") is fully delivered!");
    
    targetMsg->HandleAck (homaHeader); // Asserts some sanity checks.
    this->ClearStateForMsg (targetTxMsgId);
  }
  else
  {
    NS_LOG_ERROR("ERROR: HomaSendScheduler (" << this 
                 << ") has received an unexpected control packet ("
                 << homaHeader.FlagsToString(ctrlFlag) << ")");
      
    return;
  }
    
  /* 
   * Since control packets may allow new packets to be sent, we should try 
   * to transmit those packets.
   */
  if(m_txEvent.IsExpired()) 
    m_txEvent = Simulator::Schedule (m_homa->GetTxQueueDrainDelay(), 
                                     &HomaSendScheduler::TxDataPacket, this);
}
    

} // namespace ns3
