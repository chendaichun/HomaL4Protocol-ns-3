/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2021 Google LLC
 *
 * All rights reserved.
 *
 * Author: Serhat Arslan <serhatarslan@google.com>
 */

#include "pfifo-bolt-queue-disc.h"

#include "ns3/boolean.h"
#include "ns3/internet-module.h"
#include "ns3/log.h"
#include "ns3/network-module.h"
#include "ns3/object-factory.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/queue.h"
#include "ns3/socket.h"

namespace ns3 {
  /* ======== Fixed-point helpers ======== */
inline uint32_t
PfifoBoltQueueDisc::Idx(uint16_t a, uint16_t b) const noexcept
{
  return ((a ^ b) & (kFlowTableSize - 1));   // 低 10 位异或
}
inline void
PfifoBoltQueueDisc::UpdateSrtt(uint32_t idx, uint32_t sample_ns)
{
    if (sample_ns == 0) return;          // 异常样本直接丢
    FlowState &st = m_flowTbl[idx];
    //std::cout << sample_ns << std::endl;
    /* ---- 判断是否需要重算 ---- */
    if (st.srtt_ns) {
        uint64_t delta = (st.srtt_ns > sample_ns)
                           ? st.srtt_ns - sample_ns
                           : sample_ns - st.srtt_ns;
        if (delta * 32u < st.srtt_ns) {      // <3 % ⇒ 不更新
            st.srtt_ns = sample_ns;          // 仍然记录最新 RTT
            return;
        }
    }
    /* ---- 真正“重算”分两件事 ---- */
    st.srtt_ns = sample_ns;

    /* ②  */
    //st.weight = 50000/st.srtt_ns;
    st.weight = st.srtt_ns;
    //std::cout << "dg" << st.srtt_ns << ' ' << st.weight << std::endl;
}


NS_LOG_COMPONENT_DEFINE("PfifoBoltQueueDisc");

NS_OBJECT_ENSURE_REGISTERED(PfifoBoltQueueDisc);

TypeId PfifoBoltQueueDisc::GetTypeId(void) {
  static TypeId tid =
      TypeId("ns3::PfifoBoltQueueDisc")
          .SetParent<QueueDisc>()
          .SetGroupName("TrafficControl")
          .AddConstructor<PfifoBoltQueueDisc>()
          .AddAttribute(
              "MaxSize",
              "The maximum number of packets accepted by this queue disc.",
              QueueSizeValue(QueueSize("1000p")),//队列的最大长度
              MakeQueueSizeAccessor(&QueueDisc::SetMaxSize,
                                    &QueueDisc::GetMaxSize),
              MakeQueueSizeChecker())
          .AddAttribute(
              "CcThreshold", "Queue occupancy threshold for CC reaction.",
              QueueSizeValue(QueueSize("62KB")),
              MakeQueueSizeAccessor(&PfifoBoltQueueDisc::m_ccThreshold),
              MakeQueueSizeChecker())
          .AddAttribute("EnableAbs",
                        "Enable Available Bandwidth Signaling feature.",
                        BooleanValue(false),
                        MakeBooleanAccessor(&PfifoBoltQueueDisc::m_absEnabled),
                        MakeBooleanChecker())
          .AddAttribute(
              "MaxInstAvailLoad",
              "Maximum amount of bytes allowed to be accumulated to measure "
              "available bandwidth.",
              IntegerValue(30000),
              MakeIntegerAccessor(&PfifoBoltQueueDisc::m_maxInstAvailLoad),
              MakeIntegerChecker<int>())
          .AddAttribute(
              "EnableBts",
              "Enable Back To Sender feature for congestion signalling.",
              BooleanValue(false),
              MakeBooleanAccessor(&PfifoBoltQueueDisc::m_btsEnabled),
              MakeBooleanChecker())
          .AddAttribute("EnableTrimming",
                        "Enable payload trimming instead of dropTail policy.",
                        BooleanValue(false),
                        MakeBooleanAccessor(&PfifoBoltQueueDisc::m_trimEnabled),
                        MakeBooleanChecker())
          .AddAttribute(
              "EnablePru",
              "Enable Proactive Ramp-Up feature for higher link utilization.",
              BooleanValue(false),//是否开启PRU
              MakeBooleanAccessor(&PfifoBoltQueueDisc::m_pruEnabled),
              MakeBooleanChecker())
          .AddAttribute(
              "MaxNumPruTokens",
              "Maximum number of packet slots for the future",
              UintegerValue(2560),//PRU的最大数量
              MakeUintegerAccessor(&PfifoBoltQueueDisc::m_maxPruToken),
              MakeUintegerChecker<uint16_t>())
          .AddAttribute(
              "ReducedBtsFactor",
              "Period of BTS transmission during congestion", UintegerValue(1),
              MakeUintegerAccessor(&PfifoBoltQueueDisc::m_reducedBtsFactor),
              MakeUintegerChecker<uint32_t>())
          .AddTraceSource(
              "AbsTokensInQueue",
              "Amount of ABS tokens currently stored in the queue disc",
              MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_availLoad),
              "ns3::TracedValueCallback::Int")
          .AddTraceSource(
              "PruTokensInQueue",
              "Number of PRU tokens currently stored in the queue disc",
              MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_nPruToken),
              "ns3::TracedValueCallback::Uint16")
          .AddTraceSource(
              "BtsDeparture",
              "Trace source tracking the internal switch state every time it "
              "generates a new BTS packet.",
              MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_btsSendTrace),
              "ns3::PfifoBoltQueueDisc::BtsDepartureTracedCallback")
          .AddTraceSource(
            "totDeQueue",
            "跟踪从队列出队的总字节数",
            MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_totDeQueue),
            "ns3::TracedCallback::Uint32")
          .AddTraceSource(
            "ALLPru",
            "跟踪总的PRU数量(不计消耗)",
            MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_nALLPru),
            "ns3::TracedCallback::Uint32")
          .AddTraceSource(
              "PruProduce",
              "跟踪PRU令牌总生成数（每次生成时+1，对应S-Bolt令牌生成统计）",
              MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_pruProduce),
              "ns3::TracedCallback::Uint32")
          .AddTraceSource(
              "PruConsume",
              "跟踪PRU令牌总消耗请求数（每次incwin包尝试消耗时+1，对应S-Bolt令牌消耗统计）",
              MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_pruConsume),
              "ns3::TracedCallback::Uint32")
          // <<< 2. 注册H值与countofH Trace源（用于S-Bolt逻辑调试）>>>
          .AddTraceSource(
              "HValue",
              "跟踪动态消耗阈值H（H=pru_produce/pru_consume，控制S-Bolt令牌消耗节奏）",
              MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_H),
              "ns3::TracedCallback::Double")
          .AddTraceSource(
              "CountofH",
              "跟踪countofH计数器（记录自上次成功消耗后尝试消耗的incwin包数量）",
              MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_countofH),
              "ns3::TracedCallback::Uint32");
  tid = tid.AddTraceSource(
        "FlowThroughput",
        "Per-flow throughput in Mbps (once per second)",
        MakeTraceSourceAccessor(&PfifoBoltQueueDisc::m_flowThptTrace),
       "ns3::TracedCallback::tlargs<ns3::PfifoBoltQueueDisc::FlowKey,double>");
  return tid;
}

