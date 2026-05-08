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

namespace {

enum PathRttTriggerKind : uint8_t
{
  PATH_RTT_INITIAL_CREDIT_REQUEST = 0,
  PATH_RTT_FIRST_DATA = 1
};

struct PathRttResponseTxState
{
  uint8_t flags;
  uint16_t grantOffset;
  uint16_t pktOffset;
  Time txTime;
};

struct PathRttFlowState
{
  bool haveTriggerTx = false;
  Time triggerTxTime;
  bool haveTriggerRx = false;
  Time triggerRxTime;
  uint8_t triggerKind = PATH_RTT_FIRST_DATA;
  bool sampleEmitted = false;
  std::vector<PathRttResponseTxState> responseTxStates;
};

static std::unordered_map<std::string, PathRttFlowState> g_pathRttFlowStates;

static std::string
MakePathRttKey (Ipv4Address sender,
                Ipv4Address receiver,
                uint16_t sport,
                uint16_t dport,
                uint16_t txMsgId)
{
  std::ostringstream key;
  key << sender << ":" << sport
      << " " << receiver << ":" << dport
      << " " << txMsgId;
  return key.str ();
}

static bool
IsPathRttTriggerPacket (const HomaHeader& homaHeader)
{
  return (homaHeader.GetFlags () & HomaHeader::Flags_t::DATA) != 0 &&
         homaHeader.GetPktOffset () == 0;
}

static uint8_t
GetPathRttTriggerKind (const HomaHeader& homaHeader)
{
  return (homaHeader.GetPayloadSize () == 0) ? PATH_RTT_INITIAL_CREDIT_REQUEST
                                             : PATH_RTT_FIRST_DATA;
}

static bool
IsPathRttResponsePacket (const HomaHeader& homaHeader)
{
  return (homaHeader.GetFlags () & HomaHeader::Flags_t::GRANT) != 0 ||
         (homaHeader.GetFlags () & HomaHeader::Flags_t::ACK) != 0;
}

} // namespace

NS_LOG_COMPONENT_DEFINE ("HomaL4Protocol");

NS_OBJECT_ENSURE_REGISTERED (HomaL4Protocol);

/* The protocol is not standardized yet. Using a temporary number */
const uint8_t HomaL4Protocol::PROT_NUMBER = 198;
    
