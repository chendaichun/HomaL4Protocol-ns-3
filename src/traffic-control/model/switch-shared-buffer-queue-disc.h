/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Shared-buffer switch queue disc skeleton for 400G single-switch experiments.
 */

#ifndef SWITCH_SHARED_BUFFER_QUEUE_DISC_H
#define SWITCH_SHARED_BUFFER_QUEUE_DISC_H

#include "ns3/callback.h"
#include "ns3/queue-disc.h"
#include "ns3/random-variable-stream.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <unordered_map>

namespace ns3 {

class SwitchSharedBufferPool;

class SwitchSharedBufferQueueDisc : public QueueDisc
{
public:
  typedef Callback<void, uint32_t> PauseCallback;
  typedef Callback<void, uint32_t> ResumeCallback;
  typedef Callback<void, uint32_t, uint32_t, double> QcnCallback;

  static TypeId GetTypeId (void);

  SwitchSharedBufferQueueDisc ();
  ~SwitchSharedBufferQueueDisc () override;

  static constexpr const char* LIMIT_EXCEEDED_DROP = "Queue disc limit exceeded";
  static constexpr const char* SHARED_BUFFER_EXCEEDED_DROP = "Switch shared-buffer limit exceeded";
  static constexpr const char* ECN_MARK = "Switch shared-buffer ECN mark";

  void SetSharedBufferPool (Ptr<SwitchSharedBufferPool> pool);
  Ptr<SwitchSharedBufferPool> GetSharedBufferPool (void) const;

  void SetPauseCallback (PauseCallback cb);
  void SetResumeCallback (ResumeCallback cb);
  void SetQcnCallback (QcnCallback cb);

  uint32_t GetEgressId (void) const;
  uint64_t GetSharedBytes (void) const;

private:
  bool DoEnqueue (Ptr<QueueDiscItem> item) override;
  Ptr<QueueDiscItem> DoDequeue (void) override;
  Ptr<const QueueDiscItem> DoPeek (void) override;
  bool CheckConfig (void) override;
  void InitializeParams (void) override;

  uint32_t GetItemSourceId (Ptr<const QueueDiscItem> item) const;
  uint32_t GetItemFlowId (Ptr<const QueueDiscItem> item) const;
  bool ShouldMark (uint32_t occupancyPackets) const;
  void MaybeSignalCongestion (Ptr<QueueDiscItem> item,
                              uint32_t occupancyPackets,
                              bool marked);
  void MaybeSignalResume (uint32_t sourceId);
  uint32_t ComputePauseThreshold (void) const;
  uint32_t ComputeResumeThreshold (void) const;

  QueueSize m_sharedBufferSize; //!< Global shared-buffer budget exposed for scenario wiring.
  Ptr<SwitchSharedBufferPool> m_pool; //!< Optional shared pool used by all switch egresses.
  uint32_t m_egressId;          //!< Scenario-level egress id for traces and shared accounting.
  uint32_t m_kmin;              //!< ECN minimum marking threshold in packets.
  uint32_t m_kmax;              //!< ECN maximum marking threshold in packets.
  double m_pmax;                //!< ECN maximum marking probability.
  bool m_useEcn;                //!< Whether ECN marking is enabled.
  bool m_usePfc;                //!< Whether PFC-style pause/resume events are enabled.
  bool m_useDynamicPfcThreshold; //!< Whether pause threshold follows shared-buffer headroom.
  uint32_t m_pauseThreshold;    //!< Static pause threshold in packets.
  uint32_t m_resumeThreshold;   //!< Static resume threshold in packets.
  uint32_t m_minDynamicPauseThreshold; //!< Lower bound for dynamic pause threshold.
  uint32_t m_dynamicThresholdPacketBytes; //!< Packet byte size used when converting shared-buffer headroom to packets.
  double m_dynamicPauseHeadroomFraction; //!< Fraction of spare shared buffer exposed as pause headroom.
  bool m_useQcn;                //!< Whether QCN-style rate feedback events are enabled.
  double m_qcnMdFactor;         //!< Multiplicative decrease factor sent on QCN events.
  uint32_t m_qcnIntervalPackets; //!< Minimum queued-packet delta between QCN events per sender.
  Ptr<UniformRandomVariable> m_uv; //!< RNG for probabilistic marking.
  std::unordered_map<uint32_t, bool> m_pausedSources; //!< PFC pause state per source.
  std::unordered_map<uint32_t, uint32_t> m_lastQcnAtPackets; //!< Last QCN queue depth per source.
  PauseCallback m_pauseCallback; //!< Callback into the scenario/controller.
  ResumeCallback m_resumeCallback; //!< Callback into the scenario/controller.
  QcnCallback m_qcnCallback; //!< Callback into the scenario/controller.

  TracedCallback<uint32_t, uint32_t, uint32_t, uint64_t, bool> m_queueStateTrace;
  TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t> m_pfcTrace;
  TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t, double> m_qcnTrace;
  TracedCallback<uint32_t, uint32_t, uint32_t, uint32_t> m_ecnTrace;
};

} // namespace ns3

#endif /* SWITCH_SHARED_BUFFER_QUEUE_DISC_H */
