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

#ifndef HOMA_L4_PROTOCOL_H
#define HOMA_L4_PROTOCOL_H

#include <stdint.h>
#include <deque>
#include <functional>
#include <queue>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "ns3/ptr.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/traced-callback.h"
#include "ns3/node.h"
#include "ns3/data-rate.h"
#include "ip-l4-protocol.h"
#include "ns3/homa-header.h"

namespace ns3 {

class Node;
class Socket;
class Ipv4EndPointDemux;
class Ipv4EndPoint;
class HomaSocket;
class NetDevice;
class HomaSendScheduler;
class HomaOutboundMsg;
class HomaRecvScheduler;
class HomaInboundMsg;
    
/**
 * \ingroup internet
 * \defgroup homa HOMA
 *
 * This  is  an  implementation of the Homa Transport Protocol described in [1].
 * It implements a connectionless, reliable, low latency message delivery
 * service. 
 *
 * [1] Behnam Montazeri, Yilong Li, Mohammad Alizadeh, and John Ousterhout. 2018. 
 * Homa: a receiver-driven low-latency transport protocol using network 
 * priorities. In <i>Proceedings of the 2018 Conference of the ACM Special Interest 
 * Group on Data Communication</i> (<i>SIGCOMM '18</i>). Association for Computing 
 * Machinery, New York, NY, USA, 221–235. 
 * DOI:https://doi-org.stanford.idm.oclc.org/10.1145/3230543.3230564
 *
 * This implementation is created in guidance of the protocol creators and 
 * maintained as the official ns-3 implementation of the protocol. The IPv6
 * compatibility of the protocol is left for future work.
 */
    
/**
 * \ingroup homa
 * \brief Implementation of the Homa Transport Protocol
 */
class HomaL4Protocol : public IpL4Protocol {
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);
  static const uint8_t PROT_NUMBER; //!< Protocol number of HOMA to be used in IP packets
    
  HomaL4Protocol ();
  virtual ~HomaL4Protocol ();

  /**
   * Set node associated with this stack.
   * \param node The corresponding node.
   */
  void SetNode (Ptr<Node> node);
  /**
   * \brief Get the node associated with this stack.
   * \return The corresponding node.
   */
  Ptr<Node> GetNode(void) const;
    
  /**
   * \brief Get the MTU of the associated net device
   * \return The corresponding MTU size in bytes.
   */
  uint32_t GetMtu(void) const;
    
  /**
   * \brief Get the approximated BDP value of the network in packets
   * \return The number of packets required for full utilization, ie. BDP.
   */
  uint16_t GetBdp(void) const;

  /**
   * \brief Get serialization time of one full-sized data packet on this link.
   * \return transmission time including PPP framing
   */
  Time GetFullDataPktTxTime (void) const;

  /**
   * \brief Get accumulated scheduled credit at the sender in packets.
   * \return total unconsumed scheduled credit across outbound messages
   */
  uint32_t GetAccumulatedSenderCreditPkts (void) const;
    
  /**
   * \brief Get the protocol number associated with Homa Transport.
   * \return The protocol identifier of Homa used in IP headers.
   */
  virtual int GetProtocolNumber (void) const;
    
  /**
   * \brief Get rtx timeout duration for inbound messages
   * \return Time value to determine the retransmission timeout of InboundMsgs
   */
  Time GetInboundRtxTimeout(void) const;
    
  /**
   * \brief Get rtx timeout duration for outbound messages
   * \return Time value to determine the retransmission timeout of OutboundMsgs
   */
  Time GetOutboundRtxTimeout(void) const;
    
  /**
   * \brief Get the maximum number of rtx timeouts allowed per message
   * \return Maximum allowed rtx timeout count per message
   */
  uint16_t GetMaxNumRtxPerMsg(void) const;
    
  /**
   * \brief Get total number of priority levels in the network
   * \return Total number of priority levels used within the network
   */
  uint8_t GetNumTotalPrioBands (void) const;
    
  /**
   * \brief Get number of priority levels dedicated to unscheduled packets in the network
   * \return Number of priority bands dedicated for unscheduled packets
   */
  uint8_t GetNumUnschedPrioBands (void) const;
  
  /**
   * \brief Get the configured number of messages to grant at the same time
   * \return Minimum number of messages to Grant at the same time
   */
  uint8_t GetOvercommitLevel (void) const;

  /**
   * \brief Whether receiver scheduling is FIFO/SRR-like instead of SRPT.
   * \return true when SRR-like ordering is enabled
   */
  bool UseSrrScheduling (void) const;

  /**
   * \brief Whether SIRD-compatible control path is enabled.
   * \return true if SIRD path is enabled
   */
  bool IsSirdEnabled (void) const;

  /**
   * \brief Baseline grant budget in packets for SIRD receiver logic.
   * \return grant budget in packets
   */
  uint16_t GetSirdCreditBudgetPkts (void) const;

  /**
   * \brief Unscheduled threshold in packets for line-rate startup.
   * \return threshold in packets
   */
  uint16_t GetSirdUnschThresholdPkts (void) const;

  /**
   * \brief Multiplicative decrease factor for ECN loop.
   * \return ECN multiplicative decrease factor
   */
  double GetSirdEcnMdFactor (void) const;

  /**
   * \brief Additive increase amount for ECN loop.
   * \return ECN additive increase amount in packets
   */
  double GetSirdEcnAiStep (void) const;

  /**
   * \brief Multiplicative decrease factor for sender congestion loop.
   * \return sender feedback multiplicative decrease factor
   */
  double GetSirdSenderMdFactor (void) const;

  /**
   * \brief Additive increase amount for sender congestion loop.
   * \return sender feedback additive increase amount in packets
   */
  double GetSirdSenderAiStep (void) const;

  /**
   * \brief EWMA gain for ECN mark ratio estimation.
   * \return EWMA gain in [0,1]
   */
  double GetSirdEcnAlphaGain (void) const;

  /**
   * \brief Sender backlog threshold used to mark csn.
   * \return queue drain time threshold
   */
  uint16_t GetSirdSenderCsnThreshold (void) const;

