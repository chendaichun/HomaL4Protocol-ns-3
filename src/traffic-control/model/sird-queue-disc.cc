/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "sird-queue-disc.h"

#include "ns3/drop-tail-queue.h"
#include "ns3/log.h"
#include "ns3/object-factory.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("SirdQueueDisc");

NS_OBJECT_ENSURE_REGISTERED (SirdQueueDisc);

TypeId
SirdQueueDisc::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SirdQueueDisc")
    .SetParent<QueueDisc> ()
    .SetGroupName ("TrafficControl")
    .AddConstructor<SirdQueueDisc> ()
    .AddAttribute ("MaxSize",
                   "The max queue size.",
                   QueueSizeValue (QueueSize ("1000p")),
                   MakeQueueSizeAccessor (&QueueDisc::SetMaxSize,
                                          &QueueDisc::GetMaxSize),
                   MakeQueueSizeChecker ())
    .AddAttribute ("MarkThreshold",
                   "Fixed queue occupancy threshold for ECN CE marking. "
                   "A zero-sized threshold disables marking.",
                   QueueSizeValue (QueueSize ("0p")),
                   MakeQueueSizeAccessor (&SirdQueueDisc::m_markThreshold),
                   MakeQueueSizeChecker ())
    .AddAttribute ("UseEcn",
                   "True to use ECN marking once the threshold is crossed.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&SirdQueueDisc::m_useEcn),
                   MakeBooleanChecker ())
  ;
  return tid;
}

SirdQueueDisc::SirdQueueDisc ()
  : QueueDisc (QueueDiscSizePolicy::SINGLE_INTERNAL_QUEUE)
{
  NS_LOG_FUNCTION (this);
}

SirdQueueDisc::~SirdQueueDisc ()
{
  NS_LOG_FUNCTION (this);
}

bool
SirdQueueDisc::DoEnqueue (Ptr<QueueDiscItem> item)
{
  NS_LOG_FUNCTION (this << item);

  QueueSize newSize = GetCurrentSize () + item;
  if (newSize > GetMaxSize ())
    {
      NS_LOG_LOGIC ("Queue full -- dropping packet");
      DropBeforeEnqueue (item, LIMIT_EXCEEDED_DROP);
      return false;
    }

  if (m_useEcn &&
      m_markThreshold.GetValue () > 0 &&
      newSize >= m_markThreshold)
    {
      if (Mark (item, THRESHOLD_MARK))
        {
          NS_LOG_LOGIC ("Marked packet due to threshold occupancy");
        }
    }

  return GetInternalQueue (0)->Enqueue (item);
}

Ptr<QueueDiscItem>
SirdQueueDisc::DoDequeue (void)
{
  NS_LOG_FUNCTION (this);
  return GetInternalQueue (0)->Dequeue ();
}

Ptr<const QueueDiscItem>
SirdQueueDisc::DoPeek (void)
{
  NS_LOG_FUNCTION (this);
  return GetInternalQueue (0)->Peek ();
}

bool
SirdQueueDisc::CheckConfig (void)
{
  NS_LOG_FUNCTION (this);

  if (GetNQueueDiscClasses () > 0)
    {
      NS_LOG_ERROR ("SirdQueueDisc cannot have classes");
      return false;
    }

  if (GetNPacketFilters () > 0)
    {
      NS_LOG_ERROR ("SirdQueueDisc needs no packet filters");
      return false;
    }

  if (m_markThreshold.GetValue () > 0 &&
      m_markThreshold.GetUnit () != GetMaxSize ().GetUnit ())
    {
      NS_LOG_ERROR ("MarkThreshold must use the same unit as MaxSize");
      return false;
    }

  if (GetNInternalQueues () == 0)
    {
      AddInternalQueue (CreateObjectWithAttributes<DropTailQueue<QueueDiscItem>> (
                          "MaxSize", QueueSizeValue (GetMaxSize ())));
    }

  if (GetNInternalQueues () != 1)
    {
      NS_LOG_ERROR ("SirdQueueDisc needs 1 internal queue");
      return false;
    }

  return true;
}

void
SirdQueueDisc::InitializeParams (void)
{
  NS_LOG_FUNCTION (this);
}

} // namespace ns3