TypeId 
// 注册并返回 HomaL4Protocol 的 TypeId，同时声明可配置属性与跟踪源。
HomaL4Protocol::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::HomaL4Protocol")
    .SetParent<IpL4Protocol> ()
    .SetGroupName ("Internet")
    .AddConstructor<HomaL4Protocol> ()
    .AddAttribute ("SocketList", "The list of sockets associated to this protocol.",
                   ObjectVectorValue (),
                   MakeObjectVectorAccessor (&HomaL4Protocol::m_sockets),
                   MakeObjectVectorChecker<HomaSocket> ())
    .AddAttribute ("RttPackets", "RTT BDP in packets for Homa's in-flight window baseline.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&HomaL4Protocol::m_bdp),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("NumTotalPrioBands", "Total number of priority levels used within the network",
                   UintegerValue (8),
                   MakeUintegerAccessor (&HomaL4Protocol::m_numTotalPrioBands),
                   MakeUintegerChecker<uint8_t> ())
    .AddAttribute ("NumUnschedPrioBands", "Number of priority bands dedicated for unscheduled packets",
                   UintegerValue (2),
                   MakeUintegerAccessor (&HomaL4Protocol::m_numUnschedPrioBands),
                   MakeUintegerChecker<uint8_t> ())
    .AddAttribute ("OvercommitLevel", "Minimum number of messages to Grant at the same time",
                   UintegerValue (1),
                   MakeUintegerAccessor (&HomaL4Protocol::m_overcommitLevel),
                   MakeUintegerChecker<uint8_t> ())
    .AddAttribute ("UseSrrScheduling", "If true, receiver orders active inbound messages in FIFO/SRR-like order instead of SRPT by remaining bytes.",
             BooleanValue (false),
             MakeBooleanAccessor (&HomaL4Protocol::m_useSrrScheduling),
             MakeBooleanChecker ())
    .AddAttribute ("SirdEnabled", "Enable SIRD-compatible receiver-driven control loops.",
             BooleanValue (false),
             MakeBooleanAccessor (&HomaL4Protocol::m_sirdEnabled),
             MakeBooleanChecker ())
    .AddAttribute ("SirdCreditBudgetPkts", "Baseline per-sender grant budget in packets.",
             UintegerValue (12),
             MakeUintegerAccessor (&HomaL4Protocol::m_sirdCreditBudgetPkts),
             MakeUintegerChecker<uint16_t> (1))
    .AddAttribute ("SirdUnschThresholdPkts", "Line-rate startup threshold in packets.",
             UintegerValue (12),
             MakeUintegerAccessor (&HomaL4Protocol::m_sirdUnschThresholdPkts),
             MakeUintegerChecker<uint16_t> (1))
    .AddAttribute ("SirdEcnMdFactor", "Multiplicative decrease factor for ECN loop.",
             DoubleValue (0.85),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdEcnMdFactor),
             MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("SirdEcnAiStep", "Additive increase step in packets for ECN loop.",
             DoubleValue (1.0),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdEcnAiStep),
             MakeDoubleChecker<double> (0.0))
    .AddAttribute ("SirdSenderMdFactor", "Multiplicative decrease factor for sender feedback loop.",
             DoubleValue (0.8),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdSenderMdFactor),
             MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("SirdSenderAiStep", "Additive increase step in packets for sender feedback loop.",
             DoubleValue (1.0),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdSenderAiStep),
             MakeDoubleChecker<double> (0.0))
    .AddAttribute ("SirdEcnAlphaGain", "EWMA gain for ECN CE-ratio estimation.",
             DoubleValue (0.125),
             MakeDoubleAccessor (&HomaL4Protocol::m_sirdEcnAlphaGain),
             MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("SirdSenderCsnThreshold", "Accumulated credit threshold beyond which sender sets csn (packets).",
             UintegerValue (20),
             MakeUintegerAccessor (&HomaL4Protocol::m_sirdSenderCsnThreshold),
             MakeUintegerChecker<uint16_t> (0))
    .AddAttribute ("SirdSenderCreditLaunchDelay",
             "Sender-side delay between receiving scheduled credit and making that credit eligible to launch DATA.",
             TimeValue (NanoSeconds (0)),
             MakeTimeAccessor (&HomaL4Protocol::m_sirdSenderCreditLaunchDelay),
             MakeTimeChecker (NanoSeconds (0)))
    .AddAttribute ("InbndRtxTimeout", "Time value to determine the retransmission timeout of InboundMsgs",
                   TimeValue (MilliSeconds (1)),
                   MakeTimeAccessor (&HomaL4Protocol::m_inboundRtxTimeout),
                   MakeTimeChecker (MicroSeconds (0)))
    .AddAttribute ("OutbndRtxTimeout", "Time value to determine the timeout of OutboundMsgs",
                   TimeValue (MilliSeconds (10)),
                   MakeTimeAccessor (&HomaL4Protocol::m_outboundRtxTimeout),
                   MakeTimeChecker (MicroSeconds (0)))
    .AddAttribute ("MaxRtxCnt", "Maximum allowed consecutive rtx timeout count per message",
                   UintegerValue (5),
                   MakeUintegerAccessor (&HomaL4Protocol::m_maxNumRtxPerMsg),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("OptimizeMemory", 
                   "High performant mode (only packet sizes are stored to save from memory).",
                   BooleanValue (true),
                   MakeBooleanAccessor (&HomaL4Protocol::m_memIsOptimized),
                   MakeBooleanChecker ())
    .AddTraceSource ("MsgBegin",
                     "Trace source indicating a message has been delivered to "
                     "the HomaL4Protocol by the sender application layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_msgBeginTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("MsgFinish",
                     "Trace source indicating a message has been delivered to "
                     "the receiver application by the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_msgFinishTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("DataPktArrival",
                     "Trace source indicating a DATA packet has arrived "
                     "to the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_dataRecvTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("DataPktDeparture",
                     "Trace source indicating a DATA packet has departed "
                     "from the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_dataSendTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("CtrlPktArrival",
                     "Trace source indicating a control packet has arrived "
                     "to the HomaL4Protocol layer.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_ctrlRecvTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("CtrlPktArrivalTxMsg",
                     "Trace source indicating a control packet has arrived "
                     "to the HomaL4Protocol layer, including txMsgId.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_ctrlRecvTxMsgTrace),
                     "ns3::Packet::TracedCallback")
    .AddTraceSource ("PathRtt",
                     "Trace source for pure path RTT samples derived from Homa packet timing.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_pathRttTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdGrantDecision",
                     "Trace source for SIRD grant decisions.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdGrantDecisionTrace),
                     "ns3::TracedCallback")//上面 3 个 trace 已经弃用
    .AddTraceSource ("SirdBucketState",
                     "Trace source for SIRD per-sender/global bucket state.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdBucketStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdPacketState",
                     "Trace source for per-packet SIRD state (flags/credit/CE/CSN).",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdPacketStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdLoopState",
                     "Trace source for per-sender SIRD control-loop state.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdLoopStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdSenderCreditState",
                     "Trace source for sender-side scheduled credit currently held.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdSenderCreditStateTrace),
                     "ns3::TracedCallback")
    .AddTraceSource ("SirdReceiverCreditState",
                     "Trace source for receiver-side available credit after bucket updates.",
                     MakeTraceSourceAccessor (&HomaL4Protocol::m_sirdReceiverCreditStateTrace),
                     "ns3::TracedCallback")
  ;
  return tid;
}
    
// 构造函数：初始化端点复用器，并创建发送/接收两个核心调度器对象。
HomaL4Protocol::HomaL4Protocol ()
  : m_endPoints (new Ipv4EndPointDemux ())
{
  NS_LOG_FUNCTION (this);
      
  m_sendScheduler = CreateObject<HomaSendScheduler> (this);
  m_recvScheduler = CreateObject<HomaRecvScheduler> (this); 
}

// 析构函数：当前仅做日志记录，资源释放由 DoDispose 统一完成。
HomaL4Protocol::~HomaL4Protocol ()
{
  NS_LOG_FUNCTION_NOARGS ();
}
    
void 
// 将协议实例绑定到节点，读取底层网卡 MTU/链路速率，并初始化发送队列空闲时间。
HomaL4Protocol::SetNode (Ptr<Node> node)
{
  m_node = node;
    
  Ptr<NetDevice> netDevice = m_node->GetDevice (0);
  m_mtu = netDevice->GetMtu ();
    
  PointToPointNetDevice* p2pNetDevice = dynamic_cast<PointToPointNetDevice*>(&(*(netDevice)));
  m_linkRate = p2pNetDevice->GetDataRate ();
    
  m_nextTimeTxQueWillBeEmpty = Simulator::Now ();
}
    
Ptr<Node> 
// 返回挂载该协议实例的 ns-3 节点。
HomaL4Protocol::GetNode(void) const
{
  return m_node;
}
    
uint32_t
// 返回底层网卡的 MTU，用于消息分片和序列化开销估算。
HomaL4Protocol::GetMtu (void) const
{
  return m_mtu;
}
    
uint16_t 
// 返回协议配置中的 RTT BDP（单位：packet）。
HomaL4Protocol::GetBdp(void) const
{
  return m_bdp;
}

Time
HomaL4Protocol::GetFullDataPktTxTime (void) const
{
  PppHeader ppp;
  return m_linkRate.CalculateBytesTxTime (m_mtu + ppp.GetSerializedSize ());
}

uint32_t
HomaL4Protocol::GetAccumulatedSenderCreditPkts (void) const
{
  return m_sendScheduler ? m_sendScheduler->GetAccumulatedCreditPkts () : 0;
}
    
int 
// 返回 Homa 在 IPv4 中使用的协议号。
HomaL4Protocol::GetProtocolNumber (void) const
{
  return PROT_NUMBER;
}
 
Time
// 返回接收侧消息的重传超时时间。
HomaL4Protocol::GetInboundRtxTimeout(void) const
{
  return m_inboundRtxTimeout;
}
    
Time
// 返回发送侧消息的重传超时时间。
HomaL4Protocol::GetOutboundRtxTimeout(void) const
{
  return m_outboundRtxTimeout;
}
    
uint16_t 
// 返回单条消息允许连续触发的最大重传超时次数。
HomaL4Protocol::GetMaxNumRtxPerMsg(void) const
{
  return m_maxNumRtxPerMsg;
}
    
uint8_t
// 返回网络支持的总优先级带数。
HomaL4Protocol::GetNumTotalPrioBands (void) const
{
  return m_numTotalPrioBands;
}
    
uint8_t
// 返回保留给 unscheduled DATA 的优先级带数。
HomaL4Protocol::GetNumUnschedPrioBands (void) const
{
  return m_numUnschedPrioBands;
}
    
uint8_t
// 返回接收端并发发放 grant 的 overcommit 配置。
HomaL4Protocol::GetOvercommitLevel (void) const
{
  return m_overcommitLevel;
}

bool
// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
HomaL4Protocol::UseSrrScheduling (void) const
{
  return m_useSrrScheduling;
}

bool
// 根据当前状态进行条件判断，返回布尔决策供上层流程分支使用。
HomaL4Protocol::IsSirdEnabled (void) const
{
  return m_sirdEnabled;
}

uint16_t
// 返回 SIRD 接收端的全局 credit budget（单位：packet）。
HomaL4Protocol::GetSirdCreditBudgetPkts (void) const
{
  return m_sirdCreditBudgetPkts;
}

uint16_t
// 返回 unscheduled startup 的阈值；超过该阈值的消息需要显式 grant。
HomaL4Protocol::GetSirdUnschThresholdPkts (void) const
{
  return m_sirdUnschThresholdPkts;
}

double
// 返回 ECN 控制环的乘法减因子。
HomaL4Protocol::GetSirdEcnMdFactor (void) const
{
  return m_sirdEcnMdFactor;
}

double
// 返回 ECN 控制环的加法增步长。
HomaL4Protocol::GetSirdEcnAiStep (void) const
{
  return m_sirdEcnAiStep;
}

double
// 返回 sender feedback 控制环的乘法减因子。
HomaL4Protocol::GetSirdSenderMdFactor (void) const
{
  return m_sirdSenderMdFactor;
}

double
// 返回 sender feedback 控制环的加法增步长。
HomaL4Protocol::GetSirdSenderAiStep (void) const
{
  return m_sirdSenderAiStep;
}

double
// 返回 CE 比例 EWMA 的更新增益。
HomaL4Protocol::GetSirdEcnAlphaGain (void) const
{
  return m_sirdEcnAlphaGain;
}

uint16_t
// 返回发送端开始置位 CSN 的 credit 积压阈值。
HomaL4Protocol::GetSirdSenderCsnThreshold (void) const
{
  return m_sirdSenderCsnThreshold;
}

Time
HomaL4Protocol::GetSirdSenderCreditLaunchDelay (void) const
{
  return m_sirdSenderCreditLaunchDelay;
}

void
// 封装 SIRD 授权决策的 trace 回调，向外暴露发送方预算与拥塞状态。
HomaL4Protocol::TraceSirdGrantDecision (Ipv4Address sender,
                                        uint16_t txMsgId,
                                        uint16_t grantOffset,
                                        double senderBudgetPkts,
                                        double ecnEwma,
                                        bool senderCsn)
{
  m_sirdGrantDecisionTrace (sender,
                            txMsgId,
                            grantOffset,
                            senderBudgetPkts,
                            ecnEwma,
                            senderCsn);
}

void
// 封装 SIRD bucket 状态的 trace 回调，便于观测 per-sender/global credit 变化。
HomaL4Protocol::TraceSirdBucketState (Ipv4Address receiver,
                                      Ipv4Address sender,
                                      double senderBudgetHostPkts,
                                      uint32_t senderCreditsInUsePkts,
                                      uint32_t globalCreditsInUsePkts,
                                      uint32_t globalBudgetPkts,
                                      uint8_t eventType)
{
  m_sirdBucketStateTrace (receiver,
                          sender,
                          senderBudgetHostPkts,
                          senderCreditsInUsePkts,
                          globalCreditsInUsePkts,
                          globalBudgetPkts,
                          eventType);
}

void
// 封装每包状态 trace 回调，暴露 flags/credit/CE/CSN 等关键字段。
HomaL4Protocol::TraceSirdPacketState (Ipv4Address receiver,
                                      Ipv4Address sender,
                                      uint8_t flags,
                                      uint32_t msgPktState,
                                      uint16_t grantOffset,
                                      uint8_t ecn,
                                      bool csn,
                                      uint32_t creditState)
{
  m_sirdPacketStateTrace (receiver,
                          sender,
                          flags,
                          msgPktState,
                          grantOffset,
                          ecn,
                          csn,
                          creditState);
}

void
HomaL4Protocol::TraceSirdLoopState (Ipv4Address receiver,
                                    Ipv4Address sender,
                                    double netBudgetPkts,
                                    double hostBudgetPkts,
                                    double effectiveBudgetPkts,
                                    double ceEwma,
                                    uint64_t loopState,
                                    uint64_t counterState)
{
  m_sirdLoopStateTrace (receiver,
                        sender,
                        netBudgetPkts,
                        hostBudgetPkts,
                        effectiveBudgetPkts,
                        ceEwma,
                        loopState,
                        counterState);
}

void
HomaL4Protocol::TraceSirdSenderCreditState (Ipv4Address sender,
                                            Ipv4Address receiver,
                                            uint16_t txMsgId,
                                            uint32_t senderCreditPkts,
                                            uint8_t eventType)
{
  m_sirdSenderCreditStateTrace (sender,
                                receiver,
                                txMsgId,
                                senderCreditPkts,
                                eventType);
}

void
HomaL4Protocol::TraceSirdReceiverCreditState (Ipv4Address receiver,
                                              Ipv4Address sender,
                                              uint32_t receiverAvailPkts,
                                              uint32_t receiverBudgetPkts,
                                              uint32_t senderAvailPkts,
                                              uint32_t senderBudgetPkts,
                                              uint8_t eventType)
{
  m_sirdReceiverCreditStateTrace (receiver,
                                  sender,
                                  receiverAvailPkts,
                                  receiverBudgetPkts,
                                  senderAvailPkts,
                                  senderBudgetPkts,
                                  eventType);
}
    
// 返回是否启用了“仅保留分组大小”的轻量消息缓存模式。
bool HomaL4Protocol::UsesOptimizedMemory (void)
{
  return m_memIsOptimized;
}
    
/*
 * This method is called by AggregateObject and completes the aggregation
 * by setting the node in the homa stack and link it to the ipv4 object
 * present in the node along with the socket factory
 */
void
// 完成与 Node/IPv4 的聚合绑定，创建 SocketFactory，并注册到 IPv4 上下行路径。
HomaL4Protocol::NotifyNewAggregate ()
{
  NS_LOG_FUNCTION (this);
  Ptr<Node> node = this->GetObject<Node> ();
  Ptr<Ipv4> ipv4 = this->GetObject<Ipv4> ();
    
  NS_ASSERT_MSG(ipv4, "Homa L4 Protocol supports only IPv4.");

  if (m_node == 0)
    {
      if ((node != 0) && (ipv4 != 0))
        {
          this->SetNode (node);
          Ptr<HomaSocketFactory> homaFactory = CreateObject<HomaSocketFactory> ();
          homaFactory->SetHoma (this);
          node->AggregateObject (homaFactory);
          
          NS_ASSERT(m_mtu); // m_mtu is set inside SetNode() above.
          NS_ASSERT_MSG(m_numTotalPrioBands > m_numUnschedPrioBands,
                "Total number of priority bands should be larger than the number of bands dedicated for unscheduled packets.");
          NS_ASSERT(m_outboundRtxTimeout != Time(0));
          NS_ASSERT(m_inboundRtxTimeout != Time(0));
        }
    }
  
  if (ipv4 != 0 && m_downTarget.IsNull())
    {
      // We register this HomaL4Protocol instance as one of the upper targets of the IP layer
      ipv4->Insert (this);
      // We set our down target to the IPv4 send function.
      this->SetDownTarget (MakeCallback (&Ipv4::Send, ipv4));
    }
  IpL4Protocol::NotifyNewAggregate ();
}
    
void
// 释放协议层对象持有的 socket/endPoint 等资源，并断开下行回调。
HomaL4Protocol::DoDispose (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  for (std::vector<Ptr<HomaSocket> >::iterator i = m_sockets.begin (); i != m_sockets.end (); i++)
    {
      *i = 0;
    }
  m_sockets.clear ();

  if (m_endPoints != 0)
    {
      delete m_endPoints;
      m_endPoints = 0;
    }
  
  m_node = 0;
  m_downTarget.Nullify ();
/*
 = MakeNullCallback<void,Ptr<Packet>, Ipv4Address, Ipv4Address, uint8_t, Ptr<Ipv4Route> > ();
*/
  IpL4Protocol::DoDispose ();
}
 
/*
 * This method is called by HomaSocketFactory associated with m_node which 
 * returns a socket that is tied to this HomaL4Protocol instance.
 */
Ptr<Socket>
// 创建并初始化一个 HomaSocket，使其与当前节点和协议实例关联。
HomaL4Protocol::CreateSocket (void)
{
  NS_LOG_FUNCTION_NOARGS ();
  Ptr<HomaSocket> socket = CreateObject<HomaSocket> ();
  socket->SetNode (m_node);
  socket->SetHoma (this);
  m_sockets.push_back (socket);
  return socket;
}
    
Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (void)
{
  NS_LOG_FUNCTION (this);
  return m_endPoints->Allocate ();
}

Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ipv4Address address)
{
  NS_LOG_FUNCTION (this << address);
  return m_endPoints->Allocate (address);
}

Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ptr<NetDevice> boundNetDevice, uint16_t port)
{
  NS_LOG_FUNCTION (this << boundNetDevice << port);
  return m_endPoints->Allocate (boundNetDevice, port);
}

Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ptr<NetDevice> boundNetDevice, Ipv4Address address, uint16_t port)
{
  NS_LOG_FUNCTION (this << boundNetDevice << address << port);
  return m_endPoints->Allocate (boundNetDevice, address, port);
}
Ipv4EndPoint *
// 向端点复用器申请本地端点（支持不同绑定粒度的重载形式）。
HomaL4Protocol::Allocate (Ptr<NetDevice> boundNetDevice,
                         Ipv4Address localAddress, uint16_t localPort,
                         Ipv4Address peerAddress, uint16_t peerPort)
{
  NS_LOG_FUNCTION (this << boundNetDevice << localAddress << localPort << peerAddress << peerPort);
  return m_endPoints->Allocate (boundNetDevice,
                                localAddress, localPort,
                                peerAddress, peerPort);
}