  /**
   * \brief Delay between receiving scheduled credit and making it sendable.
   * \return sender-side credit launch delay
   */
  Time GetSirdSenderCreditLaunchDelay (void) const;

  /**
   * \brief Emit a SIRD grant-decision trace record.
   * \param sender Source sender address
   * \param txMsgId Message ID on sender side
   * \param grantOffset Granted packet offset
   * \param senderBudgetPkts Sender budget after control update
   * \param ecnEwma ECN CE-ratio EWMA
   * \param senderCsn Sender congestion feedback bit
   */
  void TraceSirdGrantDecision (Ipv4Address sender,
                               uint16_t txMsgId,
                               uint16_t grantOffset,
                               double senderBudgetPkts,
                               double ecnEwma,
                               bool senderCsn);

  /**
   * \brief Emit a SIRD bucket-state trace record.
   * \param receiver Receiver address owning the bucket state
   * \param sender Source sender address
   * \param senderBudgetHostPkts Sender host-side bucket budget
   * \param senderCreditsInUsePkts Sender outstanding credits in use
   * \param globalCreditsInUsePkts Global outstanding credits in use
   * \param globalBudgetPkts Global credit budget
   * \param eventType 1=grant-issued, 2=credit-reclaimed, 0=no-grant
   */
  void TraceSirdBucketState (Ipv4Address receiver,
                             Ipv4Address sender,
                             double senderBudgetHostPkts,
                             uint32_t senderCreditsInUsePkts,
                             uint32_t globalCreditsInUsePkts,
                             uint32_t globalBudgetPkts,
                             uint8_t eventType);

  /**
   * \brief Emit a per-packet SIRD state trace record.
   * \param receiver Receiver address handling this packet
   * \param sender Sender address of this packet
   * \param flags Homa flags field
   * \param msgPktState Packed state: high 16 bits=txMsgId, low 16 bits=pktOffset
   * \param grantOffset Grant offset field
   * \param ecn IPv4 ECN field (0..3)
   * \param csn Whether CSN feedback bit is set
   * \param creditState Packed credits: low 16 bits=senderInUse, high 16 bits=globalInUse
   */
  void TraceSirdPacketState (Ipv4Address receiver,
                             Ipv4Address sender,
                             uint8_t flags,
                             uint32_t msgPktState,
                             uint16_t grantOffset,
                             uint8_t ecn,
                             bool csn,
                             uint32_t creditState);

  /**
   * \brief Emit a per-sender SIRD control-loop state trace record.
   * \param receiver Receiver address owning the control state
   * \param sender Sender address whose state is sampled
   * \param netBudgetPkts Network-side sender budget after CE loop update
   * \param hostBudgetPkts Host-side sender budget after CSN loop update
   * \param effectiveBudgetPkts Effective sender budget used for grant gating
   * \param ceEwma Receiver-wide CE EWMA value
   * \param loopState Packed loop state:
   *   bit0=senderCe, bit1=senderCsn, bits[2..3]=eventType,
   *   bits[8..23]=senderCreditsInUsePkts, bits[24..39]=globalCreditsInUsePkts,
   *   bits[40..55]=globalBudgetPkts
   * \param counterState Packed counters:
   *   bits[0..20]=ceMarksObserved, bits[21..41]=csnMarksObserved,
   *   bits[42..62]=dataPktsObserved
   */
  void TraceSirdLoopState (Ipv4Address receiver,
                           Ipv4Address sender,
                           double netBudgetPkts,
                           double hostBudgetPkts,
                           double effectiveBudgetPkts,
                           double ceEwma,
                           uint64_t loopState,
                           uint64_t counterState);

  /**
   * \brief Emit sender-side scheduled credit currently held by a sender.
   * \param sender Sender address that owns the credit
   * \param receiver Receiver address that issued the credit
   * \param txMsgId Message id the credit applies to
   * \param senderCreditPkts Unconsumed scheduled credit in packets
   * \param eventType 1=grant received, 2=resend credit, 3=data sent, 4=message cleared
   */
  void TraceSirdSenderCreditState (Ipv4Address sender,
                                   Ipv4Address receiver,
                                   uint16_t txMsgId,
                                   uint32_t senderCreditPkts,
                                   uint8_t eventType);

  /**
   * \brief Emit receiver-side available credit after a bucket update.
   * \param receiver Receiver address owning the bucket
   * \param sender Sender address associated with this update
   * \param receiverAvailPkts Remaining global receiver budget
   * \param receiverBudgetPkts Total global receiver budget
   * \param senderAvailPkts Remaining per-sender budget
   * \param senderBudgetPkts Total per-sender budget
   * \param eventType 1=grant issued, 2=credit reclaimed, 0=no grant
   */
  void TraceSirdReceiverCreditState (Ipv4Address receiver,
                                     Ipv4Address sender,
                                     uint32_t receiverAvailPkts,
                                     uint32_t receiverBudgetPkts,
                                     uint32_t senderAvailPkts,
                                     uint32_t senderBudgetPkts,
                                     uint8_t eventType);
    
  /**
   * \brief Return whether outbound/inbound messages store only packet sizes.
   * \return true when the memory-saving representation is enabled
   */
  bool UsesOptimizedMemory (void);
    
  /**
   * \brief Create a HomaSocket and associate it with this Homa Protocol instance.
   * \return A smart Socket pointer to a HomaSocket, allocated by the HOMA Protocol.
   */
  Ptr<Socket> CreateSocket (void);
    
  /**
   * \brief Allocate an IPv4 Endpoint
   * \return the Endpoint
   */
  Ipv4EndPoint *Allocate (void);
  /**
   * \brief Allocate an IPv4 Endpoint
   * \param address address to use
   * \return the Endpoint
   */
  Ipv4EndPoint *Allocate (Ipv4Address address);
  /**
   * \brief Allocate an IPv4 Endpoint
   * \param boundNetDevice Bound NetDevice (if any)
   * \param port port to use
   * \return the Endpoint
   */
  Ipv4EndPoint *Allocate (Ptr<NetDevice> boundNetDevice, uint16_t port);
  /**
   * \brief Allocate an IPv4 Endpoint
   * \param boundNetDevice Bound NetDevice (if any)
   * \param address address to use
   * \param port port to use
   * \return the Endpoint
   */
  Ipv4EndPoint *Allocate (Ptr<NetDevice> boundNetDevice, Ipv4Address address, uint16_t port);
  /**
   * \brief Allocate an IPv4 Endpoint
   * \param boundNetDevice Bound NetDevice (if any)
   * \param localAddress local address to use
   * \param localPort local port to use
   * \param peerAddress remote address to use
   * \param peerPort remote port to use
   * \return the Endpoint
   */
  Ipv4EndPoint *Allocate (Ptr<NetDevice> boundNetDevice,
                          Ipv4Address localAddress, uint16_t localPort,
                          Ipv4Address peerAddress, uint16_t peerPort);
    
