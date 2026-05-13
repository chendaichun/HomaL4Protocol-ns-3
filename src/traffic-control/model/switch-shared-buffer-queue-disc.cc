/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "switch-shared-buffer-queue-disc.h"

#include "ns3/drop-tail-queue.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/object-factory.h"
#include "ns3/pointer.h"
#include "ns3/simulator.h"
#include "ns3/switch-flow-id-tag.h"
#include "ns3/switch-shared-buffer-pool.h"
#include "ns3/uinteger.h"

#include <algorithm>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("SwitchSharedBufferQueueDisc");

NS_OBJECT_ENSURE_REGISTERED (SwitchSharedBufferQueueDisc);

TypeId
SwitchSharedBufferQueueDisc::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SwitchSharedBufferQueueDisc")
    .SetParent<QueueDisc> ()
    .SetGroupName ("TrafficControl")
    .AddConstructor<SwitchSharedBufferQueueDisc> ()
    .AddAttribute ("MaxSize",
                   "The max queue size.",
                   QueueSizeValue (QueueSize ("1600p")),
                   MakeQueueSizeAccessor (&QueueDisc::SetMaxSize,
                                          &QueueDisc::GetMaxSize),
                   MakeQueueSizeChecker ())
    .AddAttribute ("SharedBufferSize",
                   "The exposed shared-buffer budget for the switch.",
                   QueueSizeValue (QueueSize ("82MB")),
                   MakeQueueSizeAccessor (&SwitchSharedBufferQueueDisc::m_sharedBufferSize),
                   MakeQueueSizeChecker ())
    .AddAttribute ("SharedBufferPool",
                   "Optional shared buffer pool used by all egress queue discs on the switch.",
                   PointerValue (),
                   MakePointerAccessor (&SwitchSharedBufferQueueDisc::SetSharedBufferPool,
                                        &SwitchSharedBufferQueueDisc::GetSharedBufferPool),
                   MakePointerChecker<SwitchSharedBufferPool> ())
    .AddAttribute ("EgressId",
                   "Scenario-level egress id used for shared-buffer accounting and traces.",
                   UintegerValue (0),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_egressId),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("Kmin",
                   "ECN minimum marking threshold in packets.",
                   UintegerValue (700),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_kmin),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("Kmax",
                   "ECN maximum marking threshold in packets.",
                   UintegerValue (1600),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_kmax),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("Pmax",
                   "ECN maximum marking probability.",
                   DoubleValue (0.2),
                   MakeDoubleAccessor (&SwitchSharedBufferQueueDisc::m_pmax),
                   MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("UseEcn",
                   "True to use ECN marking once occupancy crosses Kmin.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&SwitchSharedBufferQueueDisc::m_useEcn),
                   MakeBooleanChecker ())
    .AddAttribute ("UsePfc",
                   "True to emit PFC-style pause/resume events.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&SwitchSharedBufferQueueDisc::m_usePfc),
                   MakeBooleanChecker ())
    .AddAttribute ("UseDynamicPfcThreshold",
                   "True to derive the pause threshold from shared-buffer headroom.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&SwitchSharedBufferQueueDisc::m_useDynamicPfcThreshold),
                   MakeBooleanChecker ())
    .AddAttribute ("PauseThreshold",
                   "Static PFC pause threshold in packets.",
                   UintegerValue (1600),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_pauseThreshold),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("ResumeThreshold",
                   "Static PFC resume threshold in packets.",
                   UintegerValue (700),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_resumeThreshold),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("MinDynamicPauseThreshold",
                   "Lower bound for dynamic PFC pause threshold in packets.",
                   UintegerValue (700),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_minDynamicPauseThreshold),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("DynamicThresholdPacketBytes",
                   "Packet byte size used to convert shared-buffer headroom to packets.",
                   UintegerValue (4000),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_dynamicThresholdPacketBytes),
                   MakeUintegerChecker<uint32_t> (1))
    .AddAttribute ("DynamicPauseHeadroomFraction",
                   "Fraction of shared-buffer headroom exposed as dynamic pause threshold.",
                   DoubleValue (0.25),
                   MakeDoubleAccessor (&SwitchSharedBufferQueueDisc::m_dynamicPauseHeadroomFraction),
                   MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("UseQcn",
                   "True to emit QCN-style multiplicative-decrease feedback events.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&SwitchSharedBufferQueueDisc::m_useQcn),
                   MakeBooleanChecker ())
    .AddAttribute ("QcnMdFactor",
                   "Multiplicative decrease factor used by QCN feedback events.",
                   DoubleValue (0.5),
                   MakeDoubleAccessor (&SwitchSharedBufferQueueDisc::m_qcnMdFactor),
                   MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("QcnIntervalPackets",
                   "Minimum queued-packet delta between QCN events for the same source.",
                   UintegerValue (64),
                   MakeUintegerAccessor (&SwitchSharedBufferQueueDisc::m_qcnIntervalPackets),
                   MakeUintegerChecker<uint32_t> ())
    .AddTraceSource ("QueueState",
                     "Queue occupancy update: egressId, packets, bytes, sharedBytes, enqueue.",
                     MakeTraceSourceAccessor (&SwitchSharedBufferQueueDisc::m_queueStateTrace),
                     "ns3::TracedCallback::Uint32Uint32Uint32Uint64Bool")
    .AddTraceSource ("PfcEvent",
                     "PFC-style event: egressId, sourceId, queuePackets, pause(1)/resume(0).",
                     MakeTraceSourceAccessor (&SwitchSharedBufferQueueDisc::m_pfcTrace),
                     "ns3::TracedCallback::Uint32Uint32Uint32Uint32")
    .AddTraceSource ("QcnEvent",
                     "QCN-style event: egressId, sourceId, flowId, queuePackets, mdFactor.",
                     MakeTraceSourceAccessor (&SwitchSharedBufferQueueDisc::m_qcnTrace),
                     "ns3::TracedCallback::Uint32Uint32Uint32Uint32Double")
    .AddTraceSource ("EcnEvent",
                     "ECN mark event: egressId, sourceId, flowId, queuePackets.",
                     MakeTraceSourceAccessor (&SwitchSharedBufferQueueDisc::m_ecnTrace),
                     "ns3::TracedCallback::Uint32Uint32Uint32Uint32")
  ;
  return tid;
}

SwitchSharedBufferQueueDisc::SwitchSharedBufferQueueDisc ()
  : QueueDisc (QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE),
    m_sharedBufferSize (QueueSize ("82MB")),
    m_pool (0),
    m_egressId (0),
    m_kmin (700),
    m_kmax (1600),
    m_pmax (0.2),
    m_useEcn (true),
    m_usePfc (true),
    m_useDynamicPfcThreshold (true),
    m_pauseThreshold (1600),
    m_resumeThreshold (700),
    m_minDynamicPauseThreshold (700),
    m_dynamicThresholdPacketBytes (4000),
    m_dynamicPauseHeadroomFraction (0.25),
    m_useQcn (true),
    m_qcnMdFactor (0.5),
    m_qcnIntervalPackets (64),
    m_uv (CreateObject<UniformRandomVariable> ())
{
  NS_LOG_FUNCTION (this);
}

SwitchSharedBufferQueueDisc::~SwitchSharedBufferQueueDisc ()
{
  NS_LOG_FUNCTION (this);
}

void
SwitchSharedBufferQueueDisc::SetSharedBufferPool (Ptr<SwitchSharedBufferPool> pool)
{
  NS_LOG_FUNCTION (this << pool);
  m_pool = pool;
}

Ptr<SwitchSharedBufferPool>
SwitchSharedBufferQueueDisc::GetSharedBufferPool (void) const
{
  return m_pool;
}

void
SwitchSharedBufferQueueDisc::SetPauseCallback (PauseCallback cb)
{
  m_pauseCallback = cb;
}

void
SwitchSharedBufferQueueDisc::SetResumeCallback (ResumeCallback cb)
{
  m_resumeCallback = cb;
}

void
SwitchSharedBufferQueueDisc::SetQcnCallback (QcnCallback cb)
{
  m_qcnCallback = cb;
}

uint32_t
SwitchSharedBufferQueueDisc::GetEgressId (void) const
{
  return m_egressId;
}

uint64_t
SwitchSharedBufferQueueDisc::GetSharedBytes (void) const
{
  return m_pool ? m_pool->GetUsedBytes () : GetNBytes ();
}

bool
SwitchSharedBufferQueueDisc::DoEnqueue (Ptr<QueueDiscItem> item)
{
  NS_LOG_FUNCTION (this << item);

  QueueSize newSize = GetCurrentSize () + item;
  if (newSize > GetMaxSize ())
    {
      DropBeforeEnqueue (item, LIMIT_EXCEEDED_DROP);
      return false;
    }

  if (m_pool && !m_pool->Reserve (m_egressId, item->GetSize ()))
    {
      DropBeforeEnqueue (item, SHARED_BUFFER_EXCEEDED_DROP);
      return false;
    }

  uint32_t occupancyPackets = GetNPackets () + 1;
  bool marked = false;
  if (m_useEcn && ShouldMark (occupancyPackets))
    {
      marked = Mark (item, ECN_MARK);
      if (marked)
        {
          m_ecnTrace (m_egressId,
                      GetItemSourceId (item),
                      GetItemFlowId (item),
                      occupancyPackets);
        }
    }

  bool enqueued = GetInternalQueue (0)->Enqueue (item);
  if (!enqueued && m_pool)
    {
      m_pool->Release (m_egressId, item->GetSize ());
    }

  if (enqueued)
    {
      MaybeSignalCongestion (item, occupancyPackets, marked);
      m_queueStateTrace (m_egressId,
                         occupancyPackets,
                         GetNBytes (),
                         GetSharedBytes (),
                         true);
    }
  return enqueued;
}

Ptr<QueueDiscItem>
SwitchSharedBufferQueueDisc::DoDequeue (void)
{
  NS_LOG_FUNCTION (this);
  Ptr<QueueDiscItem> item = GetInternalQueue (0)->Dequeue ();
  if (item)
    {
      if (m_pool)
        {
          m_pool->Release (m_egressId, item->GetSize ());
        }
      MaybeSignalResume (GetItemSourceId (item));
      m_queueStateTrace (m_egressId,
                         GetNPackets (),
                         GetNBytes (),
                         GetSharedBytes (),
                         false);
    }
  return item;
}

Ptr<const QueueDiscItem>
SwitchSharedBufferQueueDisc::DoPeek (void)
{
  NS_LOG_FUNCTION (this);
  return GetInternalQueue (0)->Peek ();
}

bool
SwitchSharedBufferQueueDisc::CheckConfig (void)
{
  NS_LOG_FUNCTION (this);

  if (GetNQueueDiscClasses () > 0)
    {
      NS_LOG_ERROR ("SwitchSharedBufferQueueDisc cannot have classes");
      return false;
    }

  if (GetNPacketFilters () > 0)
    {
      NS_LOG_ERROR ("SwitchSharedBufferQueueDisc needs no packet filters");
      return false;
    }

  if (GetNInternalQueues () == 0)
    {
      AddInternalQueue (CreateObjectWithAttributes<DropTailQueue<QueueDiscItem>> (
                          "MaxSize", QueueSizeValue (GetMaxSize ())));
    }

  if (GetNInternalQueues () != 1)
    {
      NS_LOG_ERROR ("SwitchSharedBufferQueueDisc needs 1 internal queue");
      return false;
    }

  if (m_kmax < m_kmin)
    {
      NS_LOG_ERROR ("Kmax must be greater than or equal to Kmin");
      return false;
    }

  if (m_resumeThreshold > m_pauseThreshold)
    {
      NS_LOG_ERROR ("ResumeThreshold must not exceed PauseThreshold");
      return false;
    }

  return true;
}

void
SwitchSharedBufferQueueDisc::InitializeParams (void)
{
  NS_LOG_FUNCTION (this);
}

uint32_t
SwitchSharedBufferQueueDisc::GetItemSourceId (Ptr<const QueueDiscItem> item) const
{
  SwitchFlowIdTag tag;
  if (item && item->GetPacket ()->PeekPacketTag (tag))
    {
      return tag.GetSourceId ();
    }
  return 0;
}

uint32_t
SwitchSharedBufferQueueDisc::GetItemFlowId (Ptr<const QueueDiscItem> item) const
{
  SwitchFlowIdTag tag;
  if (item && item->GetPacket ()->PeekPacketTag (tag))
    {
      return tag.GetFlowId ();
    }
  return 0;
}

bool
SwitchSharedBufferQueueDisc::ShouldMark (uint32_t occupancyPackets) const
{
  if (occupancyPackets < m_kmin)
    {
      return false;
    }

  if (m_kmax <= m_kmin)
    {
      return occupancyPackets >= m_kmin;
    }

  if (occupancyPackets >= m_kmax)
    {
      return m_uv->GetValue () <= m_pmax;
    }

  double fraction = static_cast<double> (occupancyPackets - m_kmin) /
                    static_cast<double> (m_kmax - m_kmin);
  double probability = fraction * m_pmax;
  return m_uv->GetValue () <= probability;
}

void
SwitchSharedBufferQueueDisc::MaybeSignalCongestion (Ptr<QueueDiscItem> item,
                                                    uint32_t occupancyPackets,
                                                    bool marked)
{
  uint32_t sourceId = GetItemSourceId (item);
  uint32_t flowId = GetItemFlowId (item);
  if (m_usePfc && occupancyPackets >= ComputePauseThreshold () && !m_pausedSources[sourceId])
    {
      m_pausedSources[sourceId] = true;
      m_pfcTrace (m_egressId, sourceId, occupancyPackets, 1);
      if (!m_pauseCallback.IsNull ())
        {
          m_pauseCallback (sourceId);
        }
    }

  if (!m_useQcn || (!marked && occupancyPackets < m_kmin))
    {
      return;
    }

  uint32_t& lastQcnPackets = m_lastQcnAtPackets[sourceId];
  if (lastQcnPackets == 0 ||
      occupancyPackets >= lastQcnPackets + m_qcnIntervalPackets ||
      occupancyPackets < lastQcnPackets)
    {
      lastQcnPackets = occupancyPackets;
      m_qcnTrace (m_egressId, sourceId, flowId, occupancyPackets, m_qcnMdFactor);
      if (!m_qcnCallback.IsNull ())
        {
          m_qcnCallback (sourceId, flowId, m_qcnMdFactor);
        }
    }
}

void
SwitchSharedBufferQueueDisc::MaybeSignalResume (uint32_t sourceId)
{
  if (!m_usePfc)
    {
      return;
    }

  auto it = m_pausedSources.find (sourceId);
  if (it == m_pausedSources.end () || !it->second)
    {
      return;
    }

  uint32_t occupancyPackets = GetNPackets ();
  if (occupancyPackets <= ComputeResumeThreshold ())
    {
      it->second = false;
      m_pfcTrace (m_egressId, sourceId, occupancyPackets, 0);
      if (!m_resumeCallback.IsNull ())
        {
          m_resumeCallback (sourceId);
        }
    }
}

uint32_t
SwitchSharedBufferQueueDisc::ComputePauseThreshold (void) const
{
  if (!m_useDynamicPfcThreshold || !m_pool)
    {
      return m_pauseThreshold;
    }

  uint64_t maxBytes = m_pool->GetMaxBytes ();
  uint64_t usedBytes = m_pool->GetUsedBytes ();
  uint64_t headroomBytes = maxBytes > usedBytes ? maxBytes - usedBytes : 0;
  uint64_t dynamicPackets =
      static_cast<uint64_t> (headroomBytes * m_dynamicPauseHeadroomFraction /
                             static_cast<double> (m_dynamicThresholdPacketBytes));
  uint32_t bounded = static_cast<uint32_t> (
      std::min<uint64_t> (m_pauseThreshold,
                          std::max<uint64_t> (m_minDynamicPauseThreshold, dynamicPackets)));
  return bounded;
}

uint32_t
SwitchSharedBufferQueueDisc::ComputeResumeThreshold (void) const
{
  uint32_t pause = ComputePauseThreshold ();
  return std::min (m_resumeThreshold, pause);
}

} // namespace ns3