void 
// 归还此前分配的 IPv4 端点，释放端口绑定状态。
HomaL4Protocol::DeAllocate (Ipv4EndPoint *endPoint)
{
  NS_LOG_FUNCTION (this << endPoint);
  m_endPoints->DeAllocate (endPoint);
}
    

/*
 * This method is called either by the associated HomaSendScheduler after 
 * the next data packet to transmit is selected or by the associated
 * HomaRecvScheduler once a control packet is generated. The selected 
 * packet is then pushed down to the lower IP layer.
 */
void
// 统一下发出口：更新链路序列化队列时间，记录发送 trace，并调用 IPv4 下行回调。
HomaL4Protocol::SendDown (Ptr<Packet> packet, 
                          Ipv4Address saddr, Ipv4Address daddr, 
                          Ptr<Ipv4Route> route)
{
  NS_LOG_FUNCTION (this << packet << saddr << daddr << route);
    
  PppHeader pph;
  Ipv4Header iph;
  HomaHeader homaHeader;
  packet->PeekHeader(homaHeader);
    
  uint32_t headerSize = iph.GetSerializedSize () + pph.GetSerializedSize ();  
  Time timeToSerialize = m_linkRate.CalculateBytesTxTime (packet->GetSize () + headerSize);
    
  if(Simulator::Now() <= m_nextTimeTxQueWillBeEmpty)
  {
    m_nextTimeTxQueWillBeEmpty += timeToSerialize;
  }
  else
  {
    m_nextTimeTxQueWillBeEmpty = Simulator::Now() + timeToSerialize;
  }
    
  if (homaHeader.GetFlags () & HomaHeader::Flags_t::DATA)
  {
    uint32_t payloadSize = m_mtu - iph.GetSerializedSize () - homaHeader.GetSerializedSize ();
    uint32_t msgSizeBytes = homaHeader.GetMsgSize ();
    uint16_t msgSizePkts = msgSizeBytes / payloadSize + (msgSizeBytes % payloadSize != 0);
    uint16_t remainingPkts = msgSizePkts - homaHeader.GetGrantOffset () - (uint16_t)1 + m_bdp; 
    m_dataSendTrace(packet, saddr, daddr, homaHeader.GetSrcPort (), 
                    homaHeader.GetDstPort (), homaHeader.GetTxMsgId (), 
                    homaHeader.GetPktOffset (), remainingPkts);

    if (IsPathRttTriggerPacket (homaHeader))
      {
        std::string key = MakePathRttKey (saddr,
                                          daddr,
                                          homaHeader.GetSrcPort (),
                                          homaHeader.GetDstPort (),
                                          homaHeader.GetTxMsgId ());
        PathRttFlowState& state = g_pathRttFlowStates[key];
        if (!state.sampleEmitted && !state.haveTriggerTx)
          {
            state.haveTriggerTx = true;
            state.triggerTxTime = Simulator::Now ();
            state.triggerKind = GetPathRttTriggerKind (homaHeader);
          }
      }
  }
  else if (IsPathRttResponsePacket (homaHeader))
  {
    std::string key = MakePathRttKey (daddr,
                                      saddr,
                                      homaHeader.GetDstPort (),
                                      homaHeader.GetSrcPort (),
                                      homaHeader.GetTxMsgId ());
    auto it = g_pathRttFlowStates.find (key);
    if (it != g_pathRttFlowStates.end () &&
        !it->second.sampleEmitted &&
        it->second.haveTriggerRx)
      {
        it->second.responseTxStates.push_back ({homaHeader.GetFlags (),
                                                homaHeader.GetGrantOffset (),
                                                homaHeader.GetPktOffset (),
                                                Simulator::Now ()});
      }
  }
   
  m_downTarget (packet, saddr, daddr, PROT_NUMBER, route);
}
    
