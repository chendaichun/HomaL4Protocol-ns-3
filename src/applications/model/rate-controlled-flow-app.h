/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Timer-driven flow application with explicit rate and pause control.
 */

#ifndef RATE_CONTROLLED_FLOW_APP_H
#define RATE_CONTROLLED_FLOW_APP_H

#include "ns3/address.h"
#include "ns3/application.h"
#include "ns3/data-rate.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/traced-callback.h"

namespace ns3 {

class Socket;

class RateControlledFlowApp : public Application
{
public:
  static TypeId GetTypeId (void);

  RateControlledFlowApp ();
  ~RateControlledFlowApp () override;

  DataRate GetCurrentRate (void) const;
  uint64_t GetTotalBytes (void) const;
  void ApplyMultiplicativeDecrease (double factor);
  void ApplyAdditiveIncrease (DataRate step);
  void PauseFlow (void);
  void ResumeFlow (void);
  bool IsPaused (void) const;

protected:
  void DoDispose (void) override;

private:
  void StartApplication (void) override;
  void StopApplication (void) override;

  void CancelSendEvent (void);
  void CancelAiEvent (void);
  void ScheduleNextTx (void);
  void ScheduleAdditiveIncrease (void);
  void AdditiveIncreaseTick (void);
  void SendPacket (void);

  Ptr<Socket> m_socket;     //!< Associated socket.
  Address m_peer;           //!< Remote peer address.
  TypeId m_tid;             //!< Socket type.
  DataRate m_initialRate;   //!< Configured initial sending rate.
  DataRate m_currentRate;   //!< Mutable runtime sending rate.
  DataRate m_minRate;       //!< Minimum rate allowed after feedback.
  DataRate m_maxRate;       //!< Maximum rate allowed by additive recovery.
  DataRate m_aiStep;        //!< Additive recovery step applied periodically.
  Time m_aiInterval;        //!< Additive recovery interval, 0 disables recovery.
  uint8_t m_ipTos;          //!< IPv4 TOS byte, including ECN bits.
  uint32_t m_packetSize;    //!< Packet payload size.
  uint64_t m_maxBytes;      //!< Transmission limit, 0 means unlimited.
  uint64_t m_totBytes;      //!< Total bytes sent so far.
  uint32_t m_flowId;        //!< Scenario-level flow id written into packet tags.
  uint32_t m_sourceId;      //!< Scenario-level sender/ingress id written into packet tags.
  bool m_paused;            //!< Whether the sender is currently paused.
  EventId m_sendEvent;      //!< Next send event.
  EventId m_aiEvent;        //!< Next additive recovery event.

  TracedCallback<Ptr<const Packet> > m_txTrace; //!< Packet transmission trace.
  TracedCallback<uint64_t, uint64_t> m_rateTrace; //!< oldRate, newRate.
  TracedCallback<bool> m_pauseTrace; //!< true on pause, false on resume.
};

} // namespace ns3

#endif /* RATE_CONTROLLED_FLOW_APP_H */
