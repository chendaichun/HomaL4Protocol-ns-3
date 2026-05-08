/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * good3: core congestion / ECN feedback.
 *
 * A compact dumbbell topology replaces the large sim1 fabric:
 * 4 senders -> left switch -> shared core link -> right switch -> 4 receivers.
 * The shared core egress queue is ECN-marking, so the experiment can compare
 * normal ECN control with an effectively disabled ECN loop.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("Good3CoreCongestion");

namespace {

static uint64_t g_completedBytes = 0;

struct QueueTarget
{
  std::string label;
  Ptr<QueueDisc> queueDisc;
};

void
DrainSocket (Ptr<Socket> socket)
{
  Address from;
  while (socket->RecvFrom (from))
    {
    }
}

void
SendPeriodic (Ptr<Socket> socket,
              InetSocketAddress dst,
              uint32_t msgSizeBytes,
              Time interval,
              Time stopTime)
{
  if (Simulator::Now () >= stopTime)
    {
      return;
    }
  socket->SendTo (Create<Packet> (msgSizeBytes), 0, dst);
  Simulator::Schedule (interval, &SendPeriodic, socket, dst, msgSizeBytes, interval, stopTime);
}

void
CountMsgFinish (Ptr<const Packet> msg,
                Ipv4Address saddr,
                Ipv4Address daddr,
                uint16_t sport,
                uint16_t dport,
                int txMsgId)
{
  (void) saddr;
  (void) daddr;
  (void) sport;
  (void) dport;
  (void) txMsgId;
  g_completedBytes += msg->GetSize ();
}

void
SampleGoodput (Ptr<OutputStreamWrapper> stream, Time interval, uint64_t* lastBytes)
{
  uint64_t delta = g_completedBytes - *lastBytes;
  *lastBytes = g_completedBytes;
  double gbps = delta * 8.0 / interval.GetSeconds () / 1e9;
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " goodputGbps=" << gbps
                        << " completedBytes=" << g_completedBytes
                        << std::endl;
  Simulator::Schedule (interval, &SampleGoodput, stream, interval, lastBytes);
}

void
SampleQueues (Ptr<OutputStreamWrapper> stream,
              const std::vector<QueueTarget>* targets,
              Time interval)
{
  uint64_t maxPackets = 0;
  uint64_t totalPackets = 0;
  for (const auto& target : *targets)
    {
      uint64_t packets = target.queueDisc->GetNPackets ();
      uint64_t bytes = target.queueDisc->GetNBytes ();
      maxPackets = std::max (maxPackets, packets);
      totalPackets += packets;
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " queue=" << target.label
                            << " packets=" << packets
                            << " bytes=" << bytes
                            << std::endl;
    }
  double meanPackets = targets->empty () ? 0.0 :
    static_cast<double> (totalPackets) / targets->size ();
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " queue=aggregate"
                        << " maxPackets=" << maxPackets
                        << " meanPackets=" << meanPackets
                        << " numQueues=" << targets->size ()
                        << std::endl;
  Simulator::Schedule (interval, &SampleQueues, stream, targets, interval);
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
  std::string outputDir = "outputs/sird-scenarios/good3";
  std::string trafficConfig = "core";
  std::string workloadFile = "";
  bool enableSird = true;
  double offeredLoad = 1.0;
  double startSec = 0.2;
  double durationSec = 0.04;
  double settleTailSec = 0.08;
  bool traceMsg = false;
  bool traceTorQueue = true;
  bool traceGoodput = true;
  uint64_t queueSampleUs = 100;
  uint64_t goodputSampleUs = 100;
  std::string deviceQueueMaxSize = "1p";
  std::string qdiscMaxSize = "200p";
  std::string qdiscMarkThreshold = "";
  double sirdEcnMdFactor = 0.85;
  double sirdEcnAiStep = 1.0;
  double coreRateGbps = 20.0;
  uint32_t msgSizeBytes = 500000;
  double bdpPkts = 70.0;

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output files", simTag);
  cmd.AddValue ("outputDir", "Output directory", outputDir);
  cmd.AddValue ("trafficConfig", "Accepted for script compatibility", trafficConfig);
  cmd.AddValue ("workloadFile", "Accepted for script compatibility", workloadFile);
  cmd.AddValue ("enableSird", "Enable SIRD", enableSird);
  cmd.AddValue ("offeredLoad", "Per-sender offered load fraction of 100Gbps", offeredLoad);
  cmd.AddValue ("startSec", "Traffic start time", startSec);
  cmd.AddValue ("durationSec", "Traffic duration", durationSec);
  cmd.AddValue ("settleTailSec", "Drain time", settleTailSec);
  cmd.AddValue ("traceMsg", "Accepted for script compatibility", traceMsg);
  cmd.AddValue ("traceTorQueue", "Trace core queue", traceTorQueue);
  cmd.AddValue ("traceGoodput", "Trace aggregate goodput", traceGoodput);
  cmd.AddValue ("queueSampleUs", "Queue sample interval", queueSampleUs);
  cmd.AddValue ("goodputSampleUs", "Goodput sample interval", goodputSampleUs);
  cmd.AddValue ("deviceQueueMaxSize", "Device queue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "QueueDisc MaxSize", qdiscMaxSize);
  cmd.AddValue ("qdiscMarkThreshold", "ECN mark threshold", qdiscMarkThreshold);
  cmd.AddValue ("sirdEcnMdFactor", "ECN MD factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "ECN AI step", sirdEcnAiStep);
  cmd.AddValue ("coreRateGbps", "Shared core link rate", coreRateGbps);
  cmd.AddValue ("msgSizeBytes", "Message size", msgSizeBytes);
  cmd.AddValue ("bdpPkts", "BDP in packets", bdpPkts);
  cmd.Parse (argc, argv);

  if (qdiscMarkThreshold.empty ())
    {
      qdiscMarkThreshold = "10p";
    }

  Time::SetResolution (Time::NS);
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (RoundPackets (bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts",
                      UintegerValue (RoundPackets (1.5 * bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts",
                      UintegerValue (RoundPackets (bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));

  const uint32_t nPairs = 4;
  NodeContainer senders;
  NodeContainer receivers;
  senders.Create (nPairs);
  receivers.Create (nPairs);
  Ptr<Node> left = CreateObject<Node> ();
  Ptr<Node> right = CreateObject<Node> ();

  PointToPointHelper edge;
  edge.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  edge.SetChannelAttribute ("Delay", StringValue ("1us"));
  edge.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  PointToPointHelper core;
  {
    std::ostringstream rate;
    rate << coreRateGbps << "Gbps";
    core.SetDeviceAttribute ("DataRate", StringValue (rate.str ()));
  }
  core.SetChannelAttribute ("Delay", StringValue ("2us"));
  core.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  std::vector<NetDeviceContainer> senderLinks;
  std::vector<NetDeviceContainer> receiverLinks;
  for (uint32_t i = 0; i < nPairs; ++i)
    {
      senderLinks.push_back (edge.Install (senders.Get (i), left));
      receiverLinks.push_back (edge.Install (receivers.Get (i), right));
    }
  NetDeviceContainer coreLink = core.Install (left, right);

  InternetStackHelper stack;
  stack.Install (senders);
  stack.Install (receivers);
  stack.Install (left);
  stack.Install (right);

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SirdQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "MarkThreshold", StringValue (qdiscMarkThreshold),
                        "UseEcn", BooleanValue (true));
  for (auto& link : senderLinks)
    {
      tch.Install (link);
    }
  for (auto& link : receiverLinks)
    {
      tch.Install (link);
    }
  QueueDiscContainer coreQ = tch.Install (coreLink);

  Ipv4AddressHelper address;
  std::vector<Ipv4Address> senderIps (nPairs);
  std::vector<Ipv4Address> receiverIps (nPairs);
  address.SetBase ("10.43.0.0", "255.255.255.0");
  for (uint32_t i = 0; i < nPairs; ++i)
    {
      Ipv4InterfaceContainer ifs = address.Assign (senderLinks[i]);
      senderIps[i] = ifs.GetAddress (0);
      address.NewNetwork ();
    }
  for (uint32_t i = 0; i < nPairs; ++i)
    {
      Ipv4InterfaceContainer ifs = address.Assign (receiverLinks[i]);
      receiverIps[i] = ifs.GetAddress (0);
      address.NewNetwork ();
    }
  address.Assign (coreLink);
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  std::system (("mkdir -p " + outputDir).c_str ());
  AsciiTraceHelper ascii;
  std::ostringstream prefix;
  prefix << outputDir << "/sim1_" << simTag;

  Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                 MakeCallback (&CountMsgFinish));

  Time start = Seconds (startSec);
  Time stop = Seconds (startSec + durationSec);
  Time simStop = Seconds (startSec + durationSec + settleTailSec);

  std::vector<QueueTarget> queueTargets = {
    {"core_left_to_right", coreQ.Get (0)},
    {"core_right_to_left", coreQ.Get (1)}
  };
  if (traceTorQueue)
    {
      Ptr<OutputStreamWrapper> stream =
        ascii.CreateFileStream (prefix.str () + ".tor-egress-queue.tr");
      Simulator::Schedule (start, &SampleQueues, stream, &queueTargets,
                           MicroSeconds (queueSampleUs));
    }
  if (traceGoodput)
    {
      Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream (prefix.str () + ".goodput.tr");
      uint64_t* lastBytes = new uint64_t (0);
      Simulator::Schedule (start + MicroSeconds (goodputSampleUs),
                           &SampleGoodput,
                           stream,
                           MicroSeconds (goodputSampleUs),
                           lastBytes);
    }

  for (uint32_t i = 0; i < nPairs; ++i)
    {
      Ptr<SocketFactory> rFactory = receivers.Get (i)->GetObject<HomaSocketFactory> ();
      Ptr<Socket> rSocket = rFactory->CreateSocket ();
      InetSocketAddress rAddr (receiverIps[i], static_cast<uint16_t> (33000 + i));
      rSocket->Bind (rAddr);
      rSocket->SetRecvCallback (MakeCallback (&DrainSocket));

      Ptr<SocketFactory> sFactory = senders.Get (i)->GetObject<HomaSocketFactory> ();
      Ptr<Socket> sSocket = sFactory->CreateSocket ();
      sSocket->Bind (InetSocketAddress (senderIps[i], static_cast<uint16_t> (23000 + i)));
      double perSenderGbps = std::max (0.1, offeredLoad * 100.0);
      Time interval = Seconds (msgSizeBytes * 8.0 / (perSenderGbps * 1e9));
      Simulator::Schedule (start, &SendPeriodic, sSocket, rAddr, msgSizeBytes, interval, stop);
    }

  Simulator::Stop (simStop);
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}