// 返回当前链路发送队列距排空还剩多久。
Time HomaL4Protocol::GetTxQueueDrainDelay ()
{
  NS_LOG_FUNCTION(this);
    
  if(Simulator::Now() < m_nextTimeTxQueWillBeEmpty)
  {
    return m_nextTimeTxQueWillBeEmpty - Simulator::Now();
  }
  else
  {
    return Time(0);
  }
}

/*
 * This method is called by the lower IP layer to notify arrival of a 
 * new packet from the network. The method then classifies the packet
 * and forward it to the appropriate scheduler (send or receive) to have
 * Homa Transport logic applied on it.
 */
enum IpL4Protocol::RxStatus
// 网络接收入口：校验报文并按 DATA/BUSY 与 GRANT/RESEND/ACK 分发到接收或发送调度器。
HomaL4Protocol::Receive (Ptr<Packet> packet,
                        Ipv4Header const &header,
                        Ptr<Ipv4Interface> interface)
{
  NS_LOG_FUNCTION (this << packet << header << interface);
    
  NS_LOG_DEBUG ("HomaL4Protocol (" << this << ") received: " << packet->ToString ());
    
  NS_ASSERT(header.GetProtocol() == PROT_NUMBER);
  
  Ptr<Packet> cp = packet->Copy ();
    
  HomaHeader homaHeader;
  cp->RemoveHeader(homaHeader);//homaHeader：拿到了包里的 Homa 头字段
  //cp：头已经被移除，只剩 payload

  NS_ASSERT_MSG(cp->GetSize()==homaHeader.GetPayloadSize(),
                "HomaL4Protocol (" << this << ") received a packet "
                " whose payload size doesn't match the homa header field!");

  NS_LOG_DEBUG ("Looking up dst " << header.GetDestination () << " port " << homaHeader.GetDstPort ()); 
  Ipv4EndPointDemux::EndPoints endPoints =
    m_endPoints->Lookup (header.GetDestination (), homaHeader.GetDstPort (),
                         header.GetSource (), homaHeader.GetSrcPort (), interface);
  if (endPoints.empty ())
    {
      NS_LOG_LOGIC ("RX_ENDPOINT_UNREACH");
      return IpL4Protocol::RX_ENDPOINT_UNREACH;
    }
    
  //  The Homa protocol logic starts here!
  uint8_t rxFlag = homaHeader.GetFlags ();
  if (rxFlag & HomaHeader::Flags_t::DATA)
    {
      if (IsPathRttTriggerPacket (homaHeader))
        {
          std::string key = MakePathRttKey (header.GetSource (),
                                            header.GetDestination (),
                                            homaHeader.GetSrcPort (),
                                            homaHeader.GetDstPort (),
                                            homaHeader.GetTxMsgId ());
          PathRttFlowState& state = g_pathRttFlowStates[key];
          if (!state.sampleEmitted && !state.haveTriggerRx)
            {
              state.haveTriggerRx = true;
              state.triggerRxTime = Simulator::Now ();
              state.triggerKind = GetPathRttTriggerKind (homaHeader);
            }
        }
    }
  else if (IsPathRttResponsePacket (homaHeader))
    {
      std::string key = MakePathRttKey (header.GetDestination (),
                                        header.GetSource (),
                                        homaHeader.GetDstPort (),
                                        homaHeader.GetSrcPort (),
                                        homaHeader.GetTxMsgId ());
      auto stateIt = g_pathRttFlowStates.find (key);
      if (stateIt != g_pathRttFlowStates.end () &&
          !stateIt->second.sampleEmitted &&
          stateIt->second.haveTriggerTx &&
          stateIt->second.haveTriggerRx)
        {
          auto& responseTxStates = stateIt->second.responseTxStates;
          for (auto respIt = responseTxStates.begin (); respIt != responseTxStates.end (); ++respIt)
            {
              if (respIt->flags == homaHeader.GetFlags () &&
                  respIt->grantOffset == homaHeader.GetGrantOffset () &&
                  respIt->pktOffset == homaHeader.GetPktOffset ())
                {
                  Time receiverDelay = respIt->txTime - stateIt->second.triggerRxTime;
                  Time pathRtt = (Simulator::Now () - stateIt->second.triggerTxTime) - receiverDelay;
                  if (pathRtt.GetNanoSeconds () >= 0)
                    {
                      m_pathRttTrace (header.GetDestination (),
                                      header.GetSource (),
                                      homaHeader.GetDstPort (),
                                      homaHeader.GetSrcPort (),
                                      homaHeader.GetTxMsgId (),
                                      stateIt->second.triggerKind,
                                      homaHeader.GetFlags (),
                                      pathRtt);
                    }
                  stateIt->second.sampleEmitted = true;
                  stateIt->second.responseTxStates.clear ();
                  break;
                }
            }
        }
    }

  if (rxFlag & HomaHeader::Flags_t::DATA ||
      rxFlag & HomaHeader::Flags_t::BUSY)
  {
    m_recvScheduler->ReceivePacket(cp, header, homaHeader, interface);//是 Data 或者 Busy 包，调度器受着
  }
  else if ((rxFlag & HomaHeader::Flags_t::GRANT) ||
           (rxFlag & HomaHeader::Flags_t::RESEND) ||
           (rxFlag & HomaHeader::Flags_t::ACK))
  {
    // GRANT/RESEND/ACK 只影响发送端消息状态，交给发送调度器处理。
    m_sendScheduler->HandleControlPacketForOutboundMsg (header, homaHeader);
  }
  else
  {
    NS_LOG_ERROR("ERROR: HomaL4Protocol received an unknown type of a packet: " 
                 << homaHeader.FlagsToString(rxFlag));
    return IpL4Protocol::RX_ENDPOINT_UNREACH;
  }
    
  if (rxFlag & HomaHeader::Flags_t::DATA)
    m_dataRecvTrace(cp, header.GetSource (), header.GetDestination (), 
                    homaHeader.GetSrcPort (), homaHeader.GetDstPort (), 
                    homaHeader.GetTxMsgId (), homaHeader.GetPktOffset (), 
                    homaHeader.GetPrio ());//trace 用
  else
  {
    m_ctrlRecvTrace(cp, header.GetSource (), header.GetDestination (), 
                    homaHeader.GetSrcPort (), homaHeader.GetDstPort (), 
                    homaHeader.GetFlags (), homaHeader.GetGrantOffset(), 
                    homaHeader.GetPrio());
    m_ctrlRecvTxMsgTrace(header.GetSource (), header.GetDestination (),
                         homaHeader.GetSrcPort (), homaHeader.GetDstPort (),
                         homaHeader.GetTxMsgId (), homaHeader.GetFlags (),
                         homaHeader.GetGrantOffset (), homaHeader.GetPrio ());
    uint8_t ecn = static_cast<uint8_t> (header.GetEcn ());
    bool csn = (homaHeader.GetFeedbackFlags () & HomaHeader::FeedbackFlags_t::FEEDBACK_CSN) != 0;
    TraceSirdPacketState (header.GetDestination (),
                          header.GetSource (),
                          homaHeader.GetFlags (),
                          (static_cast<uint32_t> (homaHeader.GetTxMsgId ()) << 16) |
                            static_cast<uint32_t> (homaHeader.GetPktOffset ()),
                          homaHeader.GetGrantOffset (),
                          ecn,
                          csn,
                          0);
  }
    
  return IpL4Protocol::RX_OK;
}
    
