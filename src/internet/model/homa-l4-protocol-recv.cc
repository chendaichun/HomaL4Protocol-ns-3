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

NS_LOG_COMPONENT_DEFINE ("HomaL4Protocol");

/*
 * This method is called by the HomaRecvScheduler everytime a message is ready to
 * be forwarded up to the applications. 
 */
// 将完整重组后的消息按四元组查找端点并上交应用，同时触发消息完成 trace。
void HomaL4Protocol::ForwardUp (Ptr<Packet> completeMsg,
                                const Ipv4Header &header,
                                uint16_t sport, uint16_t dport, uint16_t txMsgId,
                                Ptr<Ipv4Interface> incomingInterface)
{
  NS_LOG_FUNCTION (this << completeMsg << header << sport << incomingInterface);
    
  NS_LOG_DEBUG ("Looking up dst " << header.GetDestination () << " port " << dport); 
  Ipv4EndPointDemux::EndPoints endPoints =
    m_endPoints->Lookup (header.GetDestination (), dport,
                         header.GetSource (), sport, incomingInterface);
    
  NS_ASSERT_MSG(!endPoints.empty (), 
                "HomaL4Protocol was able to find an endpoint when msg was received, but now it couldn't");
    
  for (Ipv4EndPointDemux::EndPointsI endPoint = endPoints.begin ();
         endPoint != endPoints.end (); endPoint++)
  {
    (*endPoint)->ForwardUp (completeMsg, header, sport, incomingInterface);
  }
    
  m_msgFinishTrace(completeMsg, header.GetSource(), header.GetDestination(), 
                   sport, dport, (int)txMsgId);
}

/******************************************************************************/

// 注册并返回 HomaInboundMsg 的 TypeId。
TypeId HomaInboundMsg::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaInboundMsg")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}

/*
 * This method creates a new inbound message with the given information.
 */
// 构造接收侧消息状态：初始化分片缓冲、授权窗口和重传相关计数。
HomaInboundMsg::HomaInboundMsg (Ptr<Packet> p,
                                Ipv4Header const &ipv4Header, HomaHeader const &homaHeader, 
                                Ptr<Ipv4Interface> iface, uint32_t mtuBytes, 
                                uint16_t rttPackets, bool memIsOptimized,
                                bool sirdEnabled, uint16_t sirdUnschThresholdPkts)
    : m_prio(0),
      m_hasGrantedData(false),
      m_creditDrivenGrantWindow(false),
      m_currentlyScheduled(false),
      m_numRtxWithoutProgress (0)
{
  NS_LOG_FUNCTION (this);

  m_ipv4Header = ipv4Header;
  m_iface = iface;
    
  m_sport = homaHeader.GetSrcPort ();
  m_dport = homaHeader.GetDstPort ();
  m_txMsgId = homaHeader.GetTxMsgId ();
   
  m_msgSizeBytes = homaHeader.GetMsgSize ();
  uint32_t maxPayloadSize = mtuBytes - homaHeader.GetSerializedSize () - ipv4Header.GetSerializedSize ();
  m_msgSizePkts = m_msgSizeBytes / maxPayloadSize + (m_msgSizeBytes % maxPayloadSize != 0);
          
  // The remaining undelivered message size equals to total minus actually received payload.
  m_remainingBytes = m_msgSizeBytes;
  
  // Fill in the packet buffer with place holder (empty) packets and set the received info as false
  for (uint16_t i = 0; i < m_msgSizePkts; i++)
  {
    if (memIsOptimized)
      m_pktSizes.push_back(0);
    else
      m_packets.push_back(Create<Packet> ());
      
    m_receivedPackets.push_back(false);
  } 
          
  uint16_t pktOffset = homaHeader.GetPktOffset ();
  bool hasPayload = p->GetSize () > 0;
  if (hasPayload)
  {
    if (memIsOptimized)
      m_pktSizes[pktOffset] = p->GetSize ();
    else
      m_packets[pktOffset] = p;
    m_receivedPackets[pktOffset] = true;
    m_remainingBytes -= p->GetSize ();
  }

  bool longSirdMsg = sirdEnabled && m_msgSizePkts > std::max<uint16_t> (1, sirdUnschThresholdPkts);
  if (longSirdMsg)
  {
    m_maxGrantedIdx = 0;
    m_maxGrantableIdx = 0;
    m_hasGrantedData = false;
    m_creditDrivenGrantWindow = true;
  }
  else
  {
    m_maxGrantedIdx = std::min<uint16_t> (rttPackets - 1, m_msgSizePkts - 1); // Unscheduled pkts are already granted
    m_maxGrantableIdx = m_maxGrantedIdx + 1; // 1 Data pkt is already received
    m_hasGrantedData = true;
  }
  m_lastRtxGrntIdx = m_maxGrantableIdx;
}

