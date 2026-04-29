/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/homa-header.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("HomaL4ProtocolBad2RttHeterogeneity");

struct CreditSampleState
{
  double bdpPkts = 1.0;
  uint32_t receiverIp = 0;
  std::vector<uint32_t> trackedSenders;
  std::map<std::tuple<uint32_t, uint32_t, uint16_t>, uint32_t> senderCredits;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> receiverAvail;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> senderAvail;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> senderBudget;
};

struct GoodputSampleState
{
  uint32_t receiverIp = 0;
  std::vector<uint32_t> trackedSenders;
  std::map<uint32_t, uint64_t> completedBytes;
  std::map<uint32_t, uint64_t> lastBytes;
};

struct SwitchEgressQueueTarget
{
  std::string label;
  Ptr<QueueDisc> queueDisc;
};

struct BackloggedFlowState
{
  Ptr<Socket> socket;
  InetSocketAddress dst;
  Ipv4Address sender;
  Ipv4Address receiver;
  uint32_t msgSizeBytes;
  Time stopTime;
  uint32_t targetInFlight;
  uint32_t inFlight;
  bool started;
};

static CreditSampleState g_creditSampleState;
static GoodputSampleState g_goodputSampleState;
static std::vector<BackloggedFlowState> g_backloggedFlows;

static bool
IsTrackedSender (uint32_t sender)
{
  return std::find (g_creditSampleState.trackedSenders.begin (),
                    g_creditSampleState.trackedSenders.end (),
                    sender) != g_creditSampleState.trackedSenders.end ();
}

static void
AppReceive (Ptr<Socket> receiverSocket)
{
  Address from;
  while (receiverSocket->RecvFrom (from))
    {
    }
}

static void
FillBackloggedFlow (BackloggedFlowState* flow)
{
  if (Simulator::Now () >= flow->stopTime)
    {
      return;
    }

  while (flow->inFlight < flow->targetInFlight)
    {
      int sentBytes = flow->socket->SendTo (Create<Packet> (flow->msgSizeBytes), 0, flow->dst);
      if (sentBytes <= 0)
        {
          return;
        }
      flow->inFlight++;
    }
}

static void
StartBackloggedFlow (BackloggedFlowState* flow)
{
  flow->started = true;
  FillBackloggedFlow (flow);
}

static void
BackloggedSenderCreditState (Ipv4Address sender,
                             Ipv4Address receiver,
                             uint16_t txMsgId,
                             uint32_t senderCreditPkts,
                             uint8_t eventType)
{
  (void) txMsgId;
  (void) senderCreditPkts;
  if (eventType != 4)
    {
      return;
    }

  for (auto& flow : g_backloggedFlows)
    {
      if (!flow.started || flow.sender != sender || flow.receiver != receiver)
        {
          continue;
        }
      if (flow.inFlight > 0)
        {
          flow.inFlight--;
        }
      FillBackloggedFlow (&flow);
    }
}

static void
TraceMsgBegin (Ptr<OutputStreamWrapper> stream,
               Ptr<const Packet> msg,
               Ipv4Address saddr,
               Ipv4Address daddr,
               uint16_t sport,
               uint16_t dport,
               int txMsgId)
{
  *stream->GetStream () << "+ " << Simulator::Now ().GetNanoSeconds ()
                        << " " << msg->GetSize ()
                        << " " << saddr << ":" << sport
                        << " " << daddr << ":" << dport
                        << " " << txMsgId << std::endl;
}

static void
UpdateGoodputOnMsgFinish (Ptr<const Packet> msg,
                          Ipv4Address saddr,
                          Ipv4Address daddr)
{
  if (daddr.Get () == g_goodputSampleState.receiverIp &&
      std::find (g_goodputSampleState.trackedSenders.begin (),
                 g_goodputSampleState.trackedSenders.end (),
                 saddr.Get ()) != g_goodputSampleState.trackedSenders.end ())
    {
      g_goodputSampleState.completedBytes[saddr.Get ()] += msg->GetSize ();
    }
}

