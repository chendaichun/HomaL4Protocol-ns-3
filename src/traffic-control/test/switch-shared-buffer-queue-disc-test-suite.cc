/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Test skeleton for the future switch shared-buffer queue disc.
 */

#include "ns3/test.h"
#include "ns3/switch-shared-buffer-queue-disc.h"
#include "ns3/switch-flow-id-tag.h"
#include "ns3/switch-shared-buffer-pool.h"
#include "ns3/packet.h"
#include "ns3/queue-disc.h"
#include "ns3/string.h"
#include "ns3/simulator.h"

using namespace ns3;

class SwitchSharedBufferQueueDiscTestItem : public QueueDiscItem
{
public:
  SwitchSharedBufferQueueDiscTestItem (Ptr<Packet> p, const Address& addr)
    : QueueDiscItem (p, addr, 0)
  {
  }

  ~SwitchSharedBufferQueueDiscTestItem () override
  {
  }

  void AddHeader (void) override
  {
  }

  bool Mark (void) override
  {
    return true;
  }
};

class SwitchSharedBufferQueueDiscBasicTestCase : public TestCase
{
public:
  SwitchSharedBufferQueueDiscBasicTestCase ()
    : TestCase ("Sanity check on the switch shared-buffer queue disc skeleton")
  {
  }

  void DoRun (void) override
  {
    Ptr<SwitchSharedBufferQueueDisc> queue = CreateObject<SwitchSharedBufferQueueDisc> ();

    NS_TEST_EXPECT_MSG_EQ (
        queue->SetAttributeFailSafe ("SharedBufferSize",
                                     QueueSizeValue (QueueSize ("82MB"))),
        true,
        "The queue disc should expose SharedBufferSize");
    NS_TEST_EXPECT_MSG_EQ (queue->SetAttributeFailSafe ("Kmin", UintegerValue (700)),
                           true,
                           "The queue disc should expose Kmin");
    NS_TEST_EXPECT_MSG_EQ (queue->SetAttributeFailSafe ("Kmax", UintegerValue (1600)),
                           true,
                           "The queue disc should expose Kmax");
    NS_TEST_EXPECT_MSG_EQ (queue->SetAttributeFailSafe ("Pmax", DoubleValue (0.2)),
                           true,
                           "The queue disc should expose Pmax");
  }
};

class SwitchSharedBufferQueueDiscControlTestCase : public TestCase
{
public:
  SwitchSharedBufferQueueDiscControlTestCase ()
    : TestCase ("Shared-buffer queue disc emits ECN, PFC, and QCN events"),
      m_pauseEvents (0),
      m_resumeEvents (0),
      m_qcnEvents (0)
  {
  }

  void DoRun (void) override
  {
    Ptr<SwitchSharedBufferQueueDisc> queue = CreateObject<SwitchSharedBufferQueueDisc> ();
    Ptr<SwitchSharedBufferPool> pool = CreateObject<SwitchSharedBufferPool> ();
    pool->SetMaxBytes (32000);
    queue->SetAttribute ("MaxSize", QueueSizeValue (QueueSize ("16p")));
    queue->SetAttribute ("Kmin", UintegerValue (2));
    queue->SetAttribute ("Kmax", UintegerValue (2));
    queue->SetAttribute ("Pmax", DoubleValue (1.0));
    queue->SetAttribute ("UseDynamicPfcThreshold", BooleanValue (false));
    queue->SetAttribute ("PauseThreshold", UintegerValue (3));
    queue->SetAttribute ("ResumeThreshold", UintegerValue (1));
    queue->SetAttribute ("QcnIntervalPackets", UintegerValue (1));
    queue->SetSharedBufferPool (pool);
    queue->Initialize ();

    queue->SetPauseCallback (
        MakeCallback (&SwitchSharedBufferQueueDiscControlTestCase::OnPause, this));
    queue->SetResumeCallback (
        MakeCallback (&SwitchSharedBufferQueueDiscControlTestCase::OnResume, this));
    queue->SetQcnCallback (
        MakeCallback (&SwitchSharedBufferQueueDiscControlTestCase::OnQcn, this));

    Address dest;
    for (uint32_t i = 0; i < 3; ++i)
      {
        Ptr<Packet> p = Create<Packet> (4000);
        p->AddPacketTag (SwitchFlowIdTag (42, 7));
        queue->Enqueue (Create<SwitchSharedBufferQueueDiscTestItem> (p, dest));
      }

    NS_TEST_EXPECT_MSG_EQ (m_pauseEvents, 1, "Queue should pause the congesting source");
    NS_TEST_EXPECT_MSG_GT (m_qcnEvents, 0, "Queue should emit QCN feedback above Kmin");
    NS_TEST_EXPECT_MSG_EQ (pool->GetUsedBytes (), static_cast<uint64_t> (12000),
                           "Shared pool should account for all queued bytes");

    queue->Dequeue ();
    queue->Dequeue ();
    NS_TEST_EXPECT_MSG_EQ (m_resumeEvents, 1, "Queue should resume below threshold");
    NS_TEST_EXPECT_MSG_EQ (pool->GetUsedBytes (), static_cast<uint64_t> (4000),
                           "Shared pool should release dequeued bytes");

    Simulator::Destroy ();
  }

private:
  void OnPause (uint32_t sourceId)
  {
    if (sourceId == 7)
      {
        ++m_pauseEvents;
      }
  }

  void OnResume (uint32_t sourceId)
  {
    if (sourceId == 7)
      {
        ++m_resumeEvents;
      }
  }

  void OnQcn (uint32_t sourceId, uint32_t flowId, double factor)
  {
    if (sourceId == 7 && flowId == 42 && factor == 0.5)
      {
        ++m_qcnEvents;
      }
  }

  uint32_t m_pauseEvents;
  uint32_t m_resumeEvents;
  uint32_t m_qcnEvents;
};

static class SwitchSharedBufferQueueDiscTestSuite : public TestSuite
{
public:
  SwitchSharedBufferQueueDiscTestSuite ()
    : TestSuite ("switch-shared-buffer-queue-disc", UNIT)
  {
    AddTestCase (new SwitchSharedBufferQueueDiscBasicTestCase (), TestCase::QUICK);
    AddTestCase (new SwitchSharedBufferQueueDiscControlTestCase (), TestCase::QUICK);
  }
} g_switchSharedBufferQueueDiscTestSuite;
