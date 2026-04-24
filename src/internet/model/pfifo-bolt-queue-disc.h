/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2021 Google LLC
 *
 * All rights reserved.
 *
 * Author: Serhat Arslan <serhatarslan@google.com>
 */

#ifndef PFIFO_BOLT_H
#define PFIFO_BOLT_H

#include "ns3/bolt-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/queue-disc.h"
#include <unordered_map>
#include <functional>
namespace ns3 {

/**
 * \ingroup traffic-control
 *
 * The pfifo_bolt is basically the prototype model of the switch program for
 * the BoltL4Protocol. It mainly mimics pfifo_fast of linux, but adds some
 * packet processing logic such as congestion signalling and return to sender
 * capabilities.
 *
 */
class PfifoBoltQueueDisc : public QueueDisc {
 public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId(void);
  /**
   * \brief PfifoBoltQueueDisc constructor
   *
   * Creates a queue with a depth of 1000 packets per band by default
   */
  PfifoBoltQueueDisc();

  virtual ~PfifoBoltQueueDisc();

  /**
   * \brief Computes the currently available bandwidth to measure congestion
   * \param pktSize The size of the arriving packet
   */
  void CalculateCurrentlyAvailableBw(uint32_t pktSize);

  /**
   * \brief Trims the given packet and updates the header accordingly
   * \param p The original data packet to be trimmed
   * \param boltHeader The Bolt header of the packet for update
   */
  void TrimPacketPayload(Ptr<Packet> p, BoltHeader *boltHeader);

  /**
   * \brief Creates a Back To Sender packet and sends it
   * \param ipv4h IPv4 header of the original packet
   * \param bolth Bolt header of the original packet
   */
  void NotifySender(Ipv4Header ipv4h, const BoltHeader &bolth);

  /**
   * \brief Creates an artificial BTS and schedules to send with a certain delay
   * \param ipv4h IPv4 header of the original packet
   * \param bolth Bolt header of the original packet
   */
  void BtsWithArtificialDelay(Ipv4Header ipv4h, const BoltHeader &bolth);
  /**
   * \brief Sends the artificial BTS directly to the sender
   * \param srcPort Used to determine the sender
   * \param p The packet to be sent to the sender
   * \param ipv4h IPv4 header of the original packet
   */
  void SendArtificialBts(uint16_t srcPort, Ptr<Packet> p, Ipv4Header newIpv4h);

  /**
   * \brief Updates the Fastest Flow (ff) state for this qdisc
   * \param srcAddr The source IPv4 address of the new ff
   * \param dstAddr The destination IPv4 address of the new ff
   * \param srcPort The source port of the new ff
   * \param dstPort The destination port of the new ff
   * \param flowRate The current transmission rate of the new ff
   */
  void UpdateFfState(Ipv4Address srcAddr, Ipv4Address dstAddr, uint16_t srcPort,
                     uint16_t dstPort, uint32_t flowRate);

  /**
   * \brief Resets any bitrate flag and marks only the bitrate of boundNetDevice
   * \param curFlag The current set of flag values on the packet
   * \return The new set of flag values to be marked on the packet
   */
  uint16_t SetBitRateFlag(uint16_t curFlag);

  // Reasons for dropping packets
  static constexpr const char *LIMIT_EXCEEDED_DROP =
      "Queue disc limit exceeded";  //!< Packet dropped due to queue
                                    //!< overflow

  


  /**
   * Priority to band map. Values are taken from the prio2band array used by
   * the Linux pfifo_fast queue disc.
   */
  static const uint32_t prio2band[16];

  virtual bool DoEnqueue(Ptr<QueueDiscItem> item);
  virtual Ptr<QueueDiscItem> DoDequeue(void);
  virtual Ptr<const QueueDiscItem> DoPeek(void);
  virtual bool CheckConfig(void);
  virtual void InitializeParams(void);

  Ptr<PointToPointNetDevice>
      m_boundNetDevice;  // The net device which this queue disc is connected to