static void
TraceMsgFinish (Ptr<OutputStreamWrapper> stream,
                Ptr<const Packet> msg,
                Ipv4Address saddr,
                Ipv4Address daddr,
                uint16_t sport,
                uint16_t dport,
                int txMsgId)
{
  *stream->GetStream () << "- " << Simulator::Now ().GetNanoSeconds ()
                        << " " << msg->GetSize ()
                        << " " << saddr << ":" << sport
                        << " " << daddr << ":" << dport
                        << " " << txMsgId << std::endl;

  UpdateGoodputOnMsgFinish (msg, saddr, daddr);
}

static void
TraceMsgFinishForGoodput (Ptr<const Packet> msg,
                          Ipv4Address saddr,
                          Ipv4Address daddr,
                          uint16_t sport,
                          uint16_t dport,
                          int txMsgId)
{
  (void) sport;
  (void) dport;
  (void) txMsgId;
  UpdateGoodputOnMsgFinish (msg, saddr, daddr);
}

static void
TraceSirdSenderCreditState (Ptr<OutputStreamWrapper> stream,
                            Ipv4Address sender,
                            Ipv4Address receiver,
                            uint16_t txMsgId,
                            uint32_t senderCreditPkts,
                            uint8_t eventType)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " sender=" << sender
                        << " receiver=" << receiver
                        << " txMsgId=" << txMsgId
                        << " senderCreditPkts=" << senderCreditPkts
                        << " eventType=" << static_cast<uint32_t> (eventType)
                        << std::endl;
}

static void
TraceSirdReceiverCreditState (Ptr<OutputStreamWrapper> stream,
                              Ipv4Address receiver,
                              Ipv4Address sender,
                              uint32_t receiverAvailPkts,
                              uint32_t receiverBudgetPkts,
                              uint32_t senderAvailPkts,
                              uint32_t senderBudgetPkts,
                              uint8_t eventType)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " receiver=" << receiver
                        << " sender=" << sender
                        << " receiverAvailPkts=" << receiverAvailPkts
                        << " receiverBudgetPkts=" << receiverBudgetPkts
                        << " senderAvailPkts=" << senderAvailPkts
                        << " senderBudgetPkts=" << senderBudgetPkts
                        << " eventType=" << static_cast<uint32_t> (eventType)
                        << std::endl;
}

static void
UpdateCreditSampleSenderState (Ipv4Address sender,
                               Ipv4Address receiver,
                               uint16_t txMsgId,
                               uint32_t senderCreditPkts,
                               uint8_t eventType)
{
  (void) eventType;
  if (!IsTrackedSender (sender.Get ()) || receiver.Get () != g_creditSampleState.receiverIp)
    {
      return;
    }

  auto key = std::make_tuple (sender.Get (), receiver.Get (), txMsgId);
  if (senderCreditPkts == 0)
    {
      g_creditSampleState.senderCredits.erase (key);
    }
  else
    {
      g_creditSampleState.senderCredits[key] = senderCreditPkts;
    }
}

static void
UpdateCreditSampleReceiverState (Ipv4Address receiver,
                                 Ipv4Address sender,
                                 uint32_t receiverAvailPkts,
                                 uint32_t receiverBudgetPkts,
                                 uint32_t senderAvailPkts,
                                 uint32_t senderBudgetPkts,
                                 uint8_t eventType)
{
  (void) receiverBudgetPkts;
  (void) eventType;
  if (!IsTrackedSender (sender.Get ()) || receiver.Get () != g_creditSampleState.receiverIp)
    {
      return;
    }

  g_creditSampleState.receiverAvail[std::make_pair (receiver.Get (), sender.Get ())] =
    receiverAvailPkts;
  g_creditSampleState.senderAvail[std::make_pair (receiver.Get (), sender.Get ())] =
    senderAvailPkts;
  g_creditSampleState.senderBudget[std::make_pair (receiver.Get (), sender.Get ())] =
    senderBudgetPkts;
}