PfifoBoltQueueDisc::PfifoBoltQueueDisc()
    : QueueDisc(QueueDiscSizePolicy::MULTIPLE_QUEUES, QueueSizeUnit::PACKETS) {
  NS_LOG_FUNCTION(this);

  m_pruProduce = 0;
  m_pruConsume = 0;
  m_H = 1.0;
  m_countofH = 0;
  
  Simulator::Schedule(Seconds(0.0001), &PfifoBoltQueueDisc::MonitorPerFlowThpt, this);
}

PfifoBoltQueueDisc::~PfifoBoltQueueDisc() { NS_LOG_FUNCTION(this); }

const uint32_t PfifoBoltQueueDisc::prio2band[16] = {1, 2, 2, 2, 1, 2, 0, 0,
                                                    1, 1, 1, 1, 1, 1, 1, 1};

void PfifoBoltQueueDisc::CalculateCurrentlyAvailableBw(uint32_t pktSize) {
  NS_LOG_FUNCTION(this << pktSize);

  Ipv4Header ipv4h;  // Account for the IP header of the arriving packet as well
  // Arriving packet reduces the available bandwidth
  int newAvailLoad =
      m_availLoad - static_cast<int>(pktSize + ipv4h.GetSerializedSize() - 1);
  // TODO(serhatarslan): The -1 above is required for m_availLoad to closely
  //                     follow the current queue occupancy. Investigate why.

  // availLoad should be periodically increasing to account for draining queue
  uint64_t now = Simulator::Now().GetNanoSeconds();
  double secondsSinceLastUpdate =
      static_cast<double>(now - m_availLoadUpdateTime) / 1e9;
  newAvailLoad += static_cast<int>(
      static_cast<double>(m_boundNetDevice->GetDataRate().GetBitRate()) *
      secondsSinceLastUpdate / 8.0);
  m_availLoadUpdateTime = now;
  // Cap the available bandwidth measure to prevent large bursts
  if (newAvailLoad > m_maxInstAvailLoad) newAvailLoad = m_maxInstAvailLoad;
  m_availLoad = newAvailLoad;
}