  /**
   * \brief Remove an IPv4 Endpoint.
   * \param endPoint the end point to remove
   */
  void DeAllocate (Ipv4EndPoint *endPoint);
    
  // called by HomaSocket.
  /**
   * \brief Send a message via Homa Transport Protocol (IPv4)
   * \param message The message to send
   * \param saddr The source Ipv4Address
   * \param daddr The destination Ipv4Address
   * \param sport The source port number
   * \param dport The destination port number
   */
  void Send (Ptr<Packet> message,
             Ipv4Address saddr, Ipv4Address daddr, 
             uint16_t sport, uint16_t dport);
  /**
   * \brief Send a message via Homa Transport Protocol (IPv4)
   * \param message The message to send
   * \param saddr The source Ipv4Address
   * \param daddr The destination Ipv4Address
   * \param sport The source port number
   * \param dport The destination port number
   * \param route The route requested by the sender
   */
  void Send (Ptr<Packet> message,
             Ipv4Address saddr, Ipv4Address daddr, 
             uint16_t sport, uint16_t dport, Ptr<Ipv4Route> route);
  
  // called by HomaSendScheduler or HomaRecvScheduler.
  /**
   * \brief Send the selected packet down to the IP Layer
   * \param packet The packet to send
   * \param saddr The source Ipv4Address
   * \param daddr The destination Ipv4Address
   * \param route The route requested by the sender
   */ 
  void SendDown (Ptr<Packet> packet, 
                 Ipv4Address saddr, Ipv4Address daddr, 
                 Ptr<Ipv4Route> route=0);
  
  /**
   * \brief Calculate how long the local link-layer transmit queue remains busy.
   *
   * This estimate is maintained from Homa's own transmit activity, so it
   * assumes the node is not simultaneously draining unrelated traffic.
   *
   * \return Remaining drain time of the transmit queue
   */
  Time GetTxQueueDrainDelay (void);
    
  // inherited from Ipv4L4Protocol
  /**
   * \brief Receive a packet from the lower IP layer
   * \param p The arriving packet from the network
   * \param header The IPv4 header of the arriving packet
   * \param interface The interface from which the packet arrives
   */ 
  virtual enum IpL4Protocol::RxStatus Receive (Ptr<Packet> p,
                                               Ipv4Header const &header,
                                               Ptr<Ipv4Interface> interface);
  virtual enum IpL4Protocol::RxStatus Receive (Ptr<Packet> p,
                                               Ipv6Header const &header,
                                               Ptr<Ipv6Interface> interface);
  
  /**
   * \brief Forward the reassembled message to the upper layers
   * \param completeMsg The message that is completely reassembled
   * \param header The IPv4 header associated with the message
   * \param sport The source port of the message
   * \param dport The destinateion port of the message
   * \param txMsgId The message ID determined by the sender
   * \param incomingInterface The interface from which the message arrived
   */ 
  void ForwardUp (Ptr<Packet> completeMsg,
                  const Ipv4Header &header,
                  uint16_t sport, uint16_t dport, uint16_t txMsgId,
                  Ptr<Ipv4Interface> incomingInterface);
  
  // inherited from Ipv4L4Protocol (Not used for Homa Transport Purposes)
  virtual void ReceiveIcmp (Ipv4Address icmpSource, uint8_t icmpTtl,
                            uint8_t icmpType, uint8_t icmpCode, uint32_t icmpInfo,
                            Ipv4Address payloadSource,Ipv4Address payloadDestination,
                            const uint8_t payload[8]);
    
  // From IpL4Protocol
  virtual void SetDownTarget (IpL4Protocol::DownTargetCallback cb);
  virtual void SetDownTarget6 (IpL4Protocol::DownTargetCallback6 cb);
    
  // From IpL4Protocol
  virtual IpL4Protocol::DownTargetCallback GetDownTarget (void) const;
  virtual IpL4Protocol::DownTargetCallback6 GetDownTarget6 (void) const;

protected:
  virtual void DoDispose (void);
  /*
   * This function will notify other components connected to the node that a 
   * new stack member is now connected. This will be used to notify Layer 3 
   * protocol of layer 4 protocol stack to connect them together.
   */
  virtual void NotifyNewAggregate ();
    
private:
  Ptr<Node> m_node; //!< the node this stack is associated with
  Ipv4EndPointDemux *m_endPoints; //!< A list of IPv4 end points.
    
  std::vector<Ptr<HomaSocket> > m_sockets;      //!< list of sockets
  IpL4Protocol::DownTargetCallback m_downTarget;   //!< Callback to send packets over IPv4
  IpL4Protocol::DownTargetCallback6 m_downTarget6; //!< Callback to send packets over IPv6 (Not supported)
    
  bool m_memIsOptimized; //!< High performant mode (only packet sizes are stored to save from memory)
  
  uint32_t m_mtu; //!< The MTU of the bounded NetDevice
  uint16_t m_bdp; //!< RTT BDP in packets for Homa's in-flight window baseline.
    