enum IpL4Protocol::RxStatus
// 网络接收入口：校验报文并按 DATA/BUSY 与 GRANT/RESEND/ACK 分发到接收或发送调度器。
HomaL4Protocol::Receive (Ptr<Packet> packet,
                        Ipv6Header const &header,
                        Ptr<Ipv6Interface> interface)
{
  NS_FATAL_ERROR_CONT("HomaL4Protocol currently doesn't support IPv6. Use IPv4 instead.");
  return IpL4Protocol::RX_ENDPOINT_UNREACH;
}

// inherited from Ipv4L4Protocol (Not used for Homa Transport Purposes)
void 
// 处理 ICMP 反馈：解析原始端口并转交对应端点，便于上层感知网络错误。
HomaL4Protocol::ReceiveIcmp (Ipv4Address icmpSource, uint8_t icmpTtl,
                            uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo,
                            Ipv4Address payloadSource,Ipv4Address payloadDestination,
                            const uint8_t payload[8])
{
  NS_LOG_FUNCTION (this << icmpSource << icmpTtl << icmpType << icmpCode << icmpInfo 
                        << payloadSource << payloadDestination);
  uint16_t src, dst;
  src = payload[0] << 8;
  src |= payload[1];
  dst = payload[2] << 8;
  dst |= payload[3];

  Ipv4EndPoint *endPoint = m_endPoints->SimpleLookup (payloadSource, src, payloadDestination, dst);
  if (endPoint != 0)
    {
      endPoint->ForwardIcmp (icmpSource, icmpTtl, icmpType, icmpCode, icmpInfo);
    }
  else
    {
      NS_LOG_DEBUG ("no endpoint found source=" << payloadSource <<
                    ", destination="<<payloadDestination<<
                    ", src=" << src << ", dst=" << dst);
    }
}
    
