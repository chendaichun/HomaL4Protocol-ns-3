/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * good2: sender uplink bottleneck.
 *
 * One sender transmits backlogged 10MB messages to three receivers that join
 * at staggered times.  The experiment compares normal sender feedback with a
 * no-feedback approximation by changing S_Thr and sender AIMD parameters.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
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

NS_LOG_COMPONENT_DEFINE ("Good2SenderBottleneck");

namespace {

struct QueueTarget
{
  std::string label;
  Ptr<QueueDisc> queueDisc;
};

struct BackloggedFlow
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

struct CreditSampleState
{
  uint32_t targetSender = 0;
  double bdpPkts = 1.0;
  std::map<std::tuple<uint32_t, uint32_t, uint16_t>, uint32_t> senderCredits;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> receiverAvail;
};

static std::vector<BackloggedFlow> g_flows;
static CreditSampleState g_creditState;

void
DrainSocket (Ptr<Socket> socket)
{
  Address from;
  while (socket->RecvFrom (from))
    {
    }
}

void
FillFlow (BackloggedFlow* flow)
{
  if (Simulator::Now () >= flow->stopTime)
    {
      return;
    }
  while (flow->inFlight < flow->targetInFlight)
    {
      int sent = flow->socket->SendTo (Create<Packet> (flow->msgSizeBytes), 0, flow->dst);
      if (sent <= 0)
        {
          return;
        }
      flow->inFlight++;
    }
}

void
StartFlow (BackloggedFlow* flow)
{
  flow->started = true;
  FillFlow (flow);
}

void
SenderCreditForBacklog (Ipv4Address sender,
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
  for (auto& flow : g_flows)
    {
      if (!flow.started || flow.sender != sender || flow.receiver != receiver)
        {
          continue;
        }
      if (flow.inFlight > 0)
        {
          flow.inFlight--;
        }
      FillFlow (&flow);
    }
}

void
UpdateSenderCreditSample (Ipv4Address sender,
                          Ipv4Address receiver,
                          uint16_t txMsgId,
                          uint32_t senderCreditPkts,
                          uint8_t eventType)
{
  if (sender.Get () != g_creditState.targetSender)
    {
      return;
    }
  auto key = std::make_tuple (sender.Get (), receiver.Get (), txMsgId);
  if (eventType == 4)
    {
      g_creditState.senderCredits.erase (key);
    }
  else
    {
      g_creditState.senderCredits[key] = senderCreditPkts;
    }
}

void
UpdateReceiverCreditSample (Ipv4Address receiver,
                            Ipv4Address sender,
                            uint32_t receiverAvailPkts,
                            uint32_t receiverBudgetPkts,
                            uint32_t senderAvailPkts,
                            uint32_t senderBudgetPkts,
                            uint8_t eventType)
{
  (void) receiverBudgetPkts;
  (void) senderAvailPkts;
  (void) senderBudgetPkts;
  (void) eventType;
  if (sender.Get () != g_creditState.targetSender)
    {
      return;
    }
  g_creditState.receiverAvail[std::make_pair (receiver.Get (), sender.Get ())] =
    receiverAvailPkts;
}

void
SampleCreditState (Ptr<OutputStreamWrapper> stream, Time interval, Time stopTime)
{
  uint32_t senderCreditPkts = 0;
  for (const auto& item : g_creditState.senderCredits)
    {
      senderCreditPkts += item.second;
    }
  uint32_t receiverAvailPkts = 0;
  for (const auto& item : g_creditState.receiverAvail)
    {
      receiverAvailPkts += item.second;
    }
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " sender=" << Ipv4Address (g_creditState.targetSender)
                        << " senderCreditPkts=" << senderCreditPkts
                        << " senderCreditXbdp=" << senderCreditPkts / g_creditState.bdpPkts
                        << " receiverAvailPkts=" << receiverAvailPkts
                        << " receiverAvailXbdp=" << receiverAvailPkts / g_creditState.bdpPkts
                        << " receiverCount=" << g_creditState.receiverAvail.size ()
                        << std::endl;
  if (Simulator::Now () + interval <= stopTime)
    {
      Simulator::Schedule (interval, &SampleCreditState, stream, interval, stopTime);
    }
}

void
SampleQueues (Ptr<OutputStreamWrapper> stream,
              const std::vector<QueueTarget>* targets,
              Time stopTime,
              Time interval)
{
  for (const auto& target : *targets)
    {
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " queue=" << target.label
                            << " packets=" << target.queueDisc->GetNPackets ()
                            << " bytes=" << target.queueDisc->GetNBytes ()
                            << std::endl;
    }
  if (Simulator::Now () + interval <= stopTime)
    {
      Simulator::Schedule (interval, &SampleQueues, stream, targets, stopTime, interval);
    }
}

uint16_t
RoundPackets (double value)
{
  return static_cast<uint16_t> (std::max (1.0, std::round (value)));
}

} // namespace