  uint8_t m_numTotalPrioBands;   //!< Total number of priority levels used within the network
  uint8_t m_numUnschedPrioBands; //!< Number of priority bands dedicated for unscheduled packets
  uint8_t m_overcommitLevel;     //!< Minimum number of messages to Grant at the same time
  bool m_useSrrScheduling;       //!< Keep active inbound messages in FIFO/SRR-like order instead of SRPT
  bool m_sirdEnabled;            //!< Whether SIRD-compatible grant/feedback logic is enabled
  uint16_t m_sirdCreditBudgetPkts; //!< Baseline grant budget in packets
  uint16_t m_sirdUnschThresholdPkts; //!< Threshold for line-rate startup in packets
  double m_sirdEcnMdFactor;      //!< Multiplicative decrease factor for ECN loop
  double m_sirdEcnAiStep;        //!< Additive increase step for ECN loop
  double m_sirdSenderMdFactor;   //!< Multiplicative decrease factor for sender feedback loop
  double m_sirdSenderAiStep;     //!< Additive increase step for sender feedback loop
  double m_sirdEcnAlphaGain;     //!< EWMA gain for ECN mark ratio
  uint16_t m_sirdSenderCsnThreshold; //!< Threshold for sender congestion notification in packets
  Time m_sirdSenderCreditLaunchDelay; //!< Sender-side delay before received scheduled credit can launch data
    
  DataRate m_linkRate;       //!< Data Rate of the corresponding net device for this prototocol
  Time m_nextTimeTxQueWillBeEmpty;   //!< Total amount of bytes serialized since the last time 
    
  Ptr<HomaSendScheduler> m_sendScheduler;  //!< The scheduler that manages transmission of HomaOutboundMsg
  Ptr<HomaRecvScheduler> m_recvScheduler;  //!< The scheduler that manages arrival of HomaInboundMsg
    
  Time m_inboundRtxTimeout;  //!< Time value to determine the retransmission timeout of InboundMsgs
  Time m_outboundRtxTimeout; //!< Time value to determine the retransmission timeout of OutboundMsgs
  uint16_t m_maxNumRtxPerMsg;    //!< Maximum allowed rtx timeout count per message
    
  TracedCallback<Ptr<const Packet>, Ipv4Address, Ipv4Address, uint16_t, uint16_t, int> m_msgBeginTrace;
  TracedCallback<Ptr<const Packet>, Ipv4Address, Ipv4Address, uint16_t, uint16_t, int> m_msgFinishTrace;
    
  TracedCallback<Ptr<const Packet>, Ipv4Address, Ipv4Address, uint16_t, uint16_t, int, 
                 uint16_t, uint8_t> m_dataRecvTrace; //!< Trace of {pkt, srcIp, dstIp, srcPort, dstPort, txMsgId, pktOffset, prio} for arriving DATA packets
  TracedCallback<Ptr<const Packet>, Ipv4Address, Ipv4Address, uint16_t, uint16_t, int, 
                 uint16_t, uint16_t> m_dataSendTrace; //!< Trace of {pkt, srcIp, dstIp, srcPort, dstPort, txMsgId, pktOffset, prio} for departing DATA packets
  TracedCallback<Ptr<const Packet>, Ipv4Address, Ipv4Address, uint16_t, uint16_t, uint8_t, 
                 uint16_t, uint8_t> m_ctrlRecvTrace; //!< Trace of {pkt, srcIp, dstIp, srcPort, dstPort, falg, grantOffset, prio} for arriving control packets
  TracedCallback<Ipv4Address, Ipv4Address, uint16_t, uint16_t, uint16_t,
                 uint8_t, uint16_t, uint8_t> m_ctrlRecvTxMsgTrace; //!< Trace of {srcIp, dstIp, srcPort, dstPort, txMsgId, flags, grantOffset, prio} for arriving control packets
  TracedCallback<Ipv4Address, Ipv4Address, uint16_t, uint16_t, uint16_t,
                 uint8_t, uint8_t, Time> m_pathRttTrace; //!< Trace of {senderIp, receiverIp, srcPort, dstPort, txMsgId, triggerKind, ctrlFlags, pathRtt} for Homa-derived path RTT
  TracedCallback<Ipv4Address, uint16_t, uint16_t, double, double, bool> m_sirdGrantDecisionTrace; //!< Trace of {senderIp, txMsgId, grantOffset, senderBudgetPkts, ecnEwma, csn}
  TracedCallback<Ipv4Address, Ipv4Address, double, uint32_t, uint32_t, uint32_t, uint8_t> m_sirdBucketStateTrace; //!< Trace of {receiverIp, senderIp, senderBudgetHostPkts, senderInUsePkts, globalInUsePkts, globalBudgetPkts, eventType}
  TracedCallback<Ipv4Address, Ipv4Address, uint8_t, uint32_t, uint16_t, uint8_t, bool, uint32_t> m_sirdPacketStateTrace; //!< Trace of per-packet SIRD state
  TracedCallback<Ipv4Address, Ipv4Address, double, double, double, double, uint64_t, uint64_t> m_sirdLoopStateTrace; //!< Trace of per-sender SIRD loop state
  TracedCallback<Ipv4Address, Ipv4Address, uint16_t, uint32_t, uint8_t> m_sirdSenderCreditStateTrace; //!< Trace of sender-held scheduled credit
  TracedCallback<Ipv4Address, Ipv4Address, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t> m_sirdReceiverCreditStateTrace; //!< Trace of receiver available credit
};
    
/******************************************************************************/
    
/**
 * \ingroup homa
 * \brief Stores the state for an outbound Homa message
 */