void PfifoBoltQueueDisc::TrimPacketPayload(Ptr<Packet> p,
                                           BoltHeader *bolth) {
  NS_LOG_FUNCTION(this << p << bolth);

  p->RemoveHeader(*bolth);
  uint32_t payloadSize = p->GetSize();
  if (payloadSize > 0)
    {
      p->RemoveAtEnd(payloadSize);
    }

  bolth->SetFlags((bolth->GetFlags() & ~BoltHeader::Flags_t::DATA) |
                  BoltHeader::Flags_t::CHOP);
  p->AddHeader(*bolth);
}

void PfifoBoltQueueDisc::NotifySender(Ipv4Header ipv4h, const BoltHeader &bolth) {
  NS_LOG_FUNCTION(this << ipv4h << bolth);

  Ptr<Packet> p = Create<Packet>();

  BoltHeader newBolth;
  newBolth.SetSrcPort(bolth.GetDstPort());
  newBolth.SetDstPort(bolth.GetSrcPort());
  newBolth.SetSeqAckNo(bolth.GetSeqAckNo());
  uint16_t flag = (bolth.GetFlags() & ~BoltHeader::Flags_t::DATA) |
                  BoltHeader::Flags_t::BTS | BoltHeader::Flags_t::DECWIN;
  newBolth.SetFlags(flag);
  newBolth.SetTxMsgId(bolth.GetTxMsgId());
  newBolth.SetDrainTime(bolth.GetDrainTime());
  newBolth.SetReflectedDelay(bolth.GetReflectedDelay());
  // TODO(serhatarslan): Lookup the default TTL value instead of using 64
  newBolth.SetReflectedHopCnt(64 - ipv4h.GetTtl());
  p->AddHeader(newBolth);

  Ipv4Header newIpv4h;
  newIpv4h.SetSource(ipv4h.GetDestination());
  newIpv4h.SetDestination(ipv4h.GetSource());
  newIpv4h.SetProtocol(BoltL4Protocol::PROT_NUMBER);
  newIpv4h.SetDontFragment();
  newIpv4h.SetPayloadSize(p->GetSize());
  p->AddHeader(newIpv4h);

  Ptr<Ipv4L3Protocol> ipv4L3Protocol =
      m_boundNetDevice->GetNode()->GetObject<Ipv4L3Protocol>();

  Address dummy;
  ipv4L3Protocol->Receive(m_boundNetDevice, p, Ipv4L3Protocol::PROT_NUMBER,
                          dummy, dummy,
                          NetDevice::PacketType::PACKET_OTHERHOST);

  m_nBtsInFlight++;
  m_btsSendTrace(m_nBtsInFlight, GetNBytes());
}