  QueueSize m_ccThreshold;  //!< Queue occupancy threshold for CC reaction
  int m_maxInstAvailLoad;   //!< Maximum amount of bytes allowed to be
                            //!< accumulated to measure available bandwidth
  TracedValue<int>
      m_availLoad;  //!< Currently available bandwidth in bytes. Congested if
                    //!< negative. Periodically increases & decreased on pkt RX
  uint64_t m_availLoadUpdateTime;  //!< Last time availLoad was updated (in ns)
  bool m_absEnabled;   //!< Flag to denote Available Bandwidth Signaling enabled
  bool m_btsEnabled;   //!< Flag to denote wheter Back To Sender is enabled
  bool m_trimEnabled;  //!< Flag to denote wheter payload trimming is enabled
  bool m_pruEnabled;   //!< Flag to enable Proactive Ramp-Up
  bool m_useEnhancedPru = true; //open the enhace sbolt--------false---------true
  TracedValue<double> m_nPruToken;  //!< Number of available packet slots in
                                      //!< the future
  TracedValue<uint16_t> m_nALLPru; // pal use it debug
  TracedValue<uint32_t> m_pruProduce; //the count of produce pru
  TracedValue<uint32_t> m_pruConsume; //the count of consume pru
  TracedValue<uint32_t> m_H;  
  TracedValue<uint32_t> m_countofH; 
  uint16_t m_maxPruToken;  //!< Maximum number of packet slots for the future
  TracedCallback<uint32_t> m_totDeQueue;
  TracedCallback<uint32_t> m_pruProduceTrace;
  TracedCallback<uint32_t> m_pruConsumeTrace;
  TracedCallback<uint32_t> m_HTrace; 
  TracedCallback<uint32_t> m_countofHTrace;
  uint32_t m_nBtsInFlight;  //!< Number of BTS packets that are still in flight
  uint32_t m_reducedBtsFactor;  //!< Period of BTS transmission during
                                //!< congestion, ie. once every x data packets
  TracedCallback<uint32_t, uint32_t> m_btsSendTrace;  //!< Trace of nBtsInFlight
                                                      //!< and queue occupancy
                                                      //!< at every BTS event



  
  struct FlowKey
  {
    uint16_t pLow;   // 较小的端口
    uint16_t pHigh;  // 较大的端口

    // 构造时自动排序
    FlowKey (uint16_t a, uint16_t b)
    {
      if (a < b) { pLow = a; pHigh = b; }
      else       { pLow = b; pHigh = a; }
    }
    bool operator==(const FlowKey& o) const
    { return pLow == o.pLow && pHigh == o.pHigh; }
  };

  struct FlowKeyHash
  {
    size_t operator()(const FlowKey& k) const noexcept
    { return (size_t(k.pLow) << 16) ^ k.pHigh; }
  };

  // 类成员
  /*
  std::unordered_map<FlowKey,double,FlowKeyHash> m_flowSrtt;  //!< 每条流 EWMA RTT（秒）
  std::unordered_map<FlowKey,double,FlowKeyHash> m_flowPRU;
  double m_minSrtt {0.0}; 
  double eps =  2.2204460492503131e-16;
  */  
    /* ---------- 新：定点状态 --------- */
  struct FlowState {
    uint32_t srtt_ns = 0;   // 最新 RTT
    double weight = 1.0;
  };
  static constexpr uint32_t kFlowTableSize = 1024;         // 可按需放大为 2^N
  FlowState m_flowTbl[kFlowTableSize];                      // <— 直接数组                              // 全局 min(1/rtt)
  /* ---------- 新增：工具函数声明 ---------- */
  inline uint32_t Idx(uint16_t a, uint16_t b) const noexcept;
  inline void     UpdateSrtt(uint32_t idx, uint32_t sample_ns);
  

  typedef TracedCallback<FlowKey, double> FlowThptTracedCallback;
  FlowThptTracedCallback m_flowThptTrace;
  std::unordered_map<FlowKey,uint64_t,FlowKeyHash> m_totalBytes;
  std::unordered_map<FlowKey,uint64_t,FlowKeyHash> m_lastBytes;
  std::unordered_map<FlowKey,Time,     FlowKeyHash> m_lastTime;
  void MonitorPerFlowThpt ();  // 周期性调度
};


}  // namespace ns3

#endif /* PFIFO_BOLT_H */