class HomaOutboundMsg : public Object
{
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);

  HomaOutboundMsg (Ptr<Packet> message, 
                   Ipv4Address saddr, Ipv4Address daddr, 
                   uint16_t sport, uint16_t dport, 
                   Ptr<HomaL4Protocol> homa);
  ~HomaOutboundMsg (void);
  
  /**
   * \brief Set the route requested for this message. (0 if not source-routed)
   * \param route The corresponding route.
   */
  void SetRoute(Ptr<Ipv4Route> route);
  /**
   * \brief Get the route requested for this message. (0 if not source-routed)
   * \return The corresponding route.
   */
  Ptr<Ipv4Route> GetRoute (void);
  
  /**
   * \brief Get the remaining undelivered bytes of this message.
   * \return The amount of undelivered bytes
   */
  uint32_t GetRemainingBytes(void);
  /**
   * \brief Get the total number of bytes for this message.
   * \return The message size in bytes
   */
  uint32_t GetMsgSizeBytes(void);
  /**
   * \brief Get the total number of packets for this message.
   * \return The number of packets
   */
  uint16_t GetMsgSizePkts(void);
  /**
   * \brief Get the sender's IP address for this message.
   * \return The IPv4 address of the sender
   */
  Ipv4Address GetSrcAddress (void);
  /**
   * \brief Get the receiver's IP address for this message.
   * \return The IPv4 address of the receiver
   */
  Ipv4Address GetDstAddress (void);
  /**
   * \brief Get the sender's port number for this message.
   * \return The port number of the sender
   */
  uint16_t GetSrcPort (void);
  /**
   * \brief Get the receiver's port number for this message.
   * \return The port number of the receiver
   */
  uint16_t GetDstPort (void);
  /**
   * \brief Get the highest granted packet offset for this message.
   * \return The highest granted packet offset
   */
  uint16_t GetMaxGrantedIdx (void);
  
  /**
   * \return Whether this message has expired and to be cleared upon rtx timeouts
   */
  bool IsExpired (void);
  
  /**
   * \brief Get the priority requested for this message by the receiver.
   * \param The offset of the packet that priority is being calculated for
   * \return The priority of this message
   */
  uint8_t GetPrio (uint16_t pktOffset);
  
  /**
   * \brief Gets the most recent retransmission event for this message, either scheduled or expired.
   * \return The most recent retransmission event scheduled by the HomaSendScheduler.
   */
  EventId GetRtxEvent (void);
  
  /**
   * \brief Determines which packet should be sent next for this message
   * \param pktOffset The index of the selected packet (determined inside this function)
   * \return Whether a packet was successfully selected for this message 
   */
  bool TryGetNextPktOffset (uint16_t &pktOffset);

  /**
   * \brief Whether this message must send a zero-payload credit request first.
   * \return true if an initial request packet should be transmitted
   */
  bool NeedsInitialCreditRequest (void) const;

  /**
   * \brief Mark the initial credit request as transmitted.
   */
  void MarkInitialCreditRequestSent (void);

  /**
   * \brief Generate a zero-payload DATA packet to request initial credits.
   * \param txMsgId The txMsgId assigned by HomaSendScheduler
   * \return The generated request packet
   */
  Ptr<Packet> GenerateInitialCreditRequest (uint16_t txMsgId);
  
  /**
   * \brief Remove the next packet from the TX queue of this message
   * \param pktOffset The offset of the packet which is to be set as sent
   * \return The packet that is to be sent next from this message
   */
  Ptr<Packet> RemoveNextPktFromTxQ (uint16_t pktOffset);
  
  /**
   * \brief Update the state per the received Grant
   * \param homaHeader The header information for the received Grant
   */
  void HandleGrantOffset (HomaHeader const &homaHeader);
  
  /**
   * \brief Update the m_toBeTxPackets state per the received Resend
   * \param homaHeader The header information for the received Resend
   */
  void HandleResend (HomaHeader const &homaHeader);
  
  /**
   * \brief Reset the remaining bytes state per the received Ack
   * \param homaHeader The header information for the received Ack
   */
  void HandleAck (HomaHeader const &homaHeader);
  
  /**
   * \brief Generates a busy packet to the receiver of the this message
   * \param targetTxMsgId The txMsgId of this message (determined by the HomaSendScheduler)
   * \return The generated BUSY packet
   */
  Ptr<Packet> GenerateBusy (uint16_t targetTxMsgId);
  
  /**
   * \brief Determines whether there exists some data packets to retransmit
   * \param lastRtxGrntIdx The m_maxGrantedIdx value as of the time rtx timer was set
   */
  void ExpireRtxTimeout(uint16_t lastRtxGrntIdx);

  /**
   * \brief Get currently accumulated scheduled credit for this message in packets.
   * \return number of granted-but-unsent scheduled packets
   */
  uint16_t GetAccumulatedCreditPkts (void) const;

  /**
   * \brief Count scheduled credits whose sender-side launch delay has expired.
   * \return currently sendable scheduled credits
   */
  uint16_t GetAvailableSirdCreditPkts (void) const;

  /**
   * \brief Find when the next delayed SIRD credit becomes sendable.
   * \param delay Time from now until the next credit can launch DATA
   * \return true if this message is blocked only by sender-side credit delay
   */
  bool GetNextSirdCreditDelay (Time &delay) const;

  /**
   * \brief Consume one mature SIRD scheduled credit after a DATA packet is selected.
   */
  void ConsumeSirdCredit (void);
  
private:
  void AddSirdCreditAvailability (uint16_t creditPkts);

  Ipv4Address m_saddr;       //!< Source IP address of this message
  Ipv4Address m_daddr;       //!< Destination IP address of this message
  uint16_t m_sport;          //!< Source port of this message
  uint16_t m_dport;          //!< Destination port of this message
  Ptr<Ipv4Route> m_route;    //!< Route of the message determined by the sender socket 
  Ptr<HomaL4Protocol> m_homa;//!< the protocol instance itself that creates/sends/receives messages
  
  // Only one of the two below are used depending on the memory optimizations enabled
  std::vector<Ptr<Packet>> m_packets;   //!< Packet buffer for the message
  std::vector<uint32_t> m_pktSizes;     //!< Optimized packet buffer that keeps only the packet sizes instead of contents
  
  std::priority_queue<uint16_t, std::vector<uint16_t>, std::greater<uint16_t> > m_pktTxQ; //!< Min-heap of packet offsets still waiting to be sent or resent.
  
  uint32_t m_msgSizeBytes;   //!< Number of bytes this message occupies
  uint32_t m_maxPayloadSize; //!< Number of bytes that can be stored in packet excluding headers
  uint32_t m_remainingBytes; //!< Sender-side estimate of bytes not yet fully delivered/ACKed for this message.
  uint16_t m_maxGrantedIdx;  //!< Highest packet offset the receiver has currently authorized this sender to transmit.
  
  uint8_t m_prio;            //!< Most recent transmit priority requested by the receiver for this message.
  bool m_prioSetByReceiver;  //!< Whether m_prio already reflects a receiver-issued GRANT/RESEND priority.
  bool m_waitForFirstGrant;  //!< Whether real DATA is blocked until the first explicit GRANT arrives.
  bool m_initialCreditRequestSent; //!< Whether the zero-payload DATA used to request the first GRANT was already sent.
  std::deque<Time> m_sirdCreditAvailableTimes; //!< Per-credit release times after sender-side launch delay; one mature entry permits one DATA send.
  
  EventId m_rtxEvent;        //!< Sender-side timeout event used to detect lack of grant progress for this message.
  bool m_isExpired;          //!< Whether the sender should garbage-collect this message after timeout with no newer grant progress.
};
 