void PfifoBoltQueueDisc::BtsWithArtificialDelay(Ipv4Header ipv4h,
                                                const BoltHeader &bolth) {
  NS_LOG_FUNCTION(this << ipv4h << bolth);

  Ptr<Packet> p = Create<Packet>();

  BoltHeader newBolth;
  newBolth.SetSrcPort(bolth.GetDstPort());
  newBolth.SetDstPort(bolth.GetSrcPort());
  newBolth.SetSeqAckNo(bolth.GetSeqAckNo() + 1454);
  uint16_t flag = (bolth.GetFlags() & ~BoltHeader::Flags_t::DATA) |
                  BoltHeader::Flags_t::BTS | BoltHeader::Flags_t::DECWIN;
  newBolth.SetFlags(flag);
  newBolth.SetTxMsgId(bolth.GetTxMsgId());
  newBolth.SetDrainTime(bolth.GetDrainTime());
  newBolth.SetReflectedDelay(bolth.GetReflectedDelay());
  // TODO(serhatarslan): Lookup the default TTL value instead of using 64
  newBolth.SetReflectedHopCnt(64 - ipv4h.GetTtl());
  p->AddHeader(newBolth);

  Ipv4Header newIpv4h;
  newIpv4h.SetSource(ipv4h.GetDestination());
  newIpv4h.SetDestination(ipv4h.GetSource());
  newIpv4h.SetProtocol(BoltL4Protocol::PROT_NUMBER);
  newIpv4h.SetDontFragment();
  newIpv4h.SetPayloadSize(p->GetSize());

  Time delay = NanoSeconds(0);
  // Time delay = NanoSeconds(bolth->GetDrainTime());
  // Time delay = NanoSeconds(1273);                          // Bts Clock
  // Time delay = NanoSeconds(bolth->GetDrainTime() + 1273); // Egress Bts Clock
  // Time delay = NanoSeconds(6593);             // RTT clock Time delay =
  // NanoSeconds(bolth->GetDrainTime() + 6593);  // Normal Ack Clock
  Simulator::Schedule(delay, &PfifoBoltQueueDisc::SendArtificialBts, this,
                      bolth.GetSrcPort(), p, newIpv4h);
}

void PfifoBoltQueueDisc::SendArtificialBts(uint16_t srcPort, Ptr<Packet> p,
                                           Ipv4Header newIpv4h) {
  Ptr<Node> senderNode = NodeList::GetNode(srcPort - 1000);
  Ptr<BoltL4Protocol> boltL4Protocol = senderNode->GetObject<BoltL4Protocol>();
  boltL4Protocol->Receive(p, newIpv4h, 0);
}

uint16_t PfifoBoltQueueDisc::SetBitRateFlag(uint16_t curFlag) {
  NS_LOG_FUNCTION(this << curFlag);

  // Reset all the bit rate flags
  uint16_t newFlag = curFlag & 0x07ff;

  uint64_t bitRate = m_boundNetDevice->GetDataRate().GetBitRate();
  switch (bitRate) {
    case 10000000000lu:
      newFlag |= BoltHeader::Flags_t::LINK10G;
      break;
    case 25000000000lu:
      newFlag |= BoltHeader::Flags_t::LINK25G;
      break;
    case 40000000000lu:
      newFlag |= BoltHeader::Flags_t::LINK40G;
      break;
    case 100000000000lu:
      newFlag |= BoltHeader::Flags_t::LINK100G;
      break;
    case 400000000000lu:
      newFlag |= BoltHeader::Flags_t::LINK400G;
      break;
    default:
      NS_ASSERT_MSG(
          false, "Error: Bitrate " << bitRate << " not recognized for Bolt.");
      break;
  }
  return newFlag;
}