int
main (int argc, char* argv[])
{
  std::string simTag = "default";
  std::string outputDir = "outputs/sird-scenarios/good2";
  double startSec = 0.2;
  double durationSec = 0.6;
  double settleTailSec = 0.1;
  uint64_t flowGapUs = 200000;
  uint64_t sendIntervalUs = 400;
  bool backloggedFlow = true;
  uint32_t backlogDepthMsgs = 2;
  uint32_t activeReceiverCount = 3;
  uint32_t msgSizeBytes = 10000000;
  bool enableSird = true;
  bool traceCreditSample = true;
  bool traceSwitchEgressQueue = true;
  uint64_t creditSampleUs = 100;
  uint64_t switchQueueSampleUs = 1000;
  double bdpPkts = 150.0;
  double sirdSenderMdFactor = 0.8;
  double sirdSenderAiStep = 1.0;
  double sirdEcnMdFactor = 0.85;
  double sirdEcnAiStep = 1.0;
  double sirdEcnAlphaGain = 0.125;
  uint16_t sirdSenderCsnThresholdPkts = 75;
  uint64_t sirdSenderCreditLaunchDelayUs = 0;
  bool useSrrScheduling = true;
  std::string deviceQueueMaxSize = "17p";
  std::string qdiscMaxSize = "1000p";

  bool traceMsg = false;
  bool traceProtocolCredit = false;
  bool traceSirdCredit = false;
  bool traceSirdBucket = false;
  bool traceCreditEvents = false;

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output files", simTag);
  cmd.AddValue ("outputDir", "Output directory", outputDir);
  cmd.AddValue ("startSec", "Traffic start time", startSec);
  cmd.AddValue ("durationSec", "Traffic duration", durationSec);
  cmd.AddValue ("settleTailSec", "Drain time", settleTailSec);
  cmd.AddValue ("flowGapUs", "Receiver flow start gap", flowGapUs);
  cmd.AddValue ("sendIntervalUs", "Periodic interval when not backlogged", sendIntervalUs);
  cmd.AddValue ("backloggedFlow", "Keep each sender-receiver flow backlogged", backloggedFlow);
  cmd.AddValue ("backlogDepthMsgs", "Target in-flight messages per flow", backlogDepthMsgs);
  cmd.AddValue ("activeReceiverCount", "Number of receivers", activeReceiverCount);
  cmd.AddValue ("msgSizeBytes", "Message size", msgSizeBytes);
  cmd.AddValue ("enableSird", "Enable SIRD", enableSird);
  cmd.AddValue ("traceMsg", "Accepted for script compatibility", traceMsg);
  cmd.AddValue ("traceProtocolCredit", "Accepted for script compatibility", traceProtocolCredit);
  cmd.AddValue ("traceCreditSample", "Trace compact credit samples", traceCreditSample);
  cmd.AddValue ("traceSirdCredit", "Accepted for script compatibility", traceSirdCredit);
  cmd.AddValue ("traceSirdBucket", "Accepted for script compatibility", traceSirdBucket);
  cmd.AddValue ("traceCreditEvents", "Accepted for script compatibility", traceCreditEvents);
  cmd.AddValue ("traceSwitchEgressQueue", "Trace switch egress queues", traceSwitchEgressQueue);
  cmd.AddValue ("creditSampleUs", "Credit sample interval", creditSampleUs);
  cmd.AddValue ("switchQueueSampleUs", "Queue sample interval", switchQueueSampleUs);
  cmd.AddValue ("bdpPkts", "BDP in packets", bdpPkts);
  cmd.AddValue ("sirdEcnMdFactor", "ECN MD factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "ECN AI step", sirdEcnAiStep);
  cmd.AddValue ("sirdSenderMdFactor", "Sender-feedback MD factor", sirdSenderMdFactor);
  cmd.AddValue ("sirdSenderAiStep", "Sender-feedback AI step", sirdSenderAiStep);
  cmd.AddValue ("sirdEcnAlphaGain", "ECN EWMA gain", sirdEcnAlphaGain);
  cmd.AddValue ("sirdSenderCsnThresholdPkts", "Sender CSN threshold", sirdSenderCsnThresholdPkts);
  cmd.AddValue ("sirdSenderCreditLaunchDelayUs", "Sender credit launch delay", sirdSenderCreditLaunchDelayUs);
  cmd.AddValue ("useSrrScheduling", "Use SRR scheduling", useSrrScheduling);
  cmd.AddValue ("deviceQueueMaxSize", "Device queue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "QueueDisc MaxSize", qdiscMaxSize);
  cmd.Parse (argc, argv);

  Time::SetResolution (Time::NS);
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (RoundPackets (bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts",
                      UintegerValue (RoundPackets (1.5 * bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts",
                      UintegerValue (RoundPackets (bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold",
                      UintegerValue (sirdSenderCsnThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderMdFactor", DoubleValue (sirdSenderMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderAiStep", DoubleValue (sirdSenderAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAlphaGain", DoubleValue (sirdEcnAlphaGain));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCreditLaunchDelay",
                      TimeValue (MicroSeconds (sirdSenderCreditLaunchDelayUs)));
  Config::SetDefault ("ns3::HomaL4Protocol::UseSrrScheduling", BooleanValue (useSrrScheduling));

  NodeContainer hosts;
  hosts.Create (4);
  Ptr<Node> sw = CreateObject<Node> ();
  const uint32_t senderIdx = 0;
  const std::vector<uint32_t> receiverIdx = {1, 2, 3};

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("1us"));
  p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  std::vector<NetDeviceContainer> links;
  for (uint32_t i = 0; i < 4; ++i)
    {
      links.push_back (p2p.Install (hosts.Get (i), sw));
    }

  InternetStackHelper stack;
  stack.Install (hosts);
  stack.Install (sw);

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SirdQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "UseEcn", BooleanValue (true));
  std::vector<QueueDiscContainer> qdiscs;
  for (auto& link : links)
    {
      qdiscs.push_back (tch.Install (link));
    }

  Ipv4AddressHelper address;
  address.SetBase ("10.42.0.0", "255.255.255.0");
  std::vector<Ipv4Address> hostIps (4);
  for (uint32_t i = 0; i < 4; ++i)
    {
      Ipv4InterfaceContainer ifs = address.Assign (links[i]);
      hostIps[i] = ifs.GetAddress (0);
      address.NewNetwork ();
    }
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  std::system (("mkdir -p " + outputDir).c_str ());
  AsciiTraceHelper ascii;
  std::ostringstream prefix;
  prefix << outputDir << "/lab2_" << simTag;

  std::vector<QueueTarget> queueTargets;
  queueTargets.push_back ({"switch_port_to_sender", qdiscs[senderIdx].Get (1)});
  for (uint32_t i = 0; i < receiverIdx.size (); ++i)
    {
      std::ostringstream label;
      label << "switch_port_to_receiver" << i;
      queueTargets.push_back ({label.str (), qdiscs[receiverIdx[i]].Get (1)});
    }

  Time start = Seconds (startSec);
  Time stop = Seconds (startSec + durationSec);
  Time simStop = Seconds (startSec + durationSec + settleTailSec);

  std::vector<InetSocketAddress> receiverAddrs;
  activeReceiverCount = std::min<uint32_t> (activeReceiverCount, receiverIdx.size ());
  for (uint32_t i = 0; i < activeReceiverCount; ++i)
    {
      uint32_t idx = receiverIdx[i];
      Ptr<SocketFactory> factory = hosts.Get (idx)->GetObject<HomaSocketFactory> ();
      Ptr<Socket> socket = factory->CreateSocket ();
      InetSocketAddress addr (hostIps[idx], static_cast<uint16_t> (31000 + i));
      socket->Bind (addr);
      socket->SetRecvCallback (MakeCallback (&DrainSocket));
      receiverAddrs.push_back (addr);
    }

  if (traceCreditSample)
    {
      g_creditState = CreditSampleState ();
      g_creditState.targetSender = hostIps[senderIdx].Get ();
      g_creditState.bdpPkts = bdpPkts;
      uint32_t receiverBudget = RoundPackets (1.5 * bdpPkts);
      for (const auto& addr : receiverAddrs)
        {
          g_creditState.receiverAvail[std::make_pair (addr.GetIpv4 ().Get (),
                                                      hostIps[senderIdx].Get ())] = receiverBudget;
        }
      Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream (prefix.str () + ".credit-sample.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdSenderCreditState",
                                     MakeCallback (&UpdateSenderCreditSample));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdReceiverCreditState",
                                     MakeCallback (&UpdateReceiverCreditSample));
      Simulator::Schedule (start, &SampleCreditState, stream, MicroSeconds (creditSampleUs), stop);
    }

  if (traceSwitchEgressQueue)
    {
      Ptr<OutputStreamWrapper> stream =
        ascii.CreateFileStream (prefix.str () + ".switch-egress-queue.tr");
      Simulator::Schedule (start, &SampleQueues, stream, &queueTargets, simStop,
                           MicroSeconds (switchQueueSampleUs));
    }

  Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/SirdSenderCreditState",
                                 MakeCallback (&SenderCreditForBacklog));

  Time interval = sendIntervalUs == 0 ?
    Seconds (msgSizeBytes * 8.0 / 100e9) : MicroSeconds (sendIntervalUs);
  (void) interval;
  g_flows.reserve (receiverAddrs.size ());
  for (uint32_t i = 0; i < receiverAddrs.size (); ++i)
    {
      Ptr<SocketFactory> factory = hosts.Get (senderIdx)->GetObject<HomaSocketFactory> ();
      Ptr<Socket> socket = factory->CreateSocket ();
      socket->Bind (InetSocketAddress (hostIps[senderIdx], static_cast<uint16_t> (22000 + i)));
      g_flows.push_back ({socket,
                          receiverAddrs[i],
                          hostIps[senderIdx],
                          receiverAddrs[i].GetIpv4 (),
                          msgSizeBytes,
                          stop,
                          backloggedFlow ? std::max<uint32_t> (1, backlogDepthMsgs) : 1,
                          0,
                          false});
      Simulator::Schedule (start + MicroSeconds (flowGapUs * i), &StartFlow, &g_flows.back ());
    }

  Simulator::Stop (simStop);
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}