/******************************************************************************/
    
/**
 * \ingroup homa
 *
 * \brief Manages the transmission of all HomaOutboundMsg from HomaL4Protocol
 *
 * This class keeps the state necessary for transmisssion of the messages. 
 * For every new message that arrives from the applications, this class is 
 * responsible for sending the data packets as grants are received.
 *
 */
class HomaSendScheduler : public Object
{
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);
  static const uint16_t MAX_N_MSG; //!< Maximum number of messages a HomaSendScheduler can hold

  HomaSendScheduler (Ptr<HomaL4Protocol> homaL4Protocol);
  ~HomaSendScheduler (void);
  
  /**
   * \brief Accept a new message from the upper layers and add to the list of pending messages
   * \param outMsg The outbound message to be accepted
   * \return The txMsgId allocated for this message (-1 if the message is not scheduled)
   */
  int ScheduleNewMsg (Ptr<HomaOutboundMsg> outMsg);
  
  /**
   * \brief Send the next packet down to the IP layer and schedule next TX.
   */
  void TxDataPacket(void);
  
  /**
   * \brief Updates the state for the corresponding outbound message per the received control packet.
   * \param ipv4Header The Ipv4 header of the received GRANT.
   * \param homaHeader The Homa header of the received GRANT.
   */
  void HandleControlPacketForOutboundMsg (Ipv4Header const &ipv4Header,
                                          HomaHeader const &homaHeader);
  
  /**
   * \brief Delete the state for a msg and set the txMsgId as free again
   * \param txMsgId The TX msg ID of the message to be cleared
   */
  void ClearStateForMsg (uint16_t txMsgId);

  /**
   * \brief Get total accumulated scheduled credit across all outbound messages.
   * \return total unconsumed scheduled credit in packets
   */
  uint32_t GetAccumulatedCreditPkts (void) const;
  
private:
  /**
   * \brief Determines which message would be selected to send a packet from
   * \param txMsgId The TX msg ID of the selected message (determined inside this function)
   * \return Whether a message was successfully selected
   */
  bool SelectNextSendableMsgId (uint16_t &txMsgId);

  /**
   * \brief Select the next packet to transmit and identify its owning message.
   * \param txMsgId The TX msg ID of the selected message (determined inside this function)
   * \param p The selected packet from the corresponding message (determined inside this function)
   * \return Whether a packet could successfully be selected
   */
  bool GetNextPacket (uint16_t &txMsgId, Ptr<Packet> &p);

  Ptr<HomaL4Protocol> m_homa; //!< the protocol instance itself that sends/receives messages
  
  EventId m_txEvent;          //!< The EventID for the next scheduled transmission
  
  std::list<uint16_t> m_txMsgIdFreeList;  //!< List of free TX msg IDs
  std::unordered_map<uint16_t, Ptr<HomaOutboundMsg>> m_outboundMsgs; //!< state to keep HomaOutboundMsg with the key as txMsgId
};
    
/******************************************************************************/
    
/**
 * \ingroup homa
 * \brief Stores the state for an inbound Homa message
 */