bool PfifoBoltQueueDisc::DoEnqueue(Ptr<QueueDiscItem> item) {
  NS_LOG_FUNCTION(this << item);
  
  Ptr<Packet> p = item->GetPacket();
  NS_LOG_DEBUG("PfifoBoltQueueDisc (" << this
                                      << ") received: " << p->ToString());
                                   
  BoltHeader bolth;
  NS_ABORT_MSG_IF(p->PeekHeader(bolth) == 0,
                  "PfifoBoltQueueDisc expected a Bolt header");
  uint16_t srcPort = bolth.GetSrcPort ();
  uint16_t dstPort = bolth.GetDstPort ();

  /* ---------- 更新 RTT ---------- */
  /* ---------- 新 RTT + PRU 逻辑 ---------- */
  uint32_t idx = Idx(srcPort, dstPort);
  FlowState &st = m_flowTbl[idx];
  /* ---------- 如果流结束，清零该流 RTT ---------- */
  /*
  uint16_t flags = bolt.GetFlags();
  if ((flags & BoltHeader::Flags_t::FIN) &&(flags & BoltHeader::Flags_t::LAST) )
  {
    m_flowSrtt.erase (fk);               // 删除该流的 EWMA 记录
    // 重新计算全局最小 RTT
    m_minSrtt = 0.0;
    for (auto &kv : m_flowSrtt)
      if (m_minSrtt == 0.0 || kv.second < m_minSrtt)
        m_minSrtt = kv.second;
  }
        */
  


  

  uint8_t priority = 0;
  SocketPriorityTag priorityTag;
  if (p->PeekPacketTag(priorityTag)) priority = priorityTag.GetPriority();
  uint32_t band = prio2band[priority & 0x0f];

  CalculateCurrentlyAvailableBw(p->GetSize());

  uint16_t boltFlag = bolth.GetFlags();
  //更新RTT
  
  UpdateSrtt(idx, bolth.GetSrttNs());
  bool headerModified = false;
  bool headerPersisted = false;

  if (boltFlag & BoltHeader::Flags_t::LAST) {
    // std::ofstream logFile("last_packets_received.log", std::ios::app);
    // logFile << Simulator::Now().GetNanoSeconds()
    //        << " Queue:" << this
    //        << " Interface:" << m_boundNetDevice->GetIfIndex()
    //        << " SeqNo:" << bolth->GetSeqAckNo()
    //        << " TxMsgId:" << bolth->GetTxMsgId()
    //        << " SrcPort:" << bolth->GetSrcPort()
    //        << " DstPort:" << bolth->GetDstPort()
    //        << " QueueSize:" << GetNBytes()
    //        << " LAST_COUNT:" << lastReceivedCount
    //        << " Flags:" << BoltHeader::FlagsToString(boltFlag)
    //        << std::endl;
    // logFile.close();
  }


  uint16_t highPriorityFlags =
      BoltHeader::Flags_t::ACK | BoltHeader::Flags_t::NACK |
      BoltHeader::Flags_t::CHOP | BoltHeader::Flags_t::BTS;
  if (boltFlag & highPriorityFlags) {
    band = 0;
  } else {
    NS_ASSERT(boltFlag & BoltHeader::Flags_t::DATA);

    Ipv4QueueDiscItem *ipv4Item = dynamic_cast<Ipv4QueueDiscItem *>(&(*(item)));
    Ipv4Header ipv4h = ipv4Item->GetHeader();
    NS_ASSERT(ipv4h.GetProtocol() == BoltL4Protocol::PROT_NUMBER);

    // Perform congestion control
    uint32_t curNBytes = GetNBytes();
    if (curNBytes >= m_ccThreshold.GetValue()) {
      uint32_t curDrainTime = m_boundNetDevice->GetDataRate()
                                  .CalculateBytesTxTime(curNBytes)
                                  .GetNanoSeconds();
      // Make sure noone else increases cwnd of this flow
      uint16_t flags = boltFlag & ~BoltHeader::Flags_t::INCWIN;
      if (m_btsEnabled) flags |= BoltHeader::Flags_t::DECWIN;

      if (!(boltFlag & BoltHeader::Flags_t::DECWIN)) {
        // if (curDrainTime > bolth->GetDrainTime()) {
        bolth.SetDrainTime(curDrainTime);
        flags = SetBitRateFlag(flags);
        bolth.SetFlags(flags);
        headerModified = true;

        if (m_btsEnabled && (m_nBtsInFlight % m_reducedBtsFactor == 0)) {
          NotifySender(ipv4h, bolth);
          // BtsWithArtificialDelay(ipv4h, bolth);
        } else {
          flags |= BoltHeader::Flags_t::DECWIN;
          bolth.SetFlags(flags);
          headerModified = true;
        }
      } else {
        if (curDrainTime > bolth.GetDrainTime()) {
          bolth.SetDrainTime(curDrainTime);
          flags = SetBitRateFlag(flags);
        }
        bolth.SetFlags(flags);
        headerModified = true;
      }

      if (GetCurrentSize() >= GetMaxSize()) {
        // The buffer is actually full. Trim payload and forward the header to
        // enable quick detection of packet loss.
        if (m_trimEnabled) {
          NS_LOG_LOGIC("Queue disc limit exceeded -- trimming packet");
          TrimPacketPayload(p, &bolth);
          band = 0;
          headerPersisted = true;
        } else {
          NS_LOG_LOGIC("Queue disc limit exceeded -- dropping packet");
          DropBeforeEnqueue(item, LIMIT_EXCEEDED_DROP);
          return false;
        }
      }
      // 标签,拥塞的时候是否作废pru标签,1表示不作废,0表示作废
      // if (0)
      // if ((m_pruEnabled && (boltFlag & BoltHeader::Flags_t::LAST))){
      //   if (BoltHeader::Flags_t::LAST) {
      //     m_nALLPru++;
      //   }
      //   if (!(boltFlag & BoltHeader::Flags_t::FIRST)) {
      //     if (m_nPruToken < m_maxPruToken) {
      //       m_nPruToken++;
            
      //     } else {
      //       m_nPruToken = m_maxPruToken;
      //     }
      //   }
      // }


    } else if ((m_pruEnabled && (boltFlag & BoltHeader::Flags_t::LAST))){
      if (!(boltFlag & BoltHeader::Flags_t::FIRST)) {
        //the part of 'if'and'else' is added for sbolt(inhance)
        if (!m_useEnhancedPru){
          m_nPruToken += st.weight;
          m_nALLPru ++;
        }else{
          m_pruProduce += 1;
          m_nPruToken += st.weight;
          m_nALLPru ++;
        }
        // m_nPruToken += st.weight;
        // m_nALLPru ++;
        //std::cout << "debug0:" << st.weight << std::endl;
        //std::cout << "debug1pru:" << m_nPruToken << std::endl;
      }
    } else if (m_pruEnabled && (boltFlag & BoltHeader::Flags_t::INCWIN) && m_nPruToken >= st.weight && m_nALLPru > 0) {
      uint32_t use = st.weight;
      //std::cout << "debug2" << use << std::endl;
      if(!(boltFlag & BoltHeader::Flags_t::FIRST)){
        //the part of 'if'and'else' is added for sblot(inhance)
        if (!m_useEnhancedPru){
          m_nPruToken -= use;
          m_nALLPru --;
        }else{
          m_pruConsume += 1;
          m_countofH++;
          bool allowConsume = (m_countofH >= (1<<m_H));
          if (allowConsume){
            m_nPruToken -= use;
            m_nALLPru --;
            m_H = 0;
            while (m_pruConsume << (m_H + 1) <= m_pruProduce)
            {
              m_H ++;
            }
            m_countofH = 0;
          }else{
            bolth.SetFlags(boltFlag & ~BoltHeader::Flags_t::INCWIN);
            headerModified = true;
          }
        }
        // m_nPruToken -= use;
        // m_nALLPru --;
      }
      // if(m_nPruToken <= st.weight) m_nPruToken = 0;
} else if (m_absEnabled && m_availLoad >= m_boundNetDevice->GetMtu() &&
               boltFlag & BoltHeader::Flags_t::INCWIN) {
      // There is available bandwidth
      m_availLoad -= m_boundNetDevice->GetMtu();
    } else {
      if (ipv4h.GetTtl() < 64 || m_absEnabled) {
        // If the sender itself never becomes bottleneck, it tends to delete the
        // INC flag on the packet and prevent PRU to take affect at the 
        // bottleneck. When ABS is enabled, this is not a problem because it 
        // makes the packet keep its INC flag anyway.
        bolth.SetFlags(boltFlag & ~BoltHeader::Flags_t::INCWIN);
        headerModified = true;
      }
    }
  }

  if (headerModified && !headerPersisted)
    {
      BoltHeader discarded;
      p->RemoveHeader(discarded);
      p->AddHeader(bolth);
    }

  bool retval = GetInternalQueue(band)->Enqueue(item);

  // If Queue::Enqueue fails, QueueDisc::DropBeforeEnqueue is called by the
  // internal queue because QueueDisc::AddInternalQueue sets the trace callback

  if (!retval)
    NS_LOG_WARN("Packet enqueue failed. Check the size of internal queues");

  NS_LOG_LOGIC("Number of packets in band "
               << band << ": " << GetInternalQueue(band)->GetNPackets());

  return retval;
}

