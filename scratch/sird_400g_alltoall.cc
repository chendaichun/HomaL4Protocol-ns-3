/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * 400G single-switch all-to-all scenario using the Homa/SIRD protocol path.
 *
 * This scenario intentionally does not configure PFC/QCN switch controls.  It
 * uses Homa sockets, optionally enables SIRD, and records both receiver-side
 * aggregate throughput and switch-side per-flow throughput.
 */

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/homa-header.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ppp-header.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("Sird400gAllToAll");

namespace {

struct FlowConfig
{
  uint32_t srcHost;
  uint32_t dstHost;
  uint64_t bytes;
  double startSec;
};

struct ExperimentStats
{
  uint32_t msgBeginCount = 0;
  uint32_t msgFinishCount = 0;
  uint64_t msgFinishBytes = 0;
  uint64_t dataRxBytes = 0;
  uint64_t switchTxPayloadBytes = 0;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> finishedBytesByFlow;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> dataBytesByFlow;
  std::map<std::pair<uint32_t, uint32_t>, uint64_t> switchBytesByFlow;
  std::map<uint32_t, uint64_t> dataRxBytesByReceiver;
};

static ExperimentStats g_stats;
static std::map<uint32_t, uint32_t> g_receiverIndexByIpv4;
static std::map<uint32_t, uint32_t> g_hostIndexByIpv4;
static std::map<std::string, uint64_t> g_throughputBytes;
static std::map<std::string, uint64_t> g_throughputPrevBytes;
static std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_switchFlowBytes;
static std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_switchFlowPrevBytes;
static uint32_t g_receiverCount = 4;

uint32_t
OriginalHostId (uint32_t zeroBasedHost)
{
  return zeroBasedHost + 1;
}

uint16_t
RoundPackets (double value)
{
  return static_cast<uint16_t> (std::max (1.0, std::round (value)));
}

std::string
GbpsString (double value)
{
  std::ostringstream out;
  out << value << "Gbps";
  return out.str ();
}

std::string
FlowSeriesName (uint32_t zeroBasedSrc, uint32_t zeroBasedDst)
{
  std::ostringstream label;
  label << OriginalHostId (zeroBasedSrc) << "->" << OriginalHostId (zeroBasedDst);
  return label.str ();
}

void
DrainSocket (Ptr<Socket> socket)
{
  Address from;
  while (socket->RecvFrom (from))
    {
      // Homa delivers complete messages here; the throughput curve is sampled
      // from DATA packet arrivals before this callback, so we only drain.
    }
}

void
SendOneShot (Ptr<Socket> socket, InetSocketAddress dst, uint32_t msgSizeBytes)
{
  socket->SendTo (Create<Packet> (msgSizeBytes), 0, dst);
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
  g_stats.msgBeginCount++;
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
  g_stats.msgFinishCount++;
  g_stats.msgFinishBytes += msg->GetSize ();
  auto srcIt = g_hostIndexByIpv4.find (saddr.Get ());
  auto dstIt = g_hostIndexByIpv4.find (daddr.Get ());
  if (srcIt != g_hostIndexByIpv4.end () && dstIt != g_hostIndexByIpv4.end ())
    {
      g_stats.finishedBytesByFlow[std::make_pair (srcIt->second, dstIt->second)] += msg->GetSize ();
    }

  *stream->GetStream () << "- " << Simulator::Now ().GetNanoSeconds ()
                        << " " << msg->GetSize ()
                        << " " << saddr << ":" << sport
                        << " " << daddr << ":" << dport
                        << " " << txMsgId << std::endl;
}

void
TraceDataArrival (Ptr<const Packet> packet,
                  Ipv4Address saddr,
                  Ipv4Address daddr,
                  uint16_t sport,
                  uint16_t dport,
                  int txMsgId,
                  uint16_t pktOffset,
                  uint8_t prio)
{
  (void) saddr;
  (void) sport;
  (void) dport;
  (void) txMsgId;
  (void) pktOffset;
  (void) prio;

  uint64_t bytes = packet->GetSize ();
  if (bytes == 0)
    {
      return;
    }

  g_stats.dataRxBytes += bytes;
  g_throughputBytes["aggregate"] += bytes;

  auto srcIt = g_hostIndexByIpv4.find (saddr.Get ());
  auto dstIt = g_hostIndexByIpv4.find (daddr.Get ());
  if (srcIt != g_hostIndexByIpv4.end () && dstIt != g_hostIndexByIpv4.end ())
    {
      g_stats.dataBytesByFlow[std::make_pair (srcIt->second, dstIt->second)] += bytes;
    }

  auto it = g_receiverIndexByIpv4.find (daddr.Get ());
  if (it != g_receiverIndexByIpv4.end ())
    {
      std::ostringstream label;
      label << "receiver" << OriginalHostId (it->second);
      g_stats.dataRxBytesByReceiver[it->second] += bytes;
      g_throughputBytes[label.str ()] += bytes;
    }
}

void
TraceSwitchPhyTxBegin (Ptr<const Packet> packet)
{
  Ptr<Packet> copy = packet->Copy ();

  PppHeader pppHeader;
  copy->RemoveHeader (pppHeader);
  if (pppHeader.GetProtocol () != 0x0021)
    {
      return;
    }

  Ipv4Header ipv4Header;
  copy->RemoveHeader (ipv4Header);
  if (ipv4Header.GetProtocol () != HomaHeader::PROT_NUMBER)
    {
      return;
    }

  HomaHeader homaHeader;
  copy->RemoveHeader (homaHeader);
  if ((homaHeader.GetFlags () & HomaHeader::Flags_t::DATA) == 0 ||
      homaHeader.GetPayloadSize () == 0)
    {
      return;
    }

  auto srcIt = g_hostIndexByIpv4.find (ipv4Header.GetSource ().Get ());
  auto dstIt = g_hostIndexByIpv4.find (ipv4Header.GetDestination ().Get ());
  if (srcIt == g_hostIndexByIpv4.end () || dstIt == g_hostIndexByIpv4.end ())
    {
      return;
    }

  const auto flow = std::make_pair (srcIt->second, dstIt->second);
  uint64_t bytes = homaHeader.GetPayloadSize ();
  g_stats.switchTxPayloadBytes += bytes;
  g_stats.switchBytesByFlow[flow] += bytes;
  g_switchFlowBytes[flow] += bytes;
}

void
SampleThroughput (Ptr<OutputStreamWrapper> stream, Time interval, Time stopTime)
{
  std::vector<std::string> labels;
  labels.push_back ("aggregate");
  for (uint32_t i = 0; i < g_receiverCount; ++i)
    {
      std::ostringstream label;
      label << "receiver" << OriginalHostId (i);
      labels.push_back (label.str ());
    }

  for (const std::string& label : labels)
    {
      uint64_t totalBytes = g_throughputBytes[label];
      uint64_t prevBytes = g_throughputPrevBytes[label];
      uint64_t deltaBytes = totalBytes - prevBytes;
      g_throughputPrevBytes[label] = totalBytes;
      double instGbps = deltaBytes * 8.0 / interval.GetSeconds () / 1e9;
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " series=" << label
                            << " instGbps=" << instGbps
                            << " totalBytes=" << totalBytes
                            << std::endl;
    }

  uint64_t aggregateBytes = g_throughputBytes["aggregate"];
  uint64_t aggregatePrevBytes = g_throughputPrevBytes["average_receiver_base"];
  uint64_t aggregateDeltaBytes = aggregateBytes - aggregatePrevBytes;
  g_throughputPrevBytes["average_receiver_base"] = aggregateBytes;
  double avgReceiverGbps =
      aggregateDeltaBytes * 8.0 / interval.GetSeconds () / 1e9 / g_receiverCount;
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " series=average_receiver"
                        << " instGbps=" << avgReceiverGbps
                        << " totalBytes=" << aggregateBytes / g_receiverCount
                        << std::endl;

  if (Simulator::Now () + interval <= stopTime)
    {
      Simulator::Schedule (interval, &SampleThroughput, stream, interval, stopTime);
    }
}

void
SampleSwitchFlowThroughput (Ptr<OutputStreamWrapper> stream,
                            Time interval,
                            Time stopTime,
                            std::vector<FlowConfig> flows)
{
  for (const auto& flowConfig : flows)
    {
      const auto flow = std::make_pair (flowConfig.srcHost, flowConfig.dstHost);
      uint64_t totalBytes = g_switchFlowBytes[flow];
      uint64_t prevBytes = g_switchFlowPrevBytes[flow];
      uint64_t deltaBytes = totalBytes - prevBytes;
      g_switchFlowPrevBytes[flow] = totalBytes;

      double instGbps = deltaBytes * 8.0 / interval.GetSeconds () / 1e9;
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " series=" << FlowSeriesName (flowConfig.srcHost,
                                                             flowConfig.dstHost)
                            << " srcHost=" << OriginalHostId (flowConfig.srcHost)
                            << " dstHost=" << OriginalHostId (flowConfig.dstHost)
                            << " instGbps=" << instGbps
                            << " totalBytes=" << totalBytes
                            << std::endl;
    }

  if (Simulator::Now () + interval <= stopTime)
    {
      Simulator::Schedule (interval,
                           &SampleSwitchFlowThroughput,
                           stream,
                           interval,
                           stopTime,
                           flows);
    }
}

void
WriteSummary (const std::string& path,
              const std::vector<FlowConfig>& flows,
              double linkRateGbps,
              const std::string& linkDelay,
              uint32_t deviceMtu,
              double stopTime,
              bool enableSird,
              bool useEcn,
              uint16_t bdpPkts,
              uint16_t sirdCreditBudgetPkts,
              uint16_t sirdUnschThresholdPkts)
{
  std::ofstream out (path.c_str ());
  uint64_t targetBytes = 0;
  for (const auto& flow : flows)
    {
      targetBytes += flow.bytes;
    }

  double dataGoodputGbps = g_stats.dataRxBytes * 8.0 / stopTime / 1e9;
  double finishedGoodputGbps = g_stats.msgFinishBytes * 8.0 / stopTime / 1e9;
  double switchPayloadGoodputGbps = g_stats.switchTxPayloadBytes * 8.0 / stopTime / 1e9;

  out << "linkRateGbps,linkDelay,deviceMtuBytes,stopTimeSec,enableSird,useEcn,"
         "bdpPkts,sirdCreditBudgetPkts,sirdUnschThresholdPkts\n";
  out << linkRateGbps << "," << linkDelay << "," << deviceMtu << ","
      << stopTime << "," << (enableSird ? 1 : 0) << "," << (useEcn ? 1 : 0)
      << "," << bdpPkts << "," << sirdCreditBudgetPkts << ","
      << sirdUnschThresholdPkts << "\n\n";

  out << "targetBytes,dataRxBytes,switchTxPayloadBytes,msgFinishBytes,msgBeginCount,msgFinishCount,"
         "dataCompletionRatio,switchCompletionRatio,msgCompletionRatio,dataGoodputGbps,"
         "switchPayloadGoodputGbps,finishedGoodputGbps\n";
  out << targetBytes << "," << g_stats.dataRxBytes << "," << g_stats.switchTxPayloadBytes
      << "," << g_stats.msgFinishBytes
      << "," << g_stats.msgBeginCount << "," << g_stats.msgFinishCount << ","
      << (targetBytes ? static_cast<double> (g_stats.dataRxBytes) / targetBytes : 0.0)
      << ","
      << (targetBytes ? static_cast<double> (g_stats.switchTxPayloadBytes) / targetBytes : 0.0)
      << ","
      << (targetBytes ? static_cast<double> (g_stats.msgFinishBytes) / targetBytes : 0.0)
      << "," << dataGoodputGbps << "," << switchPayloadGoodputGbps << ","
      << finishedGoodputGbps << "\n\n";

  out << "srcHost,dstHost,targetBytes,finishedBytes,completionRatio\n";
  for (const auto& flow : flows)
    {
      uint64_t finished = g_stats.finishedBytesByFlow[std::make_pair (flow.srcHost, flow.dstHost)];
      double completion = flow.bytes ? static_cast<double> (finished) / flow.bytes : 0.0;
      out << OriginalHostId (flow.srcHost) << ","
          << OriginalHostId (flow.dstHost) << ","
          << flow.bytes << ","
          << finished << ","
          << completion << "\n";
    }

  out << "\n";
  out << "srcHost,dstHost,dataRxBytes,dataRxRatio\n";
  for (const auto& flow : flows)
    {
      uint64_t dataBytes = g_stats.dataBytesByFlow[std::make_pair (flow.srcHost, flow.dstHost)];
      double ratio = flow.bytes ? static_cast<double> (dataBytes) / flow.bytes : 0.0;
      out << OriginalHostId (flow.srcHost) << ","
          << OriginalHostId (flow.dstHost) << ","
          << dataBytes << ","
          << ratio << "\n";
    }

  out << "\n";
  out << "srcHost,dstHost,switchTxPayloadBytes,switchTxRatio\n";
  for (const auto& flow : flows)
    {
      uint64_t switchBytes = g_stats.switchBytesByFlow[std::make_pair (flow.srcHost, flow.dstHost)];
      double ratio = flow.bytes ? static_cast<double> (switchBytes) / flow.bytes : 0.0;
      out << OriginalHostId (flow.srcHost) << ","
          << OriginalHostId (flow.dstHost) << ","
          << switchBytes << ","
          << ratio << "\n";
    }
}

} // namespace