void
// 设置 IPv4 下行发送回调，使 Homa 能把封装后的分组交给网络层。
HomaL4Protocol::SetDownTarget (IpL4Protocol::DownTargetCallback callback)
{
  NS_LOG_FUNCTION (this);
  m_downTarget = callback;
}
    
void
// IPv6 下行回调设置接口；当前实现明确不支持 IPv6 并直接报错。
HomaL4Protocol::SetDownTarget6 (IpL4Protocol::DownTargetCallback6 callback)
{
  NS_LOG_FUNCTION (this);
  NS_FATAL_ERROR("HomaL4Protocol currently doesn't support IPv6. Use IPv4 instead.");
  m_downTarget6 = callback;
}

IpL4Protocol::DownTargetCallback
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetDownTarget (void) const
{
  return m_downTarget;
}
    
IpL4Protocol::DownTargetCallback6
// 读取并返回当前对象中的配置或运行时状态值（只读访问，不修改内部状态）。
HomaL4Protocol::GetDownTarget6 (void) const
{
  NS_FATAL_ERROR("HomaL4Protocol currently doesn't support IPv6. Use IPv4 instead.");
  return m_downTarget6;
}

// 清理完成或失效消息的发送状态，回收 txMsgId。
void HomaSendScheduler::ClearStateForMsg (uint16_t txMsgId)
{
  NS_LOG_FUNCTION(this << txMsgId);

  Ptr<HomaOutboundMsg> outMsg = m_outboundMsgs[txMsgId];
  if (outMsg)
    {
      m_homa->TraceSirdSenderCreditState (outMsg->GetSrcAddress (),
                                          outMsg->GetDstAddress (),
                                          txMsgId,
                                          0,
                                          4);
    }
  std::string key = MakePathRttKey (outMsg->GetSrcAddress (),
                                    outMsg->GetDstAddress (),
                                    outMsg->GetSrcPort (),
                                    outMsg->GetDstPort (),
                                    txMsgId);
  g_pathRttFlowStates.erase (key);

  Simulator::Cancel (outMsg->GetRtxEvent ());
  m_outboundMsgs.erase(m_outboundMsgs.find(txMsgId));
  m_txMsgIdFreeList.push_back(txMsgId);
}

} // namespace ns3