Ptr<QueueDiscItem> PfifoBoltQueueDisc::DoDequeue(void) {
  NS_LOG_FUNCTION(this);

  Ptr<QueueDiscItem> item;

  for (uint32_t i = 0; i < GetNInternalQueues(); i++) {
    if ((item = GetInternalQueue(i)->Dequeue()) != 0) {
      NS_LOG_LOGIC("Popped from band " << i << ": " << item);
      NS_LOG_LOGIC("Number of packets in band "
                   << i << ": " << GetInternalQueue(i)->GetNPackets());
      
      // 触发吞吐量跟踪 - 当数据包出队时记录字节数
      if (item->GetPacket()) {
        m_totDeQueue(item->GetPacket()->GetSize());
        BoltHeader bolt;
        item->GetPacket()->PeekHeader(bolt);
        FlowKey fk(bolt.GetSrcPort(), bolt.GetDstPort());
        m_totalBytes[fk] += item->GetPacket()->GetSize();
      }
      
      return item;
    }
  }

  NS_LOG_LOGIC("Queue empty");
  return item;
}

Ptr<const QueueDiscItem> PfifoBoltQueueDisc::DoPeek(void) {
  NS_LOG_FUNCTION(this);

  Ptr<const QueueDiscItem> item;

  for (uint32_t i = 0; i < GetNInternalQueues(); i++) {
    if ((item = GetInternalQueue(i)->Peek()) != 0) {
      NS_LOG_LOGIC("Peeked from band " << i << ": " << item);
      NS_LOG_LOGIC("Number of packets in band "
                   << i << ": " << GetInternalQueue(i)->GetNPackets());
      return item;
    }
  }

  NS_LOG_LOGIC("Queue empty");
  return item;
}