static void
SampleCreditState (Ptr<OutputStreamWrapper> stream,
                   Time sampleInterval,
                   Time stopTime)
{
  uint32_t totalSenderCreditPkts = 0;
  uint32_t totalReceiverAvailPkts = 0;

  for (uint32_t sender : g_creditSampleState.trackedSenders)
    {
      uint32_t senderCreditPkts = 0;
      uint32_t receiverAvailPkts = 0;
      uint32_t senderAvailPkts = 0;
      uint32_t senderBudgetPkts = 0;

      for (const auto& kv : g_creditSampleState.senderCredits)
        {
          if (std::get<0> (kv.first) == sender)
            {
              senderCreditPkts += kv.second;
            }
        }

      auto receiverKey = std::make_pair (g_creditSampleState.receiverIp, sender);
      auto it = g_creditSampleState.receiverAvail.find (receiverKey);
      if (it != g_creditSampleState.receiverAvail.end ())
        {
          receiverAvailPkts = it->second;
        }
      auto senderAvailIt = g_creditSampleState.senderAvail.find (receiverKey);
      if (senderAvailIt != g_creditSampleState.senderAvail.end ())
        {
          senderAvailPkts = senderAvailIt->second;
        }
      auto senderBudgetIt = g_creditSampleState.senderBudget.find (receiverKey);
      if (senderBudgetIt != g_creditSampleState.senderBudget.end ())
        {
          senderBudgetPkts = senderBudgetIt->second;
        }

      totalSenderCreditPkts += senderCreditPkts;
      totalReceiverAvailPkts += receiverAvailPkts;

      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " sender=" << Ipv4Address (sender)
                            << " senderCreditPkts=" << senderCreditPkts
                            << " receiverAvailPkts=" << receiverAvailPkts
                            << " senderAvailPkts=" << senderAvailPkts
                            << " senderBudgetPkts=" << senderBudgetPkts
                            << " senderCreditXbdp="
                            << (static_cast<double> (senderCreditPkts) / g_creditSampleState.bdpPkts)
                            << " receiverAvailXbdp="
                            << (static_cast<double> (receiverAvailPkts) / g_creditSampleState.bdpPkts)
                            << std::endl;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " sender=aggregate"
                        << " senderCreditPkts=" << totalSenderCreditPkts
                        << " receiverAvailPkts=" << totalReceiverAvailPkts
                        << " senderCreditXbdp="
                        << (static_cast<double> (totalSenderCreditPkts) / g_creditSampleState.bdpPkts)
                        << " receiverAvailXbdp="
                        << (static_cast<double> (totalReceiverAvailPkts) / g_creditSampleState.bdpPkts)
                        << std::endl;

  if (Simulator::Now () + sampleInterval <= stopTime)
    {
      Simulator::Schedule (sampleInterval,
                           &SampleCreditState,
                           stream,
                           sampleInterval,
                           stopTime);
    }
}

static void
SampleGoodputState (Ptr<OutputStreamWrapper> stream,
                    Time sampleInterval,
                    Time stopTime)
{
  double aggregateGbps = 0.0;

  for (uint32_t sender : g_goodputSampleState.trackedSenders)
    {
      uint64_t completedBytes = g_goodputSampleState.completedBytes[sender];
      uint64_t lastBytes = g_goodputSampleState.lastBytes[sender];
      uint64_t deltaBytes = completedBytes - lastBytes;
      g_goodputSampleState.lastBytes[sender] = completedBytes;

      double goodputGbps = (deltaBytes * 8.0) / sampleInterval.GetSeconds () / 1e9;
      aggregateGbps += goodputGbps;

      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " sender=" << Ipv4Address (sender)
                            << " goodputGbps=" << goodputGbps
                            << " completedBytes=" << completedBytes
                            << std::endl;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " sender=aggregate"
                        << " goodputGbps=" << aggregateGbps
                        << std::endl;

  if (Simulator::Now () + sampleInterval <= stopTime)
    {
      Simulator::Schedule (sampleInterval,
                           &SampleGoodputState,
                           stream,
                           sampleInterval,
                           stopTime);
    }
}

static void
TraceSwitchEgressQueueSample (Ptr<OutputStreamWrapper> stream,
                              const std::vector<SwitchEgressQueueTarget>* targets,
                              Time stopTime,
                              Time sampleInterval)
{
  for (const auto& target : *targets)
    {
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " queue=" << target.label
                            << " packets=" << target.queueDisc->GetNPackets ()
                            << " bytes=" << target.queueDisc->GetNBytes ()
                            << std::endl;
    }

  if (Simulator::Now () + sampleInterval <= stopTime)
    {
      Simulator::Schedule (sampleInterval,
                           &TraceSwitchEgressQueueSample,
                           stream,
                           targets,
                           stopTime,
                           sampleInterval);
    }
}

int
main (int argc, char* argv[])
{
  std::string simTag = "heterogeneous";
  std::string outputDir = "outputs/sird-scenarios/HomaL4Protocol-bad2-rtt-heterogeneity";

  double startSec = 0.2;
  double durationSec = 2.0;
  double settleTailSec = 0.1;
  uint32_t msgSizeBytes = 10000000;
  uint32_t backlogDepthMsgs = 2;

  double hostRateGbps = 100.0;
  double interSwitchRateGbps = 100.0;
  double bottleneckRateGbps = 40.0;
  double shortPathDelayUs = 0.5;
  double longPathExtraDelayUs = 8.0;

  bool enableSird = true;
  bool useSrrScheduling = false;
  bool traceMsg = false;
  bool traceProtocolCredit = true;
  bool traceCreditSample = true;
  bool traceGoodput = true;
  bool traceSwitchEgressQueue = true;
  uint64_t creditSampleUs = 100;
  uint64_t goodputSampleUs = 100;
  uint64_t switchQueueSampleUs = 100;

  double bdpPkts = 150.0;
  uint8_t numTotalPrioBands = 8;
  uint8_t numUnschedPrioBands = 2;

  double sirdEcnMdFactor = 0.85;
  double sirdEcnAiStep = 1.0;
  double sirdSenderMdFactor = 0.8;
  double sirdSenderAiStep = 1.0;
  double sirdEcnAlphaGain = 0.125;
  uint16_t sirdSenderCsnThresholdPkts = 0;
  uint64_t sirdSenderCreditLaunchDelayUs = 0;

  std::string deviceQueueMaxSize = "2000p";
  std::string qdiscMaxSize = "1000p";

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output trace files", simTag);
  cmd.AddValue ("outputDir", "Directory for output trace files", outputDir);
  cmd.AddValue ("startSec", "Start time of traffic generation", startSec);
  cmd.AddValue ("durationSec", "Traffic generation duration in seconds", durationSec);
  cmd.AddValue ("settleTailSec", "Tail time after traffic generation for draining in-flight packets", settleTailSec);
  cmd.AddValue ("msgSizeBytes", "Sender-to-receiver message size", msgSizeBytes);
  cmd.AddValue ("backlogDepthMsgs", "Target outstanding Homa messages per sender flow", backlogDepthMsgs);
  cmd.AddValue ("hostRateGbps", "Host-edge link rate in Gbps", hostRateGbps);
  cmd.AddValue ("interSwitchRateGbps", "Inter-switch link rate in Gbps", interSwitchRateGbps);
  cmd.AddValue ("bottleneckRateGbps", "Receiver-edge bottleneck link rate in Gbps", bottleneckRateGbps);
  cmd.AddValue ("shortPathDelayUs", "Base per-link propagation delay in microseconds", shortPathDelayUs);
  cmd.AddValue ("longPathExtraDelayUs", "Extra one-way delay added to sender-A path in microseconds", longPathExtraDelayUs);
  cmd.AddValue ("enableSird", "Enable SIRD control path", enableSird);
  cmd.AddValue ("useSrrScheduling", "Enable SRR receiver scheduling", useSrrScheduling);
  cmd.AddValue ("traceMsg", "Whether to trace message begin/finish events", traceMsg);
  cmd.AddValue ("traceProtocolCredit", "Whether to trace protocol-level sender/receiver credit state", traceProtocolCredit);
  cmd.AddValue ("traceCreditSample", "Whether to sample compact sender/receiver credit state over time", traceCreditSample);
  cmd.AddValue ("traceGoodput", "Whether to sample per-sender goodput over time", traceGoodput);
  cmd.AddValue ("traceSwitchEgressQueue", "Whether to sample switch egress queue occupancy", traceSwitchEgressQueue);
  cmd.AddValue ("creditSampleUs", "Credit state sampling period in microseconds", creditSampleUs);
  cmd.AddValue ("goodputSampleUs", "Goodput sampling period in microseconds", goodputSampleUs);
  cmd.AddValue ("switchQueueSampleUs", "Switch egress queue sampling interval in microseconds", switchQueueSampleUs);
  cmd.AddValue ("bdpPkts", "RTT BDP in packets; all SIRD/Homa thresholds are derived from it", bdpPkts);
  cmd.AddValue ("sirdEcnMdFactor", "SIRD ECN multiplicative decrease factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "SIRD ECN additive increase step", sirdEcnAiStep);
  cmd.AddValue ("sirdSenderMdFactor", "SIRD sender-feedback multiplicative decrease factor", sirdSenderMdFactor);
  cmd.AddValue ("sirdSenderAiStep", "SIRD sender-feedback additive increase step", sirdSenderAiStep);
  cmd.AddValue ("sirdEcnAlphaGain", "SIRD ECN EWMA gain", sirdEcnAlphaGain);
  cmd.AddValue ("sirdSenderCsnThresholdPkts", "Optional override for SIRD sender CSN threshold in packets; 0 means derive from bdpPkts", sirdSenderCsnThresholdPkts);
  cmd.AddValue ("sirdSenderCreditLaunchDelayUs", "Sender-side delay before scheduled credit can launch DATA", sirdSenderCreditLaunchDelayUs);
  cmd.AddValue ("deviceQueueMaxSize", "PointToPointNetDevice TxQueue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "SirdQueueDisc MaxSize", qdiscMaxSize);
  cmd.Parse (argc, argv);

  auto roundPackets = [] (double value) -> uint16_t {
    return static_cast<uint16_t> (std::max<long> (1, std::lround (value)));
  };
  uint32_t homaBdpPkts = roundPackets (bdpPkts);
  uint16_t sirdCreditBudgetPkts = roundPackets (1.5 * bdpPkts);
  uint16_t sirdUnschThresholdPkts = roundPackets (1.0 * bdpPkts);
  uint16_t derivedSirdSenderCsnThresholdPkts = roundPackets (0.5 * bdpPkts);
  if (sirdSenderCsnThresholdPkts == 0)
    {
      sirdSenderCsnThresholdPkts = derivedSirdSenderCsnThresholdPkts;
    }
  std::ostringstream qdiscMarkThresholdBuilder;
  qdiscMarkThresholdBuilder << roundPackets (1.25 * bdpPkts) << "p";
  std::string qdiscMarkThreshold = qdiscMarkThresholdBuilder.str ();

  const uint32_t senderAIdx = 0;
  const uint32_t senderBIdx = 1;
  const uint32_t receiverIdx = 2;

  Time::SetResolution (Time::NS);
  SeedManager::SetRun (1);

  Config::SetDefault ("ns3::Ipv4GlobalRouting::EcmpMode", EnumValue (Ipv4GlobalRouting::ECMP_RANDOM));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (homaBdpPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::NumTotalPrioBands", UintegerValue (numTotalPrioBands));
  Config::SetDefault ("ns3::HomaL4Protocol::NumUnschedPrioBands", UintegerValue (numUnschedPrioBands));
  Config::SetDefault ("ns3::HomaL4Protocol::UseSrrScheduling", BooleanValue (useSrrScheduling));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts", UintegerValue (sirdCreditBudgetPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts", UintegerValue (sirdUnschThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderMdFactor", DoubleValue (sirdSenderMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderAiStep", DoubleValue (sirdSenderAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAlphaGain", DoubleValue (sirdEcnAlphaGain));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold", UintegerValue (sirdSenderCsnThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCreditLaunchDelay",
                      TimeValue (MicroSeconds (sirdSenderCreditLaunchDelayUs)));

  NodeContainer hosts;
  hosts.Create (3);
  NodeContainer switches;
  switches.Create (2);

  PointToPointHelper access;
  {
    std::ostringstream rate;
    rate << hostRateGbps << "Gbps";
    access.SetDeviceAttribute ("DataRate", StringValue (rate.str ()));
  }
  {
    std::ostringstream delay;
    delay << shortPathDelayUs << "us";
    access.SetChannelAttribute ("Delay", StringValue (delay.str ()));
  }
  access.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  PointToPointHelper interSwitch;
  {
    std::ostringstream rate;
    rate << interSwitchRateGbps << "Gbps";
    interSwitch.SetDeviceAttribute ("DataRate", StringValue (rate.str ()));
  }
  {
    std::ostringstream delay;
    delay << shortPathDelayUs + longPathExtraDelayUs << "us";
    interSwitch.SetChannelAttribute ("Delay", StringValue (delay.str ()));
  }
  interSwitch.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  PointToPointHelper bottleneck;
  {
    std::ostringstream rate;
    rate << bottleneckRateGbps << "Gbps";
    bottleneck.SetDeviceAttribute ("DataRate", StringValue (rate.str ()));
  }
  {
    std::ostringstream delay;
    delay << shortPathDelayUs << "us";
    bottleneck.SetChannelAttribute ("Delay", StringValue (delay.str ()));
  }
  bottleneck.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  NetDeviceContainer senderALink = access.Install (hosts.Get (senderAIdx), switches.Get (0));
  NetDeviceContainer interSwitchLink = interSwitch.Install (switches.Get (0), switches.Get (1));
  NetDeviceContainer senderBLink = access.Install (hosts.Get (senderBIdx), switches.Get (1));
  NetDeviceContainer receiverLink = bottleneck.Install (hosts.Get (receiverIdx), switches.Get (1));

  InternetStackHelper stack;
  stack.Install (hosts);
  stack.Install (switches);

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SirdQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "MarkThreshold", StringValue (qdiscMarkThreshold),
                        "UseEcn", BooleanValue (true));

  QueueDiscContainer senderAQdisc = tch.Install (senderALink);
  QueueDiscContainer interSwitchQdisc = tch.Install (interSwitchLink);
  QueueDiscContainer senderBQdisc = tch.Install (senderBLink);
  QueueDiscContainer receiverQdisc = tch.Install (receiverLink);

  Ipv4AddressHelper address;
  address.SetBase ("10.20.0.0", "255.255.255.0");

  Ipv4InterfaceContainer senderAIf = address.Assign (senderALink);
  address.NewNetwork ();
  Ipv4InterfaceContainer interIf = address.Assign (interSwitchLink);
  address.NewNetwork ();
  Ipv4InterfaceContainer senderBIf = address.Assign (senderBLink);
  address.NewNetwork ();
  Ipv4InterfaceContainer receiverIf = address.Assign (receiverLink);

  Ipv4Address senderAIp = senderAIf.GetAddress (0);
  Ipv4Address senderBIp = senderBIf.GetAddress (0);
  Ipv4Address receiverIp = receiverIf.GetAddress (0);
  (void) interIf;

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  AsciiTraceHelper ascii;
  std::string mkdirCmd = "mkdir -p " + outputDir;
  std::system (mkdirCmd.c_str ());
  std::ostringstream prefix;
  prefix << outputDir << "/bad2_" << simTag;

  {
    std::ofstream summary (prefix.str () + ".summary.tr");
    summary << "simTag=" << simTag << "\n"
            << "topology=senderA_switch0_to_switch1_receiver_senderB_switch1_receiver\n"
            << "senderA=" << senderAIp << "\n"
            << "senderB=" << senderBIp << "\n"
            << "receiver=" << receiverIp << "\n"
            << "senderAPath=hostA-switch0-switch1-receiver\n"
            << "senderBPath=hostB-switch1-receiver\n"
            << "startSec=" << startSec << "\n"
            << "durationSec=" << durationSec << "\n"
            << "settleTailSec=" << settleTailSec << "\n"
            << "msgSizeBytes=" << msgSizeBytes << "\n"
            << "backlogDepthMsgs=" << backlogDepthMsgs << "\n"
            << "hostRateGbps=" << hostRateGbps << "\n"
            << "interSwitchRateGbps=" << interSwitchRateGbps << "\n"
            << "bottleneckRateGbps=" << bottleneckRateGbps << "\n"
            << "shortPathDelayUs=" << shortPathDelayUs << "\n"
            << "longPathExtraDelayUs=" << longPathExtraDelayUs << "\n"
            << "bdpPkts=" << bdpPkts << "\n"
            << "homaBdpPkts=" << homaBdpPkts << "\n"
            << "sirdCreditBudgetPkts=" << sirdCreditBudgetPkts << "\n"
            << "sirdUnschThresholdPkts=" << sirdUnschThresholdPkts << "\n"
            << "sirdSenderCsnThresholdPkts=" << sirdSenderCsnThresholdPkts << "\n"
            << "useSrrScheduling=" << useSrrScheduling << "\n"
            << "deviceQueueMaxSize=" << deviceQueueMaxSize << "\n"
            << "qdiscMaxSize=" << qdiscMaxSize << "\n";
  }

  if (traceMsg)
    {
      Ptr<OutputStreamWrapper> msgStream = ascii.CreateFileStream (prefix.str () + ".msg.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgBegin",
                                     MakeBoundCallback (&TraceMsgBegin, msgStream));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                     MakeBoundCallback (&TraceMsgFinish, msgStream));
    }
  else
    {
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                     MakeCallback (&TraceMsgFinishForGoodput));
    }

  if (enableSird && traceProtocolCredit)
    {
      Ptr<OutputStreamWrapper> senderCreditStream = ascii.CreateFileStream (prefix.str () + ".sender-credit.tr");
      Ptr<OutputStreamWrapper> receiverCreditStream = ascii.CreateFileStream (prefix.str () + ".receiver-credit.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdSenderCreditState",
                                     MakeBoundCallback (&TraceSirdSenderCreditState, senderCreditStream));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdReceiverCreditState",
                                     MakeBoundCallback (&TraceSirdReceiverCreditState, receiverCreditStream));
    }

  Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdSenderCreditState",
                                 MakeCallback (&BackloggedSenderCreditState));

  Time startTime = Seconds (startSec);
  Time stopTime = Seconds (startSec + durationSec);
  Time simStopTime = Seconds (startSec + durationSec + settleTailSec);

  if (enableSird && traceCreditSample)
    {
      g_creditSampleState = CreditSampleState ();
      g_creditSampleState.receiverIp = receiverIp.Get ();
      g_creditSampleState.bdpPkts = bdpPkts;
      g_creditSampleState.trackedSenders = {senderAIp.Get (), senderBIp.Get ()};

      const uint32_t receiverBudgetPkts = roundPackets (1.5 * bdpPkts);
      g_creditSampleState.receiverAvail[std::make_pair (receiverIp.Get (), senderAIp.Get ())] =
        receiverBudgetPkts;
      g_creditSampleState.receiverAvail[std::make_pair (receiverIp.Get (), senderBIp.Get ())] =
        receiverBudgetPkts;
      g_creditSampleState.senderAvail[std::make_pair (receiverIp.Get (), senderAIp.Get ())] =
        receiverBudgetPkts;
      g_creditSampleState.senderAvail[std::make_pair (receiverIp.Get (), senderBIp.Get ())] =
        receiverBudgetPkts;
      g_creditSampleState.senderBudget[std::make_pair (receiverIp.Get (), senderAIp.Get ())] =
        receiverBudgetPkts;
      g_creditSampleState.senderBudget[std::make_pair (receiverIp.Get (), senderBIp.Get ())] =
        receiverBudgetPkts;

      Ptr<OutputStreamWrapper> creditSampleStream =
        ascii.CreateFileStream (prefix.str () + ".credit-sample.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdSenderCreditState",
                                     MakeCallback (&UpdateCreditSampleSenderState));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdReceiverCreditState",
                                     MakeCallback (&UpdateCreditSampleReceiverState));
      Simulator::Schedule (startTime,
                           &SampleCreditState,
                           creditSampleStream,
                           MicroSeconds (creditSampleUs),
                           stopTime);
    }

  if (traceGoodput)
    {
      g_goodputSampleState = GoodputSampleState ();
      g_goodputSampleState.receiverIp = receiverIp.Get ();
      g_goodputSampleState.trackedSenders = {senderAIp.Get (), senderBIp.Get ()};
      g_goodputSampleState.completedBytes[senderAIp.Get ()] = 0;
      g_goodputSampleState.completedBytes[senderBIp.Get ()] = 0;
      g_goodputSampleState.lastBytes[senderAIp.Get ()] = 0;
      g_goodputSampleState.lastBytes[senderBIp.Get ()] = 0;

      Ptr<OutputStreamWrapper> goodputStream = ascii.CreateFileStream (prefix.str () + ".goodput.tr");
      Simulator::Schedule (startTime,
                           &SampleGoodputState,
                           goodputStream,
                           MicroSeconds (goodputSampleUs),
                           stopTime);
    }

  if (traceSwitchEgressQueue)
    {
      std::vector<SwitchEgressQueueTarget>* queueTargets = new std::vector<SwitchEgressQueueTarget> ();
      queueTargets->push_back ({"switch0_to_switch1", interSwitchQdisc.Get (0)});
      queueTargets->push_back ({"switch1_to_receiver", receiverQdisc.Get (1)});
      queueTargets->push_back ({"switch1_to_senderB", senderBQdisc.Get (1)});

      Ptr<OutputStreamWrapper> switchQueueStream =
        ascii.CreateFileStream (prefix.str () + ".switch-egress-queue.tr");
      Simulator::Schedule (startTime,
                           &TraceSwitchEgressQueueSample,
                           switchQueueStream,
                           queueTargets,
                           simStopTime,
                           MicroSeconds (switchQueueSampleUs));
    }

  Ptr<SocketFactory> receiverFactory = hosts.Get (receiverIdx)->GetObject<HomaSocketFactory> ();
  Ptr<Socket> receiverSock = receiverFactory->CreateSocket ();
  InetSocketAddress receiverAddr (receiverIp, 31000);
  receiverSock->Bind (receiverAddr);
  receiverSock->SetRecvCallback (MakeCallback (&AppReceive));

  g_backloggedFlows.clear ();
  g_backloggedFlows.reserve (2);

  Ptr<SocketFactory> senderAFactory = hosts.Get (senderAIdx)->GetObject<HomaSocketFactory> ();
  Ptr<Socket> senderASock = senderAFactory->CreateSocket ();
  senderASock->Bind (InetSocketAddress (senderAIp, 22000));
  g_backloggedFlows.push_back ({senderASock,
                                receiverAddr,
                                senderAIp,
                                receiverIp,
                                msgSizeBytes,
                                stopTime,
                                std::max<uint32_t> (1, backlogDepthMsgs),
                                0,
                                false});

  Ptr<SocketFactory> senderBFactory = hosts.Get (senderBIdx)->GetObject<HomaSocketFactory> ();
  Ptr<Socket> senderBSock = senderBFactory->CreateSocket ();
  senderBSock->Bind (InetSocketAddress (senderBIp, 22001));
  g_backloggedFlows.push_back ({senderBSock,
                                receiverAddr,
                                senderBIp,
                                receiverIp,
                                msgSizeBytes,
                                stopTime,
                                std::max<uint32_t> (1, backlogDepthMsgs),
                                0,
                                false});

  Simulator::Schedule (startTime, &StartBackloggedFlow, &g_backloggedFlows[0]);
  Simulator::Schedule (startTime, &StartBackloggedFlow, &g_backloggedFlows[1]);

  Simulator::Stop (simStopTime);
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}
