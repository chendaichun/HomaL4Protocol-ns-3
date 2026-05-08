/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * good4: mixed message size under receiver-side scheduling.
 *
 * Topology: 6 long-message senders, 1 probe sender, and 1 receiver
 * connected through one switch.  The same executable is run with 8B and
 * 500KB probes, then SRPT/SRR is compared for scheduled medium messages.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
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

NS_LOG_COMPONENT_DEFINE ("Good4MixedMessageSize");

namespace {

struct QueueTarget
{
  std::string label;
  Ptr<QueueDisc> queueDisc;
};

static std::unordered_map<std::string, uint64_t> g_linkRxBytes;
static std::unordered_map<std::string, uint64_t> g_linkRxBytesPrev;

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
              Time stopTime,
              uint32_t remaining)
{
  if (Simulator::Now () >= stopTime || remaining == 0)
    {
      return;
    }
  socket->SendTo (Create<Packet> (msgSizeBytes), 0, dst);
  Simulator::Schedule (interval,
                       &SendPeriodic,
                       socket,
                       dst,
                       msgSizeBytes,
                       interval,
                       stopTime,
                       remaining == 0xffffffffu ? remaining : remaining - 1);
}

void
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

void
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
}

void
TraceLinkRx (std::string label, Ptr<const Packet> packet)
{
  g_linkRxBytes[label] += packet->GetSize ();
}