bool PfifoBoltQueueDisc::CheckConfig(void) {
  NS_LOG_FUNCTION(this);
  if (GetNQueueDiscClasses() > 0) {
    NS_LOG_ERROR("PfifoBoltQueueDisc cannot have classes");
    return false;
  }

  if (GetNPacketFilters() != 0) {
    NS_LOG_ERROR("PfifoBoltQueueDisc needs no packet filter");
    return false;
  }

  if (GetNInternalQueues() == 0) {
    // create 3 DropTail queues with GetLimit() packets each
    ObjectFactory factory;
    factory.SetTypeId("ns3::DropTailQueue<QueueDiscItem>");
    factory.Set("MaxSize", QueueSizeValue(GetMaxSize()));
    AddInternalQueue(factory.Create<InternalQueue>());
    AddInternalQueue(factory.Create<InternalQueue>());
    AddInternalQueue(factory.Create<InternalQueue>());
  }

  if (GetNInternalQueues() != 3) {
    NS_LOG_ERROR("PfifoBoltQueueDisc needs 3 internal queues");
    return false;
  }

  if (GetInternalQueue(0)->GetMaxSize().GetUnit() != QueueSizeUnit::PACKETS ||
      GetInternalQueue(1)->GetMaxSize().GetUnit() != QueueSizeUnit::PACKETS ||
      GetInternalQueue(2)->GetMaxSize().GetUnit() != QueueSizeUnit::PACKETS) {
    NS_LOG_ERROR("PfifoBoltQueueDisc needs 3 internal queues "
                 << "operating in packet mode");
    return false;
  }

  for (uint8_t i = 0; i < 2; i++) {
    if (GetInternalQueue(i)->GetMaxSize() < GetMaxSize()) {
      NS_LOG_ERROR("The capacity of some internal queue(s) is "
                   << "less than the queue disc capacity");
      return false;
    }
  }

  if (m_ccThreshold.GetUnit() != QueueSizeUnit::BYTES) {
    NS_LOG_ERROR(
        "The congestion control threshold should be provided in Bytes!");
    return false;
  }

  if (m_ccThreshold.GetValue() > GetMaxSize().GetValue() * 1500) {
    // TODO(serhatarslan): Find a generic method to compute the default MTU
    NS_LOG_ERROR("The congestion control threshold is not smaller than "
                 << "the total size of the queue disc!");
    return false;
  }

  return true;
}