int
main (int argc, char* argv[])
{
  std::string simTag = "sird";
  std::string outputDir = "outputs/sird-scenarios/sird_400g_alltoall";
  bool enableSird = true;
  bool useEcn = false;
  bool useSrrScheduling = true;
  double linkRateGbps = 400.0;
  std::string linkDelay = "4us";
  uint32_t deviceMtu = 4064;
  std::string deviceQueueMaxSize = "1p";
  std::string qdiscMaxSize = "100000p";
  double startSec = 40e-6;
  double stopTimeSec = 0.5;
  uint32_t msgSizeBytes = 100000000;
  uint32_t sampleIntervalUs = 100;
  double bdpPktsDouble = 200.0;
  uint16_t sirdSenderCsnThresholdPkts = 100;
  double sirdEcnMdFactor = 0.85;
  double sirdEcnAiStep = 1.0;
  double sirdSenderMdFactor = 0.8;
  double sirdSenderAiStep = 1.0;
  double sirdEcnAlphaGain = 0.125;

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output files", simTag);
  cmd.AddValue ("outputDir", "Output directory", outputDir);
  cmd.AddValue ("enableSird", "Enable SIRD control path", enableSird);
  cmd.AddValue ("useEcn", "Enable SirdQueueDisc ECN marking", useEcn);
  cmd.AddValue ("useSrrScheduling", "Use SRR receiver scheduling", useSrrScheduling);
  cmd.AddValue ("linkRateGbps", "Host-switch link rate in Gbps", linkRateGbps);
  cmd.AddValue ("linkDelay", "One-way host-switch delay", linkDelay);
  cmd.AddValue ("deviceMtu", "PointToPointNetDevice MTU", deviceMtu);
  cmd.AddValue ("deviceQueueMaxSize", "PointToPointNetDevice TxQueue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "SirdQueueDisc MaxSize", qdiscMaxSize);
  cmd.AddValue ("startSec", "Traffic start time", startSec);
  cmd.AddValue ("stopTime", "Simulation stop time", stopTimeSec);
  cmd.AddValue ("msgSizeBytes", "Message size for each all-to-all flow", msgSizeBytes);
  cmd.AddValue ("sampleIntervalUs", "Throughput sample interval", sampleIntervalUs);
  cmd.AddValue ("bdpPkts", "RTT BDP in packets", bdpPktsDouble);
  cmd.AddValue ("sirdSenderCsnThresholdPkts", "Sender CSN threshold in packets", sirdSenderCsnThresholdPkts);
  cmd.AddValue ("sirdEcnMdFactor", "SIRD ECN MD factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "SIRD ECN AI step", sirdEcnAiStep);
  cmd.AddValue ("sirdSenderMdFactor", "SIRD sender-feedback MD factor", sirdSenderMdFactor);
  cmd.AddValue ("sirdSenderAiStep", "SIRD sender-feedback AI step", sirdSenderAiStep);
  cmd.AddValue ("sirdEcnAlphaGain", "SIRD ECN EWMA gain", sirdEcnAlphaGain);
  cmd.Parse (argc, argv);

  Time::SetResolution (Time::NS);
  SeedManager::SetRun (1);

  uint16_t bdpPkts = RoundPackets (bdpPktsDouble);
  uint16_t sirdCreditBudgetPkts = RoundPackets (1.5 * bdpPktsDouble);
  uint16_t sirdUnschThresholdPkts = RoundPackets (bdpPktsDouble);

  Config::SetDefault ("ns3::Ipv4GlobalRouting::EcmpMode", EnumValue (Ipv4GlobalRouting::ECMP_RANDOM));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (bdpPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::NumTotalPrioBands", UintegerValue (8));
  Config::SetDefault ("ns3::HomaL4Protocol::NumUnschedPrioBands", UintegerValue (2));
  Config::SetDefault ("ns3::HomaL4Protocol::UseSrrScheduling", BooleanValue (useSrrScheduling));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts",
                      UintegerValue (sirdCreditBudgetPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts",
                      UintegerValue (sirdUnschThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold",
                      UintegerValue (sirdSenderCsnThresholdPkts));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderMdFactor", DoubleValue (sirdSenderMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderAiStep", DoubleValue (sirdSenderAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAlphaGain", DoubleValue (sirdEcnAlphaGain));

  NodeContainer hosts;
  hosts.Create (4);
  Ptr<Node> sw = CreateObject<Node> ();

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue (GbpsString (linkRateGbps)));
  p2p.SetDeviceAttribute ("Mtu", UintegerValue (deviceMtu));
  p2p.SetChannelAttribute ("Delay", StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  std::array<NetDeviceContainer, 4> links;
  for (uint32_t i = 0; i < links.size (); ++i)
    {
      links[i] = p2p.Install (sw, hosts.Get (i));
    }

  InternetStackHelper internet;
  internet.SetIpv6StackInstall (false);
  internet.Install (hosts);
  internet.Install (sw);

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SirdQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "UseEcn", BooleanValue (useEcn));
  for (uint32_t i = 0; i < links.size (); ++i)
    {
      tch.Install (links[i]);
    }

  Ipv4AddressHelper ipv4;
  std::array<Ipv4InterfaceContainer, 4> ifaces;
  std::array<Ipv4Address, 4> hostIps;
  for (uint32_t i = 0; i < links.size (); ++i)
    {
      std::ostringstream subnet;
      subnet << "10.70." << (i + 1) << ".0";
      ipv4.SetBase (subnet.str ().c_str (), "255.255.255.0");
      ifaces[i] = ipv4.Assign (links[i]);
      hostIps[i] = ifaces[i].GetAddress (1);
      g_receiverIndexByIpv4[hostIps[i].Get ()] = i;
      g_hostIndexByIpv4[hostIps[i].Get ()] = i;
    }
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  std::vector<FlowConfig> flows;
  for (uint32_t src = 0; src < 4; ++src)
    {
      for (uint32_t dst = 0; dst < 4; ++dst)
        {
          if (src == dst)
            {
              continue;
            }
          flows.push_back ({src, dst, msgSizeBytes, startSec});
        }
    }

  std::system (("mkdir -p " + outputDir).c_str ());
  AsciiTraceHelper ascii;
  std::ostringstream prefix;
  prefix << outputDir << "/sird400g_" << simTag;

  Ptr<OutputStreamWrapper> msgStream = ascii.CreateFileStream (prefix.str () + ".msg.tr");
  Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgBegin",
                                 MakeBoundCallback (&TraceMsgBegin, msgStream));
  Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                 MakeBoundCallback (&TraceMsgFinish, msgStream));
  Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/DataPktArrival",
                                 MakeCallback (&TraceDataArrival));
  for (uint32_t i = 0; i < links.size (); ++i)
    {
      links[i].Get (0)->TraceConnectWithoutContext ("PhyTxBegin",
                                                    MakeCallback (&TraceSwitchPhyTxBegin));
    }

  Ptr<OutputStreamWrapper> throughputStream =
      ascii.CreateFileStream (prefix.str () + ".throughput.tr");
  Simulator::Schedule (Seconds (startSec) + MicroSeconds (sampleIntervalUs),
                       &SampleThroughput,
                       throughputStream,
                       MicroSeconds (sampleIntervalUs),
                       Seconds (stopTimeSec));
  Ptr<OutputStreamWrapper> switchFlowThroughputStream =
      ascii.CreateFileStream (prefix.str () + ".switch-flow-throughput.tr");
  Simulator::Schedule (Seconds (startSec) + MicroSeconds (sampleIntervalUs),
                       &SampleSwitchFlowThroughput,
                       switchFlowThroughputStream,
                       MicroSeconds (sampleIntervalUs),
                       Seconds (stopTimeSec),
                       flows);

  std::array<Ptr<Socket>, 4> receiverSockets;
  std::array<InetSocketAddress, 4> receiverAddrs = {
    InetSocketAddress (hostIps[0], 30000),
    InetSocketAddress (hostIps[1], 30001),
    InetSocketAddress (hostIps[2], 30002),
    InetSocketAddress (hostIps[3], 30003),
  };
  for (uint32_t i = 0; i < receiverSockets.size (); ++i)
    {
      Ptr<SocketFactory> factory = hosts.Get (i)->GetObject<HomaSocketFactory> ();
      receiverSockets[i] = factory->CreateSocket ();
      receiverSockets[i]->Bind (receiverAddrs[i]);
      receiverSockets[i]->SetRecvCallback (MakeCallback (&DrainSocket));
    }

  std::array<Ptr<Socket>, 4> senderSockets;
  for (uint32_t i = 0; i < senderSockets.size (); ++i)
    {
      Ptr<SocketFactory> factory = hosts.Get (i)->GetObject<HomaSocketFactory> ();
      senderSockets[i] = factory->CreateSocket ();
      senderSockets[i]->Bind (InetSocketAddress (hostIps[i], static_cast<uint16_t> (20000 + i)));
    }

  for (const auto& flow : flows)
    {
      Simulator::Schedule (Seconds (flow.startSec),
                           &SendOneShot,
                           senderSockets[flow.srcHost],
                           receiverAddrs[flow.dstHost],
                           static_cast<uint32_t> (flow.bytes));
    }

  Simulator::Stop (Seconds (stopTimeSec));
  Simulator::Run ();

  std::ostringstream summaryPath;
  summaryPath << prefix.str () << ".summary.csv";
  WriteSummary (summaryPath.str (),
                flows,
                linkRateGbps,
                linkDelay,
                deviceMtu,
                stopTimeSec,
                enableSird,
                useEcn,
                bdpPkts,
                sirdCreditBudgetPkts,
                sirdUnschThresholdPkts);

  Simulator::Destroy ();
  return 0;
}