void
SampleLinkThroughput (Ptr<OutputStreamWrapper> stream, Time interval)
{
  for (const auto& item : g_linkRxBytes)
    {
      uint64_t prev = g_linkRxBytesPrev[item.first];
      uint64_t delta = item.second - prev;
      g_linkRxBytesPrev[item.first] = item.second;
      double gbps = delta * 8.0 / interval.GetSeconds () / 1e9;
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " link=" << item.first
                            << " instGbps=" << gbps
                            << " totalBytes=" << item.second
                            << std::endl;
    }
  Simulator::Schedule (interval, &SampleLinkThroughput, stream, interval);
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
  std::string outputDir = "outputs/sird-scenarios/good4";
  bool enableSird = true;
  double startSec = 0.2;
  double durationSec = 0.003;
  double settleTailSec = 0.01;
  uint32_t longMsgSizeBytes = 10000000;
  double longSenderRateGbps = 16.67;
  uint32_t shortMsgSizeBytes = 8;
  uint64_t shortIntervalUs = 100;
  uint32_t targetProbeMessages = 400;
  bool enableBackgroundTraffic = true;
  bool useSrrScheduling = false;
  bool traceMsg = true;
  bool traceSwitchEgressQueue = true;
  bool traceLinkThroughput = true;
  uint64_t switchQueueSampleUs = 100;
  double bdpPkts = 33.32;
  std::string deviceQueueMaxSize = "1000p";
  std::string qdiscMaxSize = "1000p";

  bool tracePathRtt = false;
  bool traceSirdCredit = false;
  bool traceSirdLoop = false;
  bool showProgressBar = false;

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output files", simTag);
  cmd.AddValue ("outputDir", "Output directory", outputDir);
  cmd.AddValue ("enableSird", "Enable SIRD controls", enableSird);
  cmd.AddValue ("startSec", "Traffic start time", startSec);
  cmd.AddValue ("durationSec", "Traffic duration", durationSec);
  cmd.AddValue ("settleTailSec", "Drain time after traffic stops", settleTailSec);
  cmd.AddValue ("longMsgSizeBytes", "Background message size", longMsgSizeBytes);
  cmd.AddValue ("longSenderRateGbps", "Per-background-sender offered load", longSenderRateGbps);
  cmd.AddValue ("shortMsgSizeBytes", "Probe message size", shortMsgSizeBytes);
  cmd.AddValue ("shortIntervalUs", "Probe send interval", shortIntervalUs);
  cmd.AddValue ("targetProbeMessages", "Maximum probe messages", targetProbeMessages);
  cmd.AddValue ("enableBackgroundTraffic", "Enable incast background traffic", enableBackgroundTraffic);
  cmd.AddValue ("useSrrScheduling", "Use SRR instead of SRPT", useSrrScheduling);
  cmd.AddValue ("traceMsg", "Trace message begin/finish", traceMsg);
  cmd.AddValue ("tracePathRtt", "Accepted for script compatibility", tracePathRtt);
  cmd.AddValue ("traceSwitchEgressQueue", "Trace switch egress queue", traceSwitchEgressQueue);
  cmd.AddValue ("switchQueueSampleUs", "Queue sample interval", switchQueueSampleUs);
  cmd.AddValue ("traceLinkThroughput", "Trace link throughput", traceLinkThroughput);
  cmd.AddValue ("traceSirdCredit", "Accepted for script compatibility", traceSirdCredit);
  cmd.AddValue ("traceSirdLoop", "Accepted for script compatibility", traceSirdLoop);
  cmd.AddValue ("bdpPkts", "BDP in packets", bdpPkts);
  cmd.AddValue ("deviceQueueMaxSize", "Device queue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "QueueDisc MaxSize", qdiscMaxSize);
  cmd.AddValue ("showProgressBar", "Accepted for script compatibility", showProgressBar);
  cmd.Parse (argc, argv);

  Time::SetResolution (Time::NS);
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (RoundPackets (bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts",
                      UintegerValue (RoundPackets (1.5 * bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts",
                      UintegerValue (RoundPackets (bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold",
                      UintegerValue (RoundPackets (0.5 * bdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::UseSrrScheduling", BooleanValue (useSrrScheduling));

  const uint32_t nLongSenders = 6;
  const uint32_t probeSenderIdx = nLongSenders;
  const uint32_t receiverIdx = nLongSenders + 1;
  const uint32_t nHosts = nLongSenders + 2;

  NodeContainer hosts;
  hosts.Create (nHosts);
  Ptr<Node> sw = CreateObject<Node> ();

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("1us"));
  p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  std::vector<NetDeviceContainer> links;
  for (uint32_t i = 0; i < nHosts; ++i)
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
  address.SetBase ("10.41.0.0", "255.255.255.0");
  std::vector<Ipv4Address> hostIps (nHosts);
  for (uint32_t i = 0; i < nHosts; ++i)
    {
      Ipv4InterfaceContainer ifs = address.Assign (links[i]);
      hostIps[i] = ifs.GetAddress (0);
      address.NewNetwork ();
    }
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  std::system (("mkdir -p " + outputDir).c_str ());
  AsciiTraceHelper ascii;
  std::ostringstream prefix;
  prefix << outputDir << "/lab1_" << simTag;

  if (traceMsg)
    {
      Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream (prefix.str () + ".msg.tr");
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgBegin",
                                     MakeBoundCallback (&TraceMsgBegin, stream));
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                     MakeBoundCallback (&TraceMsgFinish, stream));
    }

  std::vector<QueueTarget> queueTargets;
  for (uint32_t i = 0; i < nHosts; ++i)
    {
      std::ostringstream label;
      if (i == receiverIdx)
        {
          label << "switch_port_to_receiver";
        }
      else
        {
          label << "switch_port_to_host" << i;
        }
      queueTargets.push_back ({label.str (), qdiscs[i].Get (1)});

      std::ostringstream linkLabel;
      linkLabel << "switch_to_host" << i;
      DynamicCast<PointToPointNetDevice> (links[i].Get (0))
        ->TraceConnectWithoutContext ("MacRx", MakeBoundCallback (&TraceLinkRx, linkLabel.str ()));
    }

  Time start = Seconds (startSec);
  Time stop = Seconds (startSec + durationSec);
  Time simStop = Seconds (startSec + durationSec + settleTailSec);

  if (traceSwitchEgressQueue)
    {
      Ptr<OutputStreamWrapper> stream =
        ascii.CreateFileStream (prefix.str () + ".switch-egress-queue.tr");
      Simulator::Schedule (start, &SampleQueues, stream, &queueTargets, simStop,
                           MicroSeconds (switchQueueSampleUs));
    }
  if (traceLinkThroughput)
    {
      Ptr<OutputStreamWrapper> stream =
        ascii.CreateFileStream (prefix.str () + ".link-throughput.tr");
      Simulator::Schedule (start + MilliSeconds (1), &SampleLinkThroughput, stream,
                           MilliSeconds (1));
    }

  Ptr<SocketFactory> rFactory = hosts.Get (receiverIdx)->GetObject<HomaSocketFactory> ();
  Ptr<Socket> receiver = rFactory->CreateSocket ();
  InetSocketAddress receiverAddr (hostIps[receiverIdx], 30000);
  receiver->Bind (receiverAddr);
  receiver->SetRecvCallback (MakeCallback (&DrainSocket));

  if (enableBackgroundTraffic)
    {
      Time longInterval = Seconds (longMsgSizeBytes * 8.0 / (longSenderRateGbps * 1e9));
      for (uint32_t i = 0; i < nLongSenders; ++i)
        {
          Ptr<SocketFactory> sFactory = hosts.Get (i)->GetObject<HomaSocketFactory> ();
          Ptr<Socket> socket = sFactory->CreateSocket ();
          socket->Bind (InetSocketAddress (hostIps[i], static_cast<uint16_t> (20000 + i)));
          Simulator::Schedule (start, &SendPeriodic, socket, receiverAddr, longMsgSizeBytes,
                               longInterval, stop, 0xffffffffu);
        }
    }

  Ptr<SocketFactory> pFactory = hosts.Get (probeSenderIdx)->GetObject<HomaSocketFactory> ();
  Ptr<Socket> probe = pFactory->CreateSocket ();
  probe->Bind (InetSocketAddress (hostIps[probeSenderIdx], 21000));
  Simulator::Schedule (start, &SendPeriodic, probe, receiverAddr, shortMsgSizeBytes,
                       MicroSeconds (shortIntervalUs), stop,
                       targetProbeMessages == 0 ? 0xffffffffu : targetProbeMessages);

  Simulator::Stop (simStop);
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}