class HomaInboundMsg : public Object
{
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);

  HomaInboundMsg (Ptr<Packet> p, Ipv4Header const &ipv4Header, HomaHeader const &homaHeader,
                  Ptr<Ipv4Interface> iface, uint32_t mtuBytes, uint16_t rttPackets,
                  bool memIsOptimized, bool sirdEnabled, uint16_t sirdUnschThresholdPkts);
  ~HomaInboundMsg (void);
  
  /**
   * \brief Get the remaining undelivered bytes of this message.
   * \return The amount of undelivered bytes
   */
  uint32_t GetRemainingBytes(void);
  
  /**
   * \brief Get the sender's IP address for this message.
   * \return The IPv4 address of the sender
   */
  Ipv4Address GetSrcAddress (void);
  /**
   * \brief Get the receiver's IP address for this message.
   * \return The IPv4 address of the receiver
   */
  Ipv4Address GetDstAddress (void);
  /**
   * \brief Get the sender's port number for this message.
   * \return The port number of the sender
   */
  uint16_t GetSrcPort (void);
  /**
   * \brief Get the receiver's port number for this message.
   * \return The port number of the receiver
   */
  uint16_t GetDstPort (void);
  /**
   * \brief Get the TX msg ID for this message.
   * \return The TX msg ID determined the sender
   */
  uint16_t GetTxMsgId (void);
  
  /**
   * \brief Get the Ipv4Header associated with the first arrived packet of the message
   * \return The IPv4 header associated with the message
   */
  Ipv4Header GetIpv4Header (void);
  /**
   * \brief Get the interface from which the message arrives.
   * \return The interface from which the message arrives
   */
  Ptr<Ipv4Interface> GetIpv4Interface (void);
  
  /**
   * \brief Sets the scheduled retransmission event for this message
   * \param rtxEvent The retransmission event scheduled by the HomaRecvScheduler.
   */
  void SetRtxEvent (EventId rtxEvent);
  /**
   * \brief Gets the most recent retransmission event for this message, either scheduled or expired.
   * \return The most recent retransmission event scheduled by the HomaRecvScheduler.
   */
  EventId GetRtxEvent (void);
  
  /**
   * \brief Get the highest grantable packet index for this message.
   * \return The highest grantable packet index so far
   */
  uint16_t GetMaxGrantableIdx (void);

  /**
   * \brief Cap the grantable window relative to the latest granted offset.
   * \param grantWindowPkts window size in packets to allow beyond max granted index
   */
  void CapGrantableWindow (uint16_t grantWindowPkts);
  /**
   * \brief Advance the grantable boundary by newly issued SIRD credit.
   * \param grantPkts number of packet credits to expose to the grant generator
   * \return true if the grantable boundary moved forward
   */
  bool AdvanceGrantableWindow (uint16_t grantPkts);
  /**
   * \brief Get the highest granted packet index for this message.
   * \return The highest granted packet index so far
   */
  uint16_t GetMaxGrantedIdx (void);
  
  /**
   * \brief Set m_maxGrantableIdx value as of last time rtx timer expired.
   * \param The highest grantable pkt index as of last time rtx timer expired.
   */
  void SetLastRtxGrntIdx (uint16_t lastRtxGrntIdx);
  /**
   * \brief Get m_maxGrantableIdx value as of last time rtx timer expired.
   * \return The highest grantable pkt index as of last time rtx timer expired.
   */
  uint16_t GetLastRtxGrntIdx (void);
  
  /**
   * \return Whether this message has been fully granted
   */
  bool IsFullyGranted (void);
  /**
   * \return Whether this message has grantable packets that are not granted yet
   */
  bool IsGrantable (void);
  /**
   * \return Whether this message has been fully received
   */
  bool IsFullyReceived (void);

  /**
   * \brief Get whether m_currentlyScheduled value is true.
   */
  bool IsCurrentlyScheduled (void);
  /**
   * \brief Get whether m_currentlyScheduled value is true.
   * \param currentlyScheduled The current value to denote being scheduled or not
   */
  void SetCurrentlyScheduled (bool currentlyScheduled);
  
  /**
   * \brief Get the number of rtx timeouts without receiving any new packet
   * \return The number of consecutive retransmission timeouts
   */
  uint16_t GetNumRtxWithoutProgress (void);
  /**
   * \brief Increments the number of rtx timeouts by 1
   */
  void IncrNumRtxWithoutProgress (void);
  /**
   * \brief Resent the number of rtx timeouts to 0
   */
  void ResetNumRtxWithoutProgress (void);
  
  /**
   * \brief Insert the received data packet in the buffer and update state
   * \param p The received data packet
   * \param pktOffset The offset of the received packet within the message
   */
  void ReceiveDataPacket (Ptr<Packet> p, uint16_t pktOffset);
  
  /**
   * \brief Reassembles the message from its packets
   * \return The reassembled message
   */
  Ptr<Packet> GetReassembledMsg (void);
  
  /**
   * \brief Generate a GRANT or an ACK packet with the most recent state of this message
   * \param grantedPrio The priority to grant DATA packets with
   * \param pktTypeFlag The type of the packet (Grant or ACK)
   * \return The generated GRANT or ACK packet
   */
  Ptr<Packet> GenerateGrantOrAck(uint8_t grantedPrio, uint8_t pktTypeFlag);
  
  /**
   * \brief Generate a list of RESEND packets to send upon retransmission timeout
   * \param maxRsndPktOffset The highest packet index to decide RESENDs upto
   * \return The list of RESEND packets
   */
  std::list<Ptr<Packet>> GenerateResends (uint16_t maxRsndPktOffset);

private:
  Ipv4Header m_ipv4Header;    //!< The IPv4 Header of the first packet arrived for this message
  Ptr<Ipv4Interface> m_iface; //!< The interface from which the message first came in from
  
  uint16_t m_sport;           //!< Source port of this message
  uint16_t m_dport;           //!< Destination port of this message
  uint16_t m_txMsgId;         //!< TX msg ID of the message determined by the sender
  
  // Only one of the two below are used depending on the memory optimizations enabled
  std::vector<Ptr<Packet>> m_packets;  //!< Packet buffer for the message
  std::vector<uint32_t> m_pktSizes;    //!< Optimized packet buffer that keeps only the packet sizes instead of contents
  
  std::vector<bool> m_receivedPackets; //!< Per-packet receive bitmap; false entries are candidates for future RESEND generation.
   
  uint32_t m_remainingBytes; //!< Remaining number of bytes that are not received yet
  uint32_t m_msgSizeBytes;   //!< Number of bytes this message occupies
  uint16_t m_msgSizePkts;    //!< Number packets this message occupies
  uint16_t m_maxGrantableIdx;//!< Highest packet offset the receiver is currently willing to expose to GRANT generation.
  uint16_t m_maxGrantedIdx;  //!< Highest packet offset already advertised to the sender in a GRANT packet.
  uint8_t m_prio;            //!< Most recent priority carried in GRANT/ACK/RESEND generated for this message.
  bool m_hasGrantedData;     //!< Whether at least one non-ACK GRANT has been sent, i.e. sender may already launch scheduled DATA.
  bool m_creditDrivenGrantWindow; //!< Whether the grantable boundary advances only through SIRD credit issuance, not just DATA arrivals.
  bool m_currentlyScheduled; //!< Whether the scheduler selected this message in the current round as actively grant-served.
  
  EventId m_rtxEvent;        //!< Receiver-side timeout event that triggers RESEND generation when granted data stops arriving.
  uint16_t m_lastRtxGrntIdx; //!< Snapshot of m_maxGrantableIdx at the previous timeout, used to detect whether grant progress advanced.
  uint16_t m_numRtxWithoutProgress;   //!< Consecutive timeout count with no new receive-side progress, used to stop retrying stalled flows.
};
    
/******************************************************************************/
    
/**
 * \ingroup homa
 *
 * \brief Manages the arrival of all HomaInboundMsg from HomaL4Protocol
 *
 * This class keeps the state necessary for arrival of the messages. 
 * For every new message that arrives from the network, this class is 
 * responsible for scheduling the messages and sending the control packets.
 *
 */
class HomaRecvScheduler : public Object
{
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId (void);

  HomaRecvScheduler (Ptr<HomaL4Protocol> homaL4Protocol);
  ~HomaRecvScheduler (void);
  
  /**
   * \brief Notify this HomaRecvScheduler upon arrival of a packet
   * \param packet The received packet (without any headers)
   * \param ipv4Header IPv4 header of the received packet
   * \param homaHeader The Homa header of the received packet
   * \param interface The interface from which the packet came in
   */
  void ReceivePacket (Ptr<Packet> packet, 
                      Ipv4Header const &ipv4Header,
                      HomaHeader const &homaHeader,
                      Ptr<Ipv4Interface> interface);
  