void PfifoBoltQueueDisc::InitializeParams(void) {
  NS_LOG_FUNCTION(this);

  m_availLoad = 0;
  m_availLoadUpdateTime = Simulator::Now().GetNanoSeconds();

  Ptr<NetDeviceQueueInterface> ndqi = GetNetDeviceQueueInterface();
  NS_ABORT_MSG_IF(ndqi == 0, "PfifoBoltQueueDisc requires a NetDeviceQueueInterface");
  m_boundNetDevice = ndqi->GetObject<PointToPointNetDevice>();
  NS_ABORT_MSG_IF(m_boundNetDevice == 0,
                  "PfifoBoltQueueDisc currently supports PointToPointNetDevice only");

  m_nPruToken = 0;
  m_nALLPru = 0;
  m_nBtsInFlight = 0;
}
void
PfifoBoltQueueDisc::MonitorPerFlowThpt ()
{
  Time now = Simulator::Now ();
  double t = now.GetSeconds ();

  for (auto &kv : m_totalBytes)
    {
      const FlowKey &fk = kv.first;
      uint64_t tot = kv.second;

      // 上次记录
      Time      &lt = m_lastTime[fk];
      uint64_t  &lb = m_lastBytes[fk];

      if (lt > Seconds(0.0))
        {
          double dt = t - lt.GetSeconds ();
          uint64_t db = tot - lb;
          double mbps = (double)(db * 8) / dt / 1e6;  // 转成 Mbps

          // 抛出 Trace 事件
          m_flowThptTrace(fk, mbps);
        }

      // 更新基准
      lt = now;
      lb = tot;
    }

  // 下次调用
  Simulator::Schedule(Seconds(0.0001), &PfifoBoltQueueDisc::MonitorPerFlowThpt, this);
}

}  // namespace ns3