// 析构函数：当前无额外资源回收逻辑。
HomaInboundMsg::~HomaInboundMsg ()
{
  NS_LOG_FUNCTION_NOARGS ();
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint32_t HomaInboundMsg::GetRemainingBytes()
{
  return m_remainingBytes;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Address HomaInboundMsg::GetSrcAddress ()
{
  return m_ipv4Header.GetSource ();
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Address HomaInboundMsg::GetDstAddress ()
{
  return m_ipv4Header.GetDestination ();
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetSrcPort ()
{
  return m_sport;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetDstPort ()
{
  return m_dport;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetTxMsgId ()
{
  return m_txMsgId;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ipv4Header HomaInboundMsg::GetIpv4Header ()
{
  return m_ipv4Header;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
Ptr<Ipv4Interface> HomaInboundMsg::GetIpv4Interface ()
{
  return m_iface;
}

/*
 * Although the retransmission events are handled by the HomaRecvScheduler
 * the corresponding EventId of messages are kept within the messages 
 * themselves for the sake of being tidy.
 */
// 把重传定时器事件句柄挂在消息对象上，便于调度器统一管理。
void HomaInboundMsg::SetRtxEvent (EventId rtxEvent)
{
  m_rtxEvent = rtxEvent;
}
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
EventId HomaInboundMsg::GetRtxEvent ()
{
  return m_rtxEvent;
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetMaxGrantableIdx ()
{
  return m_maxGrantableIdx;
}

// 限制本轮最多可继续授权的窗口大小，用于精确控制 SIRD 授权额度。
void HomaInboundMsg::CapGrantableWindow (uint16_t grantWindowPkts)
{
  if (m_msgSizePkts == 0)
  {
    return;
  }

  uint16_t cappedWindow = std::max<uint16_t> (1, grantWindowPkts);
  uint32_t baseGrantable = m_hasGrantedData ? m_maxGrantedIdx + cappedWindow : cappedWindow - 1;
  uint32_t cappedMaxGrantable = std::min<uint32_t> (m_msgSizePkts - 1, baseGrantable);
  m_maxGrantableIdx = std::min<uint16_t> (m_maxGrantableIdx, static_cast<uint16_t> (cappedMaxGrantable));
}

bool HomaInboundMsg::AdvanceGrantableWindow (uint16_t grantPkts)
{
  if (m_msgSizePkts == 0 || this->IsFullyGranted ())
  {
    return false;
  }

  uint16_t creditPkts = std::max<uint16_t> (1, grantPkts);
  uint32_t baseGrantable = m_hasGrantedData ? m_maxGrantedIdx + creditPkts : creditPkts - 1;
  uint32_t advancedMaxGrantable = std::min<uint32_t> (m_msgSizePkts - 1, baseGrantable);
  if (advancedMaxGrantable <= m_maxGrantableIdx)
  {
    return false;
  }

  m_maxGrantableIdx = static_cast<uint16_t> (advancedMaxGrantable);
  return true;
}

// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetMaxGrantedIdx ()
{
  return m_maxGrantedIdx;
}
 
// 写入并更新当前对象的配置或运行时状态，为后续流程提供输入。
void HomaInboundMsg::SetLastRtxGrntIdx (uint16_t lastRtxGrntIdx)
{
  m_lastRtxGrntIdx = lastRtxGrntIdx;
}
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetLastRtxGrntIdx ()
{
  return m_lastRtxGrntIdx;
}
    
// 判断该消息是否已经被授权到最后一个分片。
bool HomaInboundMsg::IsFullyGranted ()
{
  if (!m_hasGrantedData)
  {
    return false;
  }
  return m_maxGrantedIdx >= m_msgSizePkts-1;
}

// 判断当前是否还能继续发送 GRANT（已授权上界尚未追上可授权上界）。
bool HomaInboundMsg::IsGrantable ()
{
  if (!m_hasGrantedData)
  {
    return m_msgSizePkts > 0 && m_maxGrantableIdx < m_msgSizePkts;
  }
  return m_maxGrantedIdx < m_maxGrantableIdx;
}
    
// 判断消息所有分片是否都已收到。
bool HomaInboundMsg::IsFullyReceived ()
{
  // 该函数实现当前类的一个核心步骤，结合调用链可理解其在状态机中的角色。
  return std::none_of(m_receivedPackets.begin(), 
                      m_receivedPackets.end(), 
                      std::logical_not<bool>());
}

// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
bool HomaInboundMsg::IsCurrentlyScheduled (void) 
{
  return m_currentlyScheduled;
}

// 写入并更新当前对象的配置或运行时状态，为后续流程提供输入。
void HomaInboundMsg::SetCurrentlyScheduled (bool currentlyScheduled) 
{
  m_currentlyScheduled = currentlyScheduled;
}
    
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
uint16_t HomaInboundMsg::GetNumRtxWithoutProgress ()
{
  return m_numRtxWithoutProgress;
}
// 该函数实现当前类的一个核心步骤，结合调用链可理解其在状态机中的角色。
void HomaInboundMsg::IncrNumRtxWithoutProgress ()
{
  m_numRtxWithoutProgress++;
}
// 该函数实现当前类的一个核心步骤，结合调用链可理解其在状态机中的角色。
void HomaInboundMsg::ResetNumRtxWithoutProgress ()
{
  m_numRtxWithoutProgress = 0;
}
    
/*
 * This method updates the state for an inbound message upon receival of a data packet.
 */
// 接收并登记一个数据分片，推进可授权窗口，并更新剩余字节。
void HomaInboundMsg::ReceiveDataPacket (Ptr<Packet> p, uint16_t pktOffset)
{
  NS_LOG_FUNCTION (this << p << pktOffset);

  if (p->GetSize () == 0)
  {
    // Zero-payload DATA is a credit request for long scheduled messages.
    return;
  }
    
  if (!m_receivedPackets[pktOffset])
  {
    if (m_pktSizes.size())
      m_pktSizes[pktOffset] = p->GetSize ();
    else
      m_packets[pktOffset] = p;
    m_receivedPackets[pktOffset] = true;
      
    m_remainingBytes -= p->GetSize ();
    /*
     * Since a packet has arrived, we can allow a new packet to be on flight
     * for this message, so that bytes in flight stay the same. However it 
     * is upto the HomaRecvScheduler to decide whether to send a Grant packet 
     * to the sender of this message or not.
     */
    if (m_creditDrivenGrantWindow || m_maxGrantableIdx >= m_msgSizePkts - 1)
    {
      return;
    }

    m_maxGrantableIdx++;
  }
  else
  {
    NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                " HomaInboundMsg (" << this << ") has received a packet for offset "
                << pktOffset << " which was already received.");
    // TODO: Insert a trace source to keep track of spurious retransmissions.
  }
}

// 在消息完整后执行重组，返回可直接上交应用层的完整 Packet。
Ptr<Packet> HomaInboundMsg::GetReassembledMsg ()
{
  NS_LOG_FUNCTION (this);
    
  if (m_pktSizes.size())
  {
    uint32_t msgSize = 0;
    for (std::size_t i = 0; i < m_msgSizePkts; i++)
    {
      NS_ASSERT_MSG(m_receivedPackets[i],
                    "ERROR: HomaRecvScheduler is trying to reassemble an incomplete msg!");
      msgSize += m_pktSizes[i];
    }
      
    return Create<Packet> (msgSize);
  }
  else
  {
    Ptr<Packet> completeMsg = Create<Packet> ();
    for (std::size_t i = 0; i < m_msgSizePkts; i++)
    {
      NS_ASSERT_MSG(m_receivedPackets[i],
                    "ERROR: HomaRecvScheduler is trying to reassemble an incomplete msg!");
      completeMsg->AddAtEnd (m_packets[i]);
    }
  
    return completeMsg;
  }
}
    
// 根据当前接收进度生成 GRANT 或 ACK 控制包，并同步授权边界。
Ptr<Packet> HomaInboundMsg::GenerateGrantOrAck(uint8_t grantedPrio,
                                               uint8_t pktTypeFlag)
{
  NS_LOG_FUNCTION (this << grantedPrio);
  NS_ASSERT_MSG((pktTypeFlag & HomaHeader::Flags_t::GRANT) || 
                (pktTypeFlag & HomaHeader::Flags_t::ACK),
                "GenerateGrantOrAck() can only be called to generate GRANT or ACK packets!");
  bool isAck = (pktTypeFlag & HomaHeader::Flags_t::ACK) != 0;
  NS_ASSERT_MSG(isAck || this->IsGrantable(),
                "GenerateGrantOrAck() can only generate GRANT for a grantable message!");
    
  m_prio = grantedPrio; // Updated with the most recent granted priority value
    
  uint16_t ackNo = m_msgSizePkts;
  for (std::size_t i = 0; i < m_msgSizePkts; i++)
  {
    if (!m_receivedPackets[i])
    {
      ackNo = i; // The earliest un-received packet
      break;
    }
  }
    
  HomaHeader homaHeader;
  // Note we swap the src and dst port numbers for reverse direction
  homaHeader.SetSrcPort (m_dport); 
  homaHeader.SetDstPort (m_sport);
  homaHeader.SetTxMsgId (m_txMsgId);
  homaHeader.SetMsgSize (m_msgSizeBytes);
  homaHeader.SetPktOffset (ackNo);
  uint16_t grantOffset = m_msgSizePkts == 0 ? 0 : std::min<uint16_t> (m_maxGrantableIdx, m_msgSizePkts - 1);
  homaHeader.SetGrantOffset (grantOffset);
  homaHeader.SetPrio (m_prio);
  homaHeader.SetPayloadSize (0);
  homaHeader.SetFlags (pktTypeFlag);
  
  Ptr<Packet> p = Create<Packet> ();
  p->AddHeader (homaHeader);
    
  SocketIpTosTag ipTosTag;
  ipTosTag.SetTos (0); // Grant packets have the highest priority
  // This packet may already have a SocketIpTosTag (see HomaSocket)
  p->ReplacePacketTag (ipTosTag);
    
  if (!isAck)
  {
    m_maxGrantedIdx = grantOffset;
    m_hasGrantedData = true;
  }
    
  return p;
}
    
// 扫描缺失分片并批量生成 RESEND 控制包列表。
std::list<Ptr<Packet>> HomaInboundMsg::GenerateResends (uint16_t maxRsndPktOffset)
{
  NS_LOG_FUNCTION (this << maxRsndPktOffset);
    
  std::list<Ptr<Packet>> rsndPkts;
    
  maxRsndPktOffset = std::min(maxRsndPktOffset, m_msgSizePkts);
  if (!m_hasGrantedData)
  {
    return rsndPkts;
  }
  maxRsndPktOffset = std::min(maxRsndPktOffset, m_maxGrantedIdx);
  for (uint16_t i = 0; i < maxRsndPktOffset; ++i)
  {
    if(!m_receivedPackets[i])
    {
      HomaHeader homaHeader;
      // Note we swap the src and dst port numbers for reverse direction
      homaHeader.SetSrcPort (m_dport); 
      homaHeader.SetDstPort (m_sport);
      homaHeader.SetTxMsgId (m_txMsgId);
      homaHeader.SetMsgSize (m_msgSizeBytes);
      homaHeader.SetPktOffset (i); 
      homaHeader.SetGrantOffset (m_maxGrantedIdx);
      homaHeader.SetPrio (m_prio);
      homaHeader.SetPayloadSize (0);
      homaHeader.SetFlags (HomaHeader::Flags_t::RESEND);
  
      Ptr<Packet> p = Create<Packet> ();
      p->AddHeader (homaHeader);
    
      SocketIpTosTag ipTosTag;
      ipTosTag.SetTos (0); // Resend packets have the highest priority
      // This packet may already have a SocketIpTosTag (see HomaSocket)
      p->ReplacePacketTag (ipTosTag);
        
      rsndPkts.push_back(p);
    }
  }
  
    return rsndPkts;
}
    
/******************************************************************************/

// 注册并返回 HomaRecvScheduler 的 TypeId。
TypeId HomaRecvScheduler::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaRecvScheduler")
    .SetParent<Object> ()
    .SetGroupName("Internet")
  ;
  return tid;
}
    
// 构造接收调度器并初始化 SIRD 相关全局/每发送方统计状态。
HomaRecvScheduler::HomaRecvScheduler (Ptr<HomaL4Protocol> homaL4Protocol)
{
  NS_LOG_FUNCTION (this);
      
  m_homa = homaL4Protocol;
  m_sirdGlobalCreditsInUsePkts = 0;
  m_sirdCeRatioEwma = 0.0;
  m_sirdDataPktsObserved = 0;
  m_sirdCeMarksObserved = 0;
  m_srrHaveLastGrantedSender = false;
  m_srrLastGrantedSender = 0;
  m_creditTickEvent = EventId ();
}

// 析构时检查是否仍有未完成入站消息，辅助排查接收流程问题。
HomaRecvScheduler::~HomaRecvScheduler ()
{
  NS_LOG_FUNCTION_NOARGS ();
    
  int numIncmpltMsg = m_inboundMsgs.size();
  if (numIncmpltMsg > 0)
  {
    NS_LOG_ERROR("ERROR: HomaRecvScheduler (" << this <<
                 ") couldn't completely deliver " << 
                 numIncmpltMsg << " active inbound messages!");
  }
}
    
// 接收侧入口：处理 DATA/BUSY，并在每次到包后重新评估是否应发放授权。
void HomaRecvScheduler::ReceivePacket (Ptr<Packet> packet, 
                                       Ipv4Header const &ipv4Header,
                                       HomaHeader const &homaHeader,
                                       Ptr<Ipv4Interface> interface)
{
  NS_LOG_FUNCTION (this << packet << ipv4Header << homaHeader);
    
  uint8_t rxFlag = homaHeader.GetFlags ();//是 Data 还是 busy
  if (rxFlag & HomaHeader::Flags_t::DATA )
  {
    if (m_homa->IsSirdEnabled ())
    {
      const uint32_t senderKey = ipv4Header.GetSource ().Get ();//用 sender 的 ip 做 key
      const double baseBudget = static_cast<double> (std::max<uint16_t> (1, m_homa->GetBdp ()));
      auto clampBudget = [baseBudget](double budget) {
        return std::max (1.0, std::min (baseBudget, budget));
      };
      if (m_sirdSenderBudgetNetPkts.find (senderKey) == m_sirdSenderBudgetNetPkts.end ())
      {
        m_sirdSenderBudgetNetPkts[senderKey] = baseBudget;
      }
      if (m_sirdSenderBudgetHostPkts.find (senderKey) == m_sirdSenderBudgetHostPkts.end ())
      {
        m_sirdSenderBudgetHostPkts[senderKey] = baseBudget;
      }
      if (m_sirdSenderNetAlpha.find (senderKey) == m_sirdSenderNetAlpha.end ())
      {
        m_sirdSenderNetAlpha[senderKey] = 0.0;
      }
      if (m_sirdSenderHostAlpha.find (senderKey) == m_sirdSenderHostAlpha.end ())
      {
        m_sirdSenderHostAlpha[senderKey] = 0.0;
      }

      bool senderCsn = (homaHeader.GetFeedbackFlags () & HomaHeader::FeedbackFlags_t::FEEDBACK_CSN) != 0;
      m_sirdSenderCsnState[senderKey] = senderCsn;//从 Homa 头里的 feedbackFlags 读取 CSN 位，并更新这个 sender 最近一次的 CSN 状态
      bool ceMarked = ipv4Header.GetEcn () == Ipv4Header::ECN_CE;
      m_sirdSenderCeState[senderKey] = ceMarked;

      m_sirdDataPktsObserved++;//用于统计或平滑估计
      m_sirdSenderDataPktsObserved[senderKey]++;
      m_sirdSenderEpochDataPkts[senderKey]++;
      if (ceMarked)
      {
        m_sirdCeMarksObserved++;
        m_sirdSenderCeMarksObserved[senderKey]++;
        m_sirdSenderEpochCeMarks[senderKey]++;
      }
      if (senderCsn)
      {
        m_sirdSenderCsnMarksObserved[senderKey]++;
        m_sirdSenderEpochCsnMarks[senderKey]++;
      }
      double sampleCeRatio = ceMarked ? 1.0 : 0.0;
      double alpha = m_homa->GetSirdEcnAlphaGain ();
      m_sirdCeRatioEwma = (1.0 - alpha) * m_sirdCeRatioEwma + alpha * sampleCeRatio;
      double netBudgetPkts = m_sirdSenderBudgetNetPkts[senderKey];
      double hostBudgetPkts = m_sirdSenderBudgetHostPkts[senderKey];

      // Reclaim outstanding scheduled credits as data arrives.
      if (packet->GetSize () > 0)//是 Data 包，负载大于 0
      {
        auto inUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
        if (inUseIt != m_sirdSenderCreditsInUsePkts.end () && inUseIt->second > 0)
        {
          inUseIt->second--;
          if (m_sirdGlobalCreditsInUsePkts > 0)
          {
            m_sirdGlobalCreditsInUsePkts--;
          }

          const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
          m_homa->TraceSirdBucketState (ipv4Header.GetDestination (),
                                        Ipv4Address (senderKey),
                                        hostBudgetPkts,
                                        inUseIt->second,
                                        m_sirdGlobalCreditsInUsePkts,
                                        globalBudgetPkts,
                                        2);
          double effectiveBudgetPkts = std::min (netBudgetPkts, hostBudgetPkts);
          uint32_t senderBudgetPkts = static_cast<uint32_t> (std::max (1.0, effectiveBudgetPkts));
          uint32_t senderAvailPkts = (senderBudgetPkts > inUseIt->second) ?
                                     (senderBudgetPkts - inUseIt->second) : 0;
          uint32_t receiverAvailPkts = (globalBudgetPkts > m_sirdGlobalCreditsInUsePkts) ?
                                       (globalBudgetPkts - m_sirdGlobalCreditsInUsePkts) : 0;
          m_homa->TraceSirdReceiverCreditState (ipv4Header.GetDestination (),
                                                Ipv4Address (senderKey),
                                                receiverAvailPkts,
                                                globalBudgetPkts,
                                                senderAvailPkts,
                                                senderBudgetPkts,
                                                2);
        }
      }//credit 归还

      const uint64_t epochPkts = std::max<uint16_t> (1, m_homa->GetBdp ());
      uint64_t epochDataPkts = m_sirdSenderEpochDataPkts[senderKey];
      if (epochDataPkts >= epochPkts)
      {
        double ceFraction = static_cast<double> (m_sirdSenderEpochCeMarks[senderKey]) /
                            static_cast<double> (epochDataPkts);
        double csnFraction = static_cast<double> (m_sirdSenderEpochCsnMarks[senderKey]) /
                             static_cast<double> (epochDataPkts);

        double netAlpha = m_sirdSenderNetAlpha[senderKey];
        netAlpha = (1.0 - alpha) * netAlpha + alpha * ceFraction;
        m_sirdSenderNetAlpha[senderKey] = netAlpha;

        double hostAlpha = m_sirdSenderHostAlpha[senderKey];
        hostAlpha = (1.0 - alpha) * hostAlpha + alpha * csnFraction;
        m_sirdSenderHostAlpha[senderKey] = hostAlpha;

        if (m_sirdSenderEpochCeMarks[senderKey] > 0)
        {
          netBudgetPkts = clampBudget (netBudgetPkts * (1.0 - netAlpha / 2.0));
        }
        else
        {
          netBudgetPkts = clampBudget (netBudgetPkts + 1.0);
        }
        m_sirdSenderBudgetNetPkts[senderKey] = netBudgetPkts;

        if (m_sirdSenderEpochCsnMarks[senderKey] > 0)
        {
          hostBudgetPkts = clampBudget (hostBudgetPkts * (1.0 - hostAlpha / 2.0));
        }
        else
        {
          hostBudgetPkts = clampBudget (hostBudgetPkts + 1.0);
        }
        m_sirdSenderBudgetHostPkts[senderKey] = hostBudgetPkts;

        const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
        double effectiveBudgetPkts = std::min (netBudgetPkts, hostBudgetPkts);
        uint32_t senderCreditsInUsePkts = 0;
        auto inUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
        if (inUseIt != m_sirdSenderCreditsInUsePkts.end ())
        {
          senderCreditsInUsePkts = inUseIt->second;
        }
        uint64_t loopState =
          (static_cast<uint64_t> (ceMarked ? 1 : 0)) |
          (static_cast<uint64_t> (senderCsn ? 1 : 0) << 1) |
          (static_cast<uint64_t> (3u) << 2) |
          (static_cast<uint64_t> (std::min<uint32_t> (senderCreditsInUsePkts, 0xFFFFu)) << 8) |
          (static_cast<uint64_t> (std::min<uint32_t> (m_sirdGlobalCreditsInUsePkts, 0xFFFFu)) << 24) |
          (static_cast<uint64_t> (std::min<uint32_t> (globalBudgetPkts, 0xFFFFu)) << 40);
        uint64_t counterState =
          (static_cast<uint64_t> (std::min<uint64_t> (m_sirdSenderCeMarksObserved[senderKey], 0x1FFFFFu))) |
          (static_cast<uint64_t> (std::min<uint64_t> (m_sirdSenderCsnMarksObserved[senderKey], 0x1FFFFFu)) << 21) |
          (static_cast<uint64_t> (std::min<uint64_t> (m_sirdSenderDataPktsObserved[senderKey], 0x1FFFFFu)) << 42);
        m_homa->TraceSirdLoopState (ipv4Header.GetDestination (),
                                    Ipv4Address (senderKey),
                                    netBudgetPkts,
                                    hostBudgetPkts,
                                    effectiveBudgetPkts,
                                    netAlpha,
                                    loopState,
                                    counterState);

        m_sirdSenderEpochDataPkts[senderKey] = 0;
        m_sirdSenderEpochCeMarks[senderKey] = 0;
        m_sirdSenderEpochCsnMarks[senderKey] = 0;
      }
    }

    this->ReceiveDataPacket (packet, ipv4Header, homaHeader, interface);
    // Sender is not busy since it is able to send data packets
    m_busySenders.erase(ipv4Header.GetSource().Get ());
  }
  else if (rxFlag & HomaHeader::Flags_t::BUSY)
  {
    m_busySenders.insert(ipv4Header.GetSource().Get ());
    // TODO: Is there anything else to do with a BUSY packet?
  }

  const uint32_t senderKey = ipv4Header.GetSource ().Get ();
  uint32_t senderCreditsInUsePkts = 0;
  auto senderInUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
  if (senderInUseIt != m_sirdSenderCreditsInUsePkts.end ())
  {
    senderCreditsInUsePkts = senderInUseIt->second;
  }
  uint8_t ecn = static_cast<uint8_t> (ipv4Header.GetEcn ());
  bool csn = (homaHeader.GetFeedbackFlags () & HomaHeader::FeedbackFlags_t::FEEDBACK_CSN) != 0;
  uint32_t senderCreditClamped = std::min<uint32_t> (senderCreditsInUsePkts, 0xFFFFu);
  uint32_t globalCreditClamped = std::min<uint32_t> (m_sirdGlobalCreditsInUsePkts, 0xFFFFu);
  uint32_t creditState = (globalCreditClamped << 16) | senderCreditClamped;
  m_homa->TraceSirdPacketState (ipv4Header.GetDestination (),
                                ipv4Header.GetSource (),
                                homaHeader.GetFlags (),
                                (static_cast<uint32_t> (homaHeader.GetTxMsgId ()) << 16) |
                                  static_cast<uint32_t> (homaHeader.GetPktOffset ()),
                                homaHeader.GetGrantOffset (),
                                ecn,
                                csn,
                                creditState);

  this->EnsureCreditTickScheduled (true);
}
    
// 把 DATA 分组归并到对应消息；若消息完成则上交，否则重排调度队列。
void HomaRecvScheduler::ReceiveDataPacket (Ptr<Packet> packet, 
                                           Ipv4Header const &ipv4Header,
                                           HomaHeader const &homaHeader,
                                           Ptr<Ipv4Interface> interface)
{
  NS_LOG_FUNCTION (this << ipv4Header << homaHeader);
    
  Ptr<Packet> cp = packet->Copy();
  Ptr<HomaInboundMsg> inboundMsg;
    
  int msgIdx = -1;
  if (this->FindInboundMsg (ipv4Header, homaHeader, msgIdx))
  {
    NS_ASSERT(msgIdx >= 0);
    inboundMsg = m_inboundMsgs[msgIdx];
    inboundMsg->ReceiveDataPacket (cp, homaHeader.GetPktOffset());
  }
  else
  {
    // First packet of a new inbound message: materialize per-message state and
    // arm its retransmission watchdog from the first granted window.
    inboundMsg = CreateObject<HomaInboundMsg> (cp, ipv4Header, homaHeader, interface,
                                               m_homa->GetMtu (), m_homa->GetBdp (),
                                               m_homa->UsesOptimizedMemory (), m_homa->IsSirdEnabled (),
                                               m_homa->GetSirdUnschThresholdPkts ());
    inboundMsg-> SetRtxEvent (Simulator::Schedule (m_homa->GetInboundRtxTimeout(), 
                                                   &HomaRecvScheduler::ExpireRtxTimeout, this, 
                                                   inboundMsg, inboundMsg->GetMaxGrantedIdx ()));
  }
    
  if (inboundMsg->IsFullyReceived ())
  {
    NS_LOG_LOGIC("HomaInboundMsg (" << inboundMsg <<
                 ") is fully received. Forwarding the message to applications.");
    this->ForwardUp (inboundMsg, msgIdx);
  }
  else
  {
    this->RescheduleInboundMsg (inboundMsg, msgIdx);
  }
}
    
// 按五元组+txMsgId 在活跃入站消息列表中查找目标消息。
bool HomaRecvScheduler::FindInboundMsg (Ipv4Header const &ipv4Header,
                                        HomaHeader const &homaHeader,
                                        int &msgIdx)
{
  NS_LOG_FUNCTION (this << ipv4Header << homaHeader);
    
  for (std::size_t i = 0; i < m_inboundMsgs.size(); ++i) 
  {
    if (m_inboundMsgs[i]->GetSrcAddress() == ipv4Header.GetSource() &&
        m_inboundMsgs[i]->GetDstAddress() == ipv4Header.GetDestination() &&
        m_inboundMsgs[i]->GetSrcPort() == homaHeader.GetSrcPort() &&
        m_inboundMsgs[i]->GetDstPort() == homaHeader.GetDstPort() &&
        m_inboundMsgs[i]->GetTxMsgId() == homaHeader.GetTxMsgId())
    {
      NS_LOG_LOGIC("The HomaInboundMsg (" << m_inboundMsgs[i]
                   << ") is found among the active inbound messages.");
      msgIdx = i;
      return true;
    }
  }
  
  NS_LOG_LOGIC("Incoming packet doesn't belong to an active inbound message.");
  return false;
}
    
// 将消息按策略（SRPT 或 SRR）插回活跃队列，维护接收调度顺序。
void HomaRecvScheduler::RescheduleInboundMsg (Ptr<HomaInboundMsg> inboundMsg,
                                              int msgIdx)
{
  NS_LOG_FUNCTION (this << inboundMsg << msgIdx);
    
  if (msgIdx >= 0)
  {
    NS_LOG_LOGIC("The HomaInboundMsg (" << inboundMsg 
                 << ") is already an active message. Reordering it.");
    
    // The caller says this message is already active; verify the index before
    // removing and reinserting it in the new scheduling position.
    Ptr<HomaInboundMsg> msgToReschedule = m_inboundMsgs[msgIdx];
    NS_ASSERT(msgToReschedule->GetSrcAddress() == inboundMsg->GetSrcAddress() &&
              msgToReschedule->GetDstAddress() == inboundMsg->GetDstAddress() &&
              msgToReschedule->GetSrcPort() == inboundMsg->GetSrcPort() &&
              msgToReschedule->GetDstPort() == inboundMsg->GetDstPort() &&
              msgToReschedule->GetTxMsgId() == inboundMsg->GetTxMsgId());
    
    m_inboundMsgs.erase(m_inboundMsgs.begin() + msgIdx);
  }

  // SRR-like mode: keep messages in FIFO order among active flows.
  if (m_homa->UseSrrScheduling ())
  {
    m_inboundMsgs.push_back (inboundMsg);
    return;
  }
  
  for(std::size_t i = 0; i < m_inboundMsgs.size(); ++i) 
  {
    if(inboundMsg->GetRemainingBytes () < m_inboundMsgs[i]->GetRemainingBytes())
    {
      m_inboundMsgs.insert(m_inboundMsgs.begin()+i, inboundMsg);
      return;
    }
  }
  // The remaining size of the inboundMsg is larger than all the active messages
  m_inboundMsgs.push_back(inboundMsg);
}
    
// 将完整消息上交 L4，并立即回送 ACK，然后清理该消息状态。
void HomaRecvScheduler::ForwardUp(Ptr<HomaInboundMsg> inboundMsg, int msgIdx)
{
  NS_LOG_FUNCTION (this << inboundMsg);
    
  m_homa->ForwardUp (inboundMsg->GetReassembledMsg(), 
                     inboundMsg->GetIpv4Header (),
                     inboundMsg->GetSrcPort(),
                     inboundMsg->GetDstPort(),
                     inboundMsg->GetTxMsgId(),
                     inboundMsg->GetIpv4Interface());
        
  m_homa->SendDown(inboundMsg->GenerateGrantOrAck(m_homa->GetNumUnschedPrioBands(), 
                                                  HomaHeader::Flags_t::ACK),
                   inboundMsg->GetDstAddress (),
                   inboundMsg->GetSrcAddress ());
    
  this->RemoveInboundMsg (inboundMsg, msgIdx);
}
    
// 取消定时器并从活跃列表移除入站消息。
void HomaRecvScheduler::RemoveInboundMsg (Ptr<HomaInboundMsg> inboundMsg, int msgIdx)
{
  NS_LOG_FUNCTION (this << inboundMsg << msgIdx);
  
  Simulator::Cancel (inboundMsg->GetRtxEvent ());
    
  if (msgIdx >= 0)
  {
    NS_ASSERT_MSG(m_inboundMsgs[msgIdx]->GetSrcAddress() == inboundMsg->GetSrcAddress() &&
                  m_inboundMsgs[msgIdx]->GetDstAddress() == inboundMsg->GetDstAddress() &&
                  m_inboundMsgs[msgIdx]->GetSrcPort() == inboundMsg->GetSrcPort() &&
                  m_inboundMsgs[msgIdx]->GetDstPort() == inboundMsg->GetDstPort() &&
                  m_inboundMsgs[msgIdx]->GetTxMsgId() == inboundMsg->GetTxMsgId(),
                  "State can not be cleared for HomaInboundMsg because the given msgIdx "
                  "is not consistent with the message itself!");
      
    NS_LOG_DEBUG("Erasing HomaInboundMsg (" << inboundMsg <<
                 ") from the active message list of HomaRecvScheduler (" <<
                 this << ").");
      
    m_inboundMsgs.erase(m_inboundMsgs.begin() + msgIdx);
  }
}

/*
 * This method is called everytime a packet (DATA or BUSY) is received because
 * both types of incoming packets may cause the reordering of active messages 
 * list which implies that we might need to send out a Grant.
 * The method loops over all the pending messages in the list of active messages
 * and checks whether they are grantable. If yes, an appropriate grant packet is 
 * generated and send down the networking stack. Although the method loops over
 * all the messages, ideally at most one message should be granted because other
 * messages should already be granted when they received packets for themselves.
 */
// 根据 overcommit、busy 状态与 SIRD 预算，扫描活跃消息并发放当前允许的 GRANT。
bool HomaRecvScheduler::IssuePendingGrants (void)
{
  NS_LOG_FUNCTION (this);
  bool grantIssued = false;

  if (!m_homa->IsSirdEnabled ())
  {
    std::unordered_set<uint32_t> grantedSenders; // Same sender can't be granted for multiple msgs at once
    uint8_t grantingPrio = m_homa->GetNumUnschedPrioBands (); // Scheduled priorities start here
    uint8_t overcommitDue = m_homa->GetOvercommitLevel ();

    Ptr<HomaInboundMsg> currentMsg;
    for (std::size_t i = 0; i < m_inboundMsgs.size(); ++i)
    {
      currentMsg = m_inboundMsgs[i];
      currentMsg->SetCurrentlyScheduled(false);

      if (overcommitDue > 0)
      {
        grantingPrio = std::min(grantingPrio, (uint8_t)(m_homa->GetNumTotalPrioBands()-1));

        Ipv4Address senderAddress = currentMsg->GetSrcAddress ();
        bool issuedThisRound = false;
        if (!currentMsg->IsFullyGranted () &&
            grantedSenders.find(senderAddress.Get ()) == grantedSenders.end())
        {
          if (m_busySenders.find(senderAddress.Get ()) == m_busySenders.end())
          {
            if (currentMsg->IsGrantable ())
            {
              m_homa->SendDown(currentMsg->GenerateGrantOrAck(grantingPrio,
                                                              HomaHeader::Flags_t::GRANT),
                               currentMsg->GetDstAddress (),
                               senderAddress);
              currentMsg->SetCurrentlyScheduled(true);
              issuedThisRound = true;
              grantIssued = true;
            }
          }
          if (issuedThisRound)
          {
            grantedSenders.insert(senderAddress.Get ());
            overcommitDue--;
            grantingPrio++;
          }
        }
      }
    }
    return grantIssued;
  }
    
  std::unordered_set<uint32_t> grantedSenders;
  uint8_t grantingPrio = m_homa->GetNumUnschedPrioBands ();
  uint8_t overcommitDue = m_homa->GetOvercommitLevel ();
  const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
  const double baseBudget = static_cast<double> (std::max<uint16_t> (1, m_homa->GetBdp ()));

  for (auto& inboundMsg : m_inboundMsgs)
    {
      inboundMsg->SetCurrentlyScheduled (false);
    }

  auto tryIssueGrant = [&] (Ptr<HomaInboundMsg> currentMsg) -> bool {
    if (overcommitDue == 0 || currentMsg->IsFullyGranted ())
      {
        return false;
      }

    Ipv4Address senderAddress = currentMsg->GetSrcAddress ();
    uint32_t senderKey = senderAddress.Get ();
    if (grantedSenders.find (senderKey) != grantedSenders.end () ||
        m_busySenders.find (senderKey) != m_busySenders.end ())
      {
        return false;
      }

    double netBudget = baseBudget;
    auto netBudgetIt = m_sirdSenderBudgetNetPkts.find (senderKey);
    if (netBudgetIt != m_sirdSenderBudgetNetPkts.end ())
      {
        netBudget = netBudgetIt->second;
      }

    bool senderCsn = false;
    auto csnIt = m_sirdSenderCsnState.find (senderKey);
    if (csnIt != m_sirdSenderCsnState.end ())
      {
        senderCsn = csnIt->second;
      }

    double hostBudget = baseBudget;
    auto hostBudgetIt = m_sirdSenderBudgetHostPkts.find (senderKey);
    if (hostBudgetIt != m_sirdSenderBudgetHostPkts.end ())
      {
        hostBudget = hostBudgetIt->second;
      }

    double budget = std::min (netBudget, hostBudget);
    if (m_sirdSenderCreditsInUsePkts.find (senderKey) == m_sirdSenderCreditsInUsePkts.end ())
      {
        m_sirdSenderCreditsInUsePkts[senderKey] = 0;
      }

    uint32_t senderBudgetPkts = static_cast<uint32_t> (std::max (1.0, budget));
    uint32_t senderInUsePkts = m_sirdSenderCreditsInUsePkts[senderKey];
    uint32_t senderAvailPkts = (senderBudgetPkts > senderInUsePkts) ?
                               (senderBudgetPkts - senderInUsePkts) : 0;
    uint32_t globalAvailPkts = (globalBudgetPkts > m_sirdGlobalCreditsInUsePkts) ?
                               (globalBudgetPkts - m_sirdGlobalCreditsInUsePkts) : 0;
    bool issuedThisRound = false;

    // SIRD issues one MSS-sized credit per tick. The credit bucket, not
    // data arrival, should decide whether the next grant can be exposed.
    if (senderAvailPkts > 0 && globalAvailPkts > 0 && !currentMsg->IsGrantable ())
      {
        currentMsg->AdvanceGrantableWindow (1);
      }

    if (senderAvailPkts > 0 && globalAvailPkts > 0 && currentMsg->IsGrantable ())
      {
        Ptr<Packet> grantPkt = currentMsg->GenerateGrantOrAck(grantingPrio,
                                                              HomaHeader::Flags_t::GRANT);
        HomaHeader traceHeader;
        grantPkt->PeekHeader (traceHeader);

        m_homa->TraceSirdGrantDecision (senderAddress,
                                        currentMsg->GetTxMsgId (),
                                        traceHeader.GetGrantOffset (),
                                        budget,
                                        m_sirdCeRatioEwma,
                                        senderCsn);

        m_homa->SendDown(grantPkt,
                         currentMsg->GetDstAddress (),
                         senderAddress);
        currentMsg->SetCurrentlyScheduled (true);
        m_sirdSenderCreditsInUsePkts[senderKey]++;
        m_sirdGlobalCreditsInUsePkts++;
        uint32_t senderAvailAfterPkts = (senderBudgetPkts > m_sirdSenderCreditsInUsePkts[senderKey]) ?
                                        (senderBudgetPkts - m_sirdSenderCreditsInUsePkts[senderKey]) : 0;
        uint32_t receiverAvailAfterPkts = (globalBudgetPkts > m_sirdGlobalCreditsInUsePkts) ?
                                          (globalBudgetPkts - m_sirdGlobalCreditsInUsePkts) : 0;
        m_homa->TraceSirdReceiverCreditState (currentMsg->GetDstAddress (),
                                              senderAddress,
                                              receiverAvailAfterPkts,
                                              globalBudgetPkts,
                                              senderAvailAfterPkts,
                                              senderBudgetPkts,
                                              1);
        issuedThisRound = true;
        grantIssued = true;
      }

    m_homa->TraceSirdBucketState (currentMsg->GetDstAddress (),
                                  senderAddress,
                                  hostBudget,
                                  m_sirdSenderCreditsInUsePkts[senderKey],
                                  m_sirdGlobalCreditsInUsePkts,
                                  globalBudgetPkts,
                                  issuedThisRound ? 1 : 0);

    if (issuedThisRound)
      {
        grantedSenders.insert (senderKey);
        overcommitDue--;
        grantingPrio++;
        if (m_homa->UseSrrScheduling ())
          {
            m_srrHaveLastGrantedSender = true;
            m_srrLastGrantedSender = senderKey;
          }
      }

    return issuedThisRound;
  };

  if (m_homa->UseSrrScheduling ())
    {
      std::vector<std::size_t> candidateIndices;
      std::unordered_set<uint32_t> candidateSenders;
      for (std::size_t i = 0; i < m_inboundMsgs.size (); ++i)
        {
          Ptr<HomaInboundMsg> currentMsg = m_inboundMsgs[i];
          uint32_t senderKey = currentMsg->GetSrcAddress ().Get ();
          if (currentMsg->IsFullyGranted () ||
              m_busySenders.find (senderKey) != m_busySenders.end () ||
              candidateSenders.find (senderKey) != candidateSenders.end ())
            {
              continue;
            }
          candidateSenders.insert (senderKey);
          candidateIndices.push_back (i);
        }

      if (m_srrHaveLastGrantedSender && !candidateIndices.empty ())
        {
          auto cursorIt = std::find_if (candidateIndices.begin (),
                                        candidateIndices.end (),
                                        [&] (std::size_t idx) {
                                          return m_inboundMsgs[idx]->GetSrcAddress ().Get () ==
                                                 m_srrLastGrantedSender;
                                        });
          if (cursorIt != candidateIndices.end ())
            {
              std::rotate (candidateIndices.begin (), cursorIt + 1, candidateIndices.end ());
            }
        }

      for (std::size_t idx : candidateIndices)
        {
          if (overcommitDue == 0)
            {
              break;
            }
          grantingPrio = std::min (grantingPrio, (uint8_t)(m_homa->GetNumTotalPrioBands () - 1));
          tryIssueGrant (m_inboundMsgs[idx]);
        }
      return grantIssued;
    }

  for (std::size_t i = 0; i < m_inboundMsgs.size (); ++i)
    {
      if (overcommitDue == 0)
        {
          break;
        }
      grantingPrio = std::min (grantingPrio, (uint8_t)(m_homa->GetNumTotalPrioBands () - 1));
      tryIssueGrant (m_inboundMsgs[i]);
    }
  return grantIssued;
}

void
HomaRecvScheduler::EnsureCreditTickScheduled (bool immediate)
{
  if (m_creditTickEvent.IsRunning ())
    {
      return;
    }

  if (!HasGrantOpportunity ())
    {
      return;
    }

  Time delay = immediate ? Time (0) : GetCreditTickInterval ();
  m_creditTickEvent = Simulator::Schedule (delay, &HomaRecvScheduler::CreditTick, this);
}

void
HomaRecvScheduler::CreditTick (void)
{
  m_creditTickEvent = EventId ();

  bool issued = IssuePendingGrants ();
  if (!issued)
    {
      return;
    }

  if (HasGrantOpportunity ())
    {
      m_creditTickEvent = Simulator::Schedule (GetCreditTickInterval (),
                                               &HomaRecvScheduler::CreditTick,
                                               this);
    }
}

bool
HomaRecvScheduler::HasGrantOpportunity (void) const
{
  const uint32_t globalBudgetPkts = std::max<uint32_t> (1, m_homa->GetSirdCreditBudgetPkts ());
  if (m_sirdGlobalCreditsInUsePkts >= globalBudgetPkts)
    {
      return false;
    }

  const double baseBudget = static_cast<double> (std::max<uint16_t> (1, m_homa->GetBdp ()));

  for (const auto& currentMsg : m_inboundMsgs)
    {
      uint32_t senderKey = currentMsg->GetSrcAddress ().Get ();
      if (currentMsg->IsFullyGranted ())
        {
          continue;
        }
      if (m_busySenders.find (senderKey) != m_busySenders.end ())
        {
          continue;
        }

      double netBudget = baseBudget;
      auto netIt = m_sirdSenderBudgetNetPkts.find (senderKey);
      if (netIt != m_sirdSenderBudgetNetPkts.end ())
        {
          netBudget = netIt->second;
        }

      double hostBudget = baseBudget;
      auto hostIt = m_sirdSenderBudgetHostPkts.find (senderKey);
      if (hostIt != m_sirdSenderBudgetHostPkts.end ())
        {
          hostBudget = hostIt->second;
        }

      double budget = std::min (netBudget, hostBudget);
      uint32_t senderBudgetPkts = static_cast<uint32_t> (std::max (1.0, budget));
      uint32_t senderInUsePkts = 0;
      auto inUseIt = m_sirdSenderCreditsInUsePkts.find (senderKey);
      if (inUseIt != m_sirdSenderCreditsInUsePkts.end ())
        {
          senderInUsePkts = inUseIt->second;
        }

      if (senderBudgetPkts > senderInUsePkts)
        {
          return true;
        }
    }
  return false;
}

Time
HomaRecvScheduler::GetCreditTickInterval (void) const
{
  return m_homa->GetFullDataPktTxTime ();
}
    
// 接收侧超时处理：必要时发 RESEND，重置定时器并更新无进展重传计数。
void HomaRecvScheduler::ExpireRtxTimeout(Ptr<HomaInboundMsg> inboundMsg,
                                         uint16_t maxRsndPktOffset)
{
  NS_LOG_FUNCTION (this << inboundMsg << maxRsndPktOffset);
    
  if (inboundMsg->IsFullyReceived ())
    return;
    
  // Create a dummy Homa Header for msg lookup
  HomaHeader homaHeader;
  homaHeader.SetSrcPort (inboundMsg->GetSrcPort ());
  homaHeader.SetDstPort (inboundMsg->GetDstPort ());
  homaHeader.SetTxMsgId (inboundMsg->GetTxMsgId ());
  int msgIdx = -1;
  if (this->FindInboundMsg (inboundMsg->GetIpv4Header (), homaHeader, msgIdx))
  {
    NS_ASSERT(msgIdx >= 0);
    if (inboundMsg->GetNumRtxWithoutProgress () >= m_homa->GetMaxNumRtxPerMsg())
    {
      NS_LOG_WARN(Simulator::Now ().GetNanoSeconds () <<
                  " Rtx Limit has been reached for the inbound Msg (" 
                  << inboundMsg << ").");
      this->RemoveInboundMsg (inboundMsg, msgIdx);
      return;
    }
      
    if (m_busySenders.find(inboundMsg->GetSrcAddress ().Get ()) == m_busySenders.end() &&
        inboundMsg->IsCurrentlyScheduled())
    {
      // We send RESEND packets only to non-busy senders with scheduled messages
      NS_LOG_LOGIC(Simulator::Now().GetNanoSeconds () << 
                   " Rtx Timer for an inbound Msg (" << inboundMsg << 
                   ") expired, which is scheduled. RESEND packets will be sent");
        
      std::list<Ptr<Packet>> rsndPkts = inboundMsg->GenerateResends (maxRsndPktOffset);
      while (!rsndPkts.empty())
      {
        m_homa->SendDown(rsndPkts.front(),
                         inboundMsg->GetDstAddress (),
                         inboundMsg->GetSrcAddress ());
        rsndPkts.pop_front();
      }
    }
      
    // Rechedule the next retransmission event for this message
    inboundMsg-> SetRtxEvent (Simulator::Schedule (m_homa->GetInboundRtxTimeout(), 
                                                   &HomaRecvScheduler::ExpireRtxTimeout, this, 
                                                   inboundMsg, inboundMsg->GetMaxGrantedIdx ()));
      
    // Update the LastRtxGrntIdx value of this message for the next timeout event
    uint16_t maxGrantableIdx = inboundMsg->GetMaxGrantableIdx ();
    if (inboundMsg->GetLastRtxGrntIdx () < maxGrantableIdx)
    {
      inboundMsg->ResetNumRtxWithoutProgress ();
    }
    else if (inboundMsg->IsCurrentlyScheduled())
    {
      inboundMsg->IncrNumRtxWithoutProgress ();
    }
    inboundMsg->SetLastRtxGrntIdx (maxGrantableIdx);
      
  }
  else
  {
    NS_LOG_DEBUG(Simulator::Now().GetNanoSeconds () << 
                 " Rtx Timer for an inbound Msg (" << inboundMsg << 
                 ") expired, which doesn't exist any more.");
  }  
  return;
}

} // namespace ns3
