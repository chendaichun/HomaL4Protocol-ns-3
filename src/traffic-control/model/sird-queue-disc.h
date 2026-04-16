/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Simple threshold-based ECN marking queue disc.
 *
 * This queue disc is intended to approximate a paper-style switch behavior:
 * FIFO service, ECN marking once a fixed queue occupancy threshold is crossed,
 * and packet drops only when the queue limit is exceeded.
 */

#ifndef SIRD_QUEUE_DISC_H
#define SIRD_QUEUE_DISC_H

#include "ns3/queue-disc.h"

namespace ns3 {

class SirdQueueDisc : public QueueDisc
{
public:
  static TypeId GetTypeId (void);

  SirdQueueDisc ();
  ~SirdQueueDisc () override;

  static constexpr const char* LIMIT_EXCEEDED_DROP = "Queue disc limit exceeded";
  static constexpr const char* THRESHOLD_MARK = "ECN threshold exceeded";

private:
  bool DoEnqueue (Ptr<QueueDiscItem> item) override;
  Ptr<QueueDiscItem> DoDequeue (void) override;
  Ptr<const QueueDiscItem> DoPeek (void) override;
  bool CheckConfig (void) override;
  void InitializeParams (void) override;

  QueueSize m_markThreshold; //!< Fixed occupancy threshold for ECN marking
  bool m_useEcn;             //!< Whether ECN marking is enabled
};

} // namespace ns3

#endif /* SIRD_QUEUE_DISC_H */