  /**
   * \brief Notify this HomaRecvScheduler upon arrival of a data packet
   * \param packet The received packet (without any headers)
   * \param ipv4Header IPv4 header of the received packet
   * \param homaHeader The Homa header of the received packet
   * \param interface The interface from which the packet came in
   */
  void ReceiveDataPacket (Ptr<Packet> packet, 
                          Ipv4Header const &ipv4Header,
                          HomaHeader const &homaHeader,
                          Ptr<Ipv4Interface> interface);
  
  /**
   * \brief Try to find the active inbound message matching the received packet.
   * \param ipv4Header IPv4 header of the received packet.
   * \param homaHeader The Homa header of the received packet.
   * \param msgIdx The index of the matching message in the active list.
   * \return Whether the corresponding inbound message was found.
   */
  bool FindInboundMsg (Ipv4Header const &ipv4Header,
                       HomaHeader const &homaHeader,
                       int &msgIdx);
  
  /**
   * \brief Insert or reorder a message within the active inbound list.
   * \param inboundMsg The message to place in scheduling order
   * \param msgIdx The old index of the message if it is already active, -1 otherwise
   */
  void RescheduleInboundMsg (Ptr<HomaInboundMsg> inboundMsg, int msgIdx);
  
  /**
   * \brief Reassemble the packets of the incoming message and forward up to the sockets.
   * \param inboundMsg The incoming message that is fully received.
   * \param msgIdx The index of the message if it is a pending one, -1 otherwise.
   */
  void ForwardUp(Ptr<HomaInboundMsg> inboundMsg, int msgIdx);
  
  /**
   * \brief Cancel timer state and remove an inbound message from the active list.
   * \param inboundMsg The incoming message that is to be removed
   * \param msgIdx The index of the message if it is active, -1 otherwise
   */
  void RemoveInboundMsg (Ptr<HomaInboundMsg> inboundMsg, int msgIdx);
  
  /**
   * \brief Walk active inbound messages and issue any GRANTs that are currently allowed.
   */
  bool IssuePendingGrants (void);
  
  /**
   * \brief Gets appropriate RESEND packets for the inbound message and sends them down.
   * \param inboundMsg The inbound message whose retransmission timer expires
   * \param maxRsndPktOffset The highest packet index to send RESEND for 
   */
  void ExpireRtxTimeout(Ptr<HomaInboundMsg> inboundMsg, uint16_t maxRsndPktOffset);

  /**
   * \brief Ensure the next tick-based credit issuance event is scheduled.
   * \param immediate true to schedule at current simulation time
   */
  void EnsureCreditTickScheduled (bool immediate);

  /**
   * \brief Periodic driver for SIRD credit issuance.
   */
  void CreditTick (void);

  /**
   * \brief Check whether any sender can receive more credits right now.
   * \return true when at least one grant opportunity exists
   */
  bool HasGrantOpportunity (void) const;

  /**
   * \brief Get spacing between tick-driven grant opportunities.
   * \return interval between consecutive credit ticks
   */
  Time GetCreditTickInterval (void) const;
  
private:
  Ptr<HomaL4Protocol> m_homa; //!< The protocol instance that owns this receive scheduler.
  
  std::vector<Ptr<HomaInboundMsg>> m_inboundMsgs; //!< Active inbound messages ordered for grant scheduling.
  std::unordered_set<uint32_t>  m_busySenders; //!< Senders whose most recent control state asks us to back off.
  std::unordered_map<uint32_t, double> m_sirdSenderBudgetNetPkts; //!< Per-sender credit cap from the ECN/network loop, in packets.
  std::unordered_map<uint32_t, double> m_sirdSenderBudgetHostPkts; //!< Per-sender credit cap from sender-host feedback, in packets.
  std::unordered_map<uint32_t, double> m_sirdSenderNetAlpha; //!< DCTCP-style CE fraction EWMA per sender
  std::unordered_map<uint32_t, double> m_sirdSenderHostAlpha; //!< DCTCP-style CSN fraction EWMA per sender
  std::unordered_map<uint32_t, uint32_t> m_sirdSenderCreditsInUsePkts; //!< Outstanding credits currently consumed by each sender.
  std::unordered_map<uint32_t, bool> m_sirdSenderCsnState; //!< Last seen csn feedback per sender
  std::unordered_map<uint32_t, bool> m_sirdSenderCeState; //!< Last seen CE mark per sender
  std::unordered_map<uint32_t, uint64_t> m_sirdSenderDataPktsObserved; //!< Number of DATA packets observed per sender
  std::unordered_map<uint32_t, uint64_t> m_sirdSenderCeMarksObserved; //!< Number of CE-marked DATA packets observed per sender
  std::unordered_map<uint32_t, uint64_t> m_sirdSenderCsnMarksObserved; //!< Number of CSN-marked DATA packets observed per sender
  std::unordered_map<uint32_t, uint64_t> m_sirdSenderEpochDataPkts; //!< Number of DATA packets observed in current sender epoch
  std::unordered_map<uint32_t, uint64_t> m_sirdSenderEpochCeMarks; //!< Number of CE-marked DATA packets in current sender epoch
  std::unordered_map<uint32_t, uint64_t> m_sirdSenderEpochCsnMarks; //!< Number of CSN-marked DATA packets in current sender epoch
  uint32_t m_sirdGlobalCreditsInUsePkts; //!< Outstanding credits currently consumed across all senders.
  double m_sirdCeRatioEwma; //!< EWMA of ECN CE ratio over received data packets
  uint64_t m_sirdDataPktsObserved; //!< Number of data packets observed for ECN ratio estimation
  uint64_t m_sirdCeMarksObserved; //!< Number of CE-marked packets observed
  bool m_srrHaveLastGrantedSender; //!< Whether SRR has a valid sender cursor
  uint32_t m_srrLastGrantedSender; //!< Sender key that most recently received SRR credit
  EventId m_creditTickEvent; //!< Periodic event for tick-driven credit issuance
};
    
} // namespace ns3

#endif /* HOMA_L4_PROTOCOL_H */
