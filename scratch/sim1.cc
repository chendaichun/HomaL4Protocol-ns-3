/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Simulation 1 scaffold:
 * - 144 hosts
 * - 9 ToR switches, 16 hosts per ToR
 * - 4 Spine switches
 * - 100Gbps host-ToR
 * - 400Gbps ToR-Spine by default, or 200Gbps in core-overload mode
 *
 * Workloads are read from a file under inputs/.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/homa-header.h"
#include "ns3/internet-apps-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/queue.h"
#include "ns3/rpc-workload-app.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("HomaL4ProtocolSim1LargeLeafSpine");

struct WorkloadSpec
{
  double avgMsgSizePkts = 0.0;
  std::map<double, int> msgSizeCdf;
};

enum class MessageRole
{
  kRequest,
  kResponse,
  kOther
};

struct GoodputCounters
{
  uint64_t requestBytes = 0;
  uint64_t responseBytes = 0;
  uint64_t totalBytes = 0;
};

struct GoodputCheckpoint
{
  uint64_t requestBytes = 0;
  uint64_t responseBytes = 0;
  uint64_t totalBytes = 0;
};

static uint16_t g_rpcServerPort = 0;
static GoodputCounters g_goodputCounters;

struct QueueSampleTarget
{
  std::string label;
  std::string role;
  uint32_t torIdx = 0;
  int32_t hostIdx = -1;
  int32_t spineIdx = -1;
  Ptr<QueueDisc> queueDisc;
  Ptr<Queue<Packet>> deviceQueue;
  bool active = false;
  uint32_t peakCombinedBytes = 0;
  uint32_t peakCombinedPackets = 0;
  int64_t peakTimeNs = -1;
};

static std::vector<QueueSampleTarget> g_torEgressQueueTargets;
static Time g_queueActiveStart = Seconds (0);
static Time g_queueActiveEnd = Seconds (0);
static Time g_statsActiveStart = Seconds (0);
static Time g_statsActiveEnd = Seconds (0);
static bool g_torQueueIncludeDevice = false;

static bool
IsStatsWindowActive ()
{
  Time now = Simulator::Now ();
  return now >= g_statsActiveStart && now <= g_statsActiveEnd;
}

static MessageRole
ClassifyMessageRole (uint16_t sport, uint16_t dport)
{
  if (g_rpcServerPort == 0)
    {
      return MessageRole::kOther;
    }
  if (dport == g_rpcServerPort)
    {
      return MessageRole::kRequest;
    }
  if (sport == g_rpcServerPort)
    {
      return MessageRole::kResponse;
    }
  return MessageRole::kOther;
}

static const char*
MessageRoleToString (MessageRole role)
{
  switch (role)
    {
    case MessageRole::kRequest:
      return "request";
    case MessageRole::kResponse:
      return "response";
    case MessageRole::kOther:
    default:
      return "other";
    }
}

static uint32_t
GetTrackedQueueBytes (const QueueSampleTarget& target)
{
  uint32_t qdiscBytes = target.queueDisc ? target.queueDisc->GetNBytes () : 0;
  if (!g_torQueueIncludeDevice)
    {
      return qdiscBytes;
    }
  uint32_t deviceBytes = target.deviceQueue ? target.deviceQueue->GetNBytes () : 0;
  return qdiscBytes + deviceBytes;
}

static uint32_t
GetTrackedQueuePackets (const QueueSampleTarget& target)
{
  uint32_t qdiscPackets = target.queueDisc ? target.queueDisc->GetNPackets () : 0;
  if (!g_torQueueIncludeDevice)
    {
      return qdiscPackets;
    }
  uint32_t devicePackets = target.deviceQueue ? target.deviceQueue->GetNPackets () : 0;
  return qdiscPackets + devicePackets;
}

static void
UpdateQueuePeak (QueueSampleTarget& target)
{
  if (!target.active)
    {
      return;
    }

  Time now = Simulator::Now ();
  if (now < g_queueActiveStart || now > g_queueActiveEnd)
    {
      return;
    }

  uint32_t combinedBytes = GetTrackedQueueBytes (target);
  uint32_t combinedPackets = GetTrackedQueuePackets (target);

  if (combinedBytes >= target.peakCombinedBytes)
    {
      target.peakCombinedBytes = combinedBytes;
      target.peakCombinedPackets = combinedPackets;
      target.peakTimeNs = now.GetNanoSeconds ();
    }
}

static void
OnQueueDiscDepthChanged (std::size_t targetIdx, uint32_t oldValue, uint32_t newValue)
{
  (void) oldValue;
  (void) newValue;
  UpdateQueuePeak (g_torEgressQueueTargets[targetIdx]);
}

static void
OnDeviceQueueDepthChanged (std::size_t targetIdx, uint32_t oldValue, uint32_t newValue)
{
  (void) oldValue;
  (void) newValue;
  UpdateQueuePeak (g_torEgressQueueTargets[targetIdx]);
}

static void
AttachQueuePeakCallbacks ()
{
  for (std::size_t i = 0; i < g_torEgressQueueTargets.size (); ++i)
    {
      auto& target = g_torEgressQueueTargets[i];
      if (target.queueDisc)
        {
          target.queueDisc->TraceConnectWithoutContext ("BytesInQueue",
                                                        MakeBoundCallback (&OnQueueDiscDepthChanged, i));
        }
      if (target.deviceQueue)
        {
          target.deviceQueue->TraceConnectWithoutContext ("BytesInQueue",
                                                          MakeBoundCallback (&OnDeviceQueueDepthChanged, i));
        }
      UpdateQueuePeak (target);
    }
}

static void
ConfigureActiveTorQueueTargets (const std::string& trafficConfig, int32_t incastReceiver)
{
  for (auto& target : g_torEgressQueueTargets)
    {
      target.active = false;
    }

  if (trafficConfig == "balanced")
    {
      for (auto& target : g_torEgressQueueTargets)
        {
          target.active = (target.role == "tor_to_host");
        }
      return;
    }

  if (trafficConfig == "core")
    {
      for (auto& target : g_torEgressQueueTargets)
        {
          target.active = (target.role == "tor_to_spine");
        }
      return;
    }

  if (trafficConfig == "incast")
    {
      for (auto& target : g_torEgressQueueTargets)
        {
          target.active = (target.role == "tor_to_host" && target.hostIdx == incastReceiver);
        }
      return;
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
  if (!IsStatsWindowActive ())
    {
      return;
    }
  MessageRole role = ClassifyMessageRole (sport, dport);
  *stream->GetStream () << "+ " << Simulator::Now ().GetNanoSeconds ()
                        << " " << msg->GetSize ()
                        << " " << saddr << ":" << sport
                        << " " << daddr << ":" << dport
                        << " " << txMsgId
                        << " kind=" << MessageRoleToString (role)
                        << std::endl;
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
  MessageRole role = ClassifyMessageRole (sport, dport);
  if (IsStatsWindowActive ())
    {
      g_goodputCounters.totalBytes += msg->GetSize ();
      if (role == MessageRole::kRequest)
        {
          g_goodputCounters.requestBytes += msg->GetSize ();
        }
      else if (role == MessageRole::kResponse)
        {
          g_goodputCounters.responseBytes += msg->GetSize ();
        }
      if (stream)
        {
          *stream->GetStream () << "- " << Simulator::Now ().GetNanoSeconds ()
                                << " " << msg->GetSize ()
                                << " " << saddr << ":" << sport
                                << " " << daddr << ":" << dport
                                << " " << txMsgId
                                << " kind=" << MessageRoleToString (role)
                                << std::endl;
        }
    }
}

static void
SampleGoodput (Ptr<OutputStreamWrapper> stream,
               Time sampleInterval,
               GoodputCheckpoint* checkpoint,
               uint32_t numHosts)
{
  uint64_t deltaRequestBytes = g_goodputCounters.requestBytes - checkpoint->requestBytes;
  uint64_t deltaResponseBytes = g_goodputCounters.responseBytes - checkpoint->responseBytes;
  uint64_t deltaTotalBytes = g_goodputCounters.totalBytes - checkpoint->totalBytes;
  checkpoint->requestBytes = g_goodputCounters.requestBytes;
  checkpoint->responseBytes = g_goodputCounters.responseBytes;
  checkpoint->totalBytes = g_goodputCounters.totalBytes;

  // Paper-facing goodput is request payload throughput only.
  double aggregateGoodputGbps = (deltaRequestBytes * 8.0) / sampleInterval.GetSeconds () / 1e9;
  double responseGoodputGbps = (deltaResponseBytes * 8.0) / sampleInterval.GetSeconds () / 1e9;
  double totalTransportGoodputGbps = (deltaTotalBytes * 8.0) / sampleInterval.GetSeconds () / 1e9;
  double perHostGoodputGbps = 0.0;
  if (numHosts > 0)
    {
      perHostGoodputGbps = aggregateGoodputGbps / numHosts;
    }
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " goodputGbps=" << aggregateGoodputGbps
                        << " aggregateGoodputGbps=" << aggregateGoodputGbps
                        << " perHostGoodputGbps=" << perHostGoodputGbps
                        << " requestGoodputGbps=" << aggregateGoodputGbps
                        << " responseGoodputGbps=" << responseGoodputGbps
                        << " totalTransportGoodputGbps=" << totalTransportGoodputGbps
                        << " completedBytes=" << g_goodputCounters.requestBytes
                        << " requestCompletedBytes=" << g_goodputCounters.requestBytes
                        << " responseCompletedBytes=" << g_goodputCounters.responseBytes
                        << " totalCompletedBytes=" << g_goodputCounters.totalBytes
                        << " numHosts=" << numHosts
                        << std::endl;

  if (Simulator::Now () + sampleInterval <= g_statsActiveEnd)
    {
      Simulator::Schedule (sampleInterval,
                           &SampleGoodput,
                           stream,
                           sampleInterval,
                           checkpoint,
                           numHosts);
    }
}

static void
SampleTorQueues (Ptr<OutputStreamWrapper> stream, Time sampleInterval)
{
  uint64_t totalBytes = 0;
  uint64_t totalPackets = 0;
  uint64_t maxBytes = 0;
  uint64_t maxPackets = 0;
  uint32_t activeQueues = 0;

  for (const auto& target : g_torEgressQueueTargets)
    {
      if (!target.active)
        {
          continue;
        }

      uint64_t qdiscPackets = target.queueDisc ? target.queueDisc->GetNPackets () : 0;
      uint64_t qdiscBytes = target.queueDisc ? target.queueDisc->GetNBytes () : 0;
      uint64_t devicePackets = target.deviceQueue ? target.deviceQueue->GetNPackets () : 0;
      uint64_t deviceBytes = target.deviceQueue ? target.deviceQueue->GetNBytes () : 0;
      uint64_t packets = GetTrackedQueuePackets (target);
      uint64_t bytes = GetTrackedQueueBytes (target);
      totalBytes += bytes;
      totalPackets += packets;
      maxBytes = std::max (maxBytes, bytes);
      maxPackets = std::max (maxPackets, packets);
      activeQueues++;

      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " queue=" << target.label
                            << " role=" << target.role
                            << " packets=" << packets
                            << " bytes=" << bytes
                            << " qdiscPackets=" << qdiscPackets
                            << " qdiscBytes=" << qdiscBytes
                            << " devicePackets=" << devicePackets
                            << " deviceBytes=" << deviceBytes
                            << std::endl;
    }

  double meanBytes = 0.0;
  double meanPackets = 0.0;
  if (activeQueues > 0)
    {
      meanBytes = static_cast<double> (totalBytes) / activeQueues;
      meanPackets = static_cast<double> (totalPackets) / activeQueues;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " queue=aggregate"
                        << " maxBytes=" << maxBytes
                        << " meanBytes=" << meanBytes
                        << " maxPackets=" << maxPackets
                        << " meanPackets=" << meanPackets
                        << " numQueues=" << activeQueues
                        << std::endl;

  Simulator::Schedule (sampleInterval,
                       &SampleTorQueues,
                       stream,
                       sampleInterval);
}

static void
WriteTorQueuePeakSummary (Ptr<OutputStreamWrapper> stream)
{
  uint32_t maxPeakBytes = 0;
  uint32_t maxPeakPackets = 0;
  uint32_t activeQueues = 0;

  for (const auto& target : g_torEgressQueueTargets)
    {
      if (!target.active)
        {
          continue;
        }
      activeQueues++;
      maxPeakBytes = std::max (maxPeakBytes, target.peakCombinedBytes);
      maxPeakPackets = std::max (maxPeakPackets, target.peakCombinedPackets);
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " queue=" << target.label
                            << " role=" << target.role
                            << " peakBytes=" << target.peakCombinedBytes
                            << " peakPackets=" << target.peakCombinedPackets
                            << " peakTimeNs=" << target.peakTimeNs
                            << std::endl;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " queue=aggregate"
                        << " peakBytes=" << maxPeakBytes
                        << " peakPackets=" << maxPeakPackets
                        << " numQueues=" << activeQueues
                        << std::endl;
}

static void
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
  Simulator::Schedule (interval,
                       &SendPeriodic,
                       socket,
                       dst,
                       msgSizeBytes,
                       interval,
                       stopTime);
}

static WorkloadSpec
ReadWorkloadFile (const std::string& path, uint32_t payloadSizeBytes)
{
  std::ifstream in (path.c_str ());
  if (!in.is_open ())
    {
      throw std::runtime_error ("Could not open workload file: " + path);
    }

  WorkloadSpec spec;
  std::string firstLine;
  if (!std::getline (in, firstLine))
    {
      throw std::runtime_error ("Empty workload file: " + path);
    }

  auto toPkts = [payloadSizeBytes](double bytes) {
    return std::max<int> (1, static_cast<int> (std::ceil (bytes / payloadSizeBytes)));
  };

  if (firstLine.find ("description,") == 0)
    {
      std::string line;
      if (!std::getline (in, line) || line.find ("mean_header,") != 0)
        {
          throw std::runtime_error ("Invalid CSV workload mean header in: " + path);
        }
      spec.avgMsgSizePkts = std::stod (line.substr (line.find (',') + 1)) / payloadSizeBytes;

      if (!std::getline (in, line) || line.find ("size_bytes_or_units,cdf") != 0)
        {
          throw std::runtime_error ("Invalid CSV workload CDF header in: " + path);
        }

      while (std::getline (in, line))
        {
          if (line.empty ())
            {
              continue;
            }
          std::size_t comma = line.find (',');
          if (comma == std::string::npos)
            {
              throw std::runtime_error ("Invalid CSV workload entry in: " + path);
            }
          double sizeBytes = std::stod (line.substr (0, comma));
          double cumulativeProbability = std::stod (line.substr (comma + 1));
          spec.msgSizeCdf[cumulativeProbability] = toPkts (sizeBytes);
        }

      if (spec.msgSizeCdf.empty ())
        {
          throw std::runtime_error ("No CDF entries found in workload file: " + path);
        }
      return spec;
    }

  std::istringstream firstLineStream (firstLine);
  if (!(firstLineStream >> spec.avgMsgSizePkts))
    {
      throw std::runtime_error ("Invalid workload header in: " + path);
    }

  int msgSizePkts = 0;
  double cumulativeProbability = 0.0;
  while (in >> msgSizePkts >> cumulativeProbability)
    {
      spec.msgSizeCdf[cumulativeProbability] = msgSizePkts;
    }

  if (spec.msgSizeCdf.empty ())
    {
      throw std::runtime_error ("No CDF entries found in workload file: " + path);
    }

  return spec;
}

static std::string
ResolveWorkloadPath (const std::string& workloadName, const std::string& workloadFile)
{
  if (!workloadFile.empty ())
    {
      return workloadFile;
    }

  if (workloadName.empty ())
    {
      throw std::runtime_error ("You must set either workloadFile or workloadName.");
    }

  return "inputs/" + workloadName + ".txt";
}

int
main (int argc, char* argv[])
{
  std::string simTag = "default";
  std::string outputDir = "outputs/sird-scenarios/HomaL4Protocol-sim1-large-leaf-spine";
  std::string trafficConfig = "balanced";
  std::string workloadName = "";
  std::string workloadFile = "";

  bool enableSird = true;
  double offeredLoad = 0.5;
  double startSec = 0.2;
  double durationSec = 1.0;
  double trafficStartSec = -1.0;
  double trafficDurationSec = -1.0;
  double traceStartSec = -1.0;
  double traceDurationSec = -1.0;
  double settleTailSec = 0.2;

  uint32_t nHosts = 144;
  uint32_t hostsPerTor = 16;
  uint32_t nTors = 9;
  uint32_t nSpines = 4;

  double hostTorRateGbps = 100.0;
  double torSpineRateGbps = 400.0;
  std::string hostTorDelay = "1us";
  std::string torSpineDelay = "1us";

  uint32_t appPort = 30000;
  uint32_t clientPortBase = 50000;
  uint32_t rpcResponseBytes = 20;
  uint32_t bdpBytes = 100000;
  uint32_t payloadSizeBytes = 1442;
  int32_t homaBdpPkts = -1;
  int32_t sirdCreditBudgetPkts = -1;
  int32_t sirdUnschThresholdPkts = -1;
  double sirdEcnMdFactor = 0.85;
  double sirdEcnAiStep = 1.0;
  double sirdSenderMdFactor = 0.8;
  double sirdSenderAiStep = 1.0;
  double sirdEcnAlphaGain = 0.125;
  int32_t sirdSenderCsnThresholdPkts = -1;

  std::string deviceQueueMaxSize = "2000p";
  std::string qdiscMaxSize = "1000p";
  std::string qdiscMarkThreshold = "";
  bool torQueueIncludeDevice = false;

  bool traceMsg = true;
  bool traceTorQueue = true;
  bool traceTorQueueSeries = false;
  bool traceGoodput = true;
  uint64_t queueSampleUs = 100;
  uint64_t goodputSampleUs = 100;

  uint32_t incastSenders = 30;
  uint32_t incastMsgBytes = 500000;
  double incastLoadFraction = 0.07;
  int32_t incastReceiverIdx = -1;
  uint32_t incastSeed = 1;

  CommandLine cmd (__FILE__);
  cmd.AddValue ("simTag", "Suffix for output trace files", simTag);
  cmd.AddValue ("outputDir", "Directory for output trace files", outputDir);
  cmd.AddValue ("trafficConfig", "balanced, core, or incast", trafficConfig);
  cmd.AddValue ("workloadName", "Logical workload name; default path resolves to inputs/<name>.txt", workloadName);
  cmd.AddValue ("workloadFile", "Explicit workload file path", workloadFile);
  cmd.AddValue ("enableSird", "Enable SIRD control path", enableSird);
  cmd.AddValue ("offeredLoad", "Per-host offered load fraction for background all-to-all traffic", offeredLoad);
  cmd.AddValue ("startSec", "Background traffic start time", startSec);
  cmd.AddValue ("durationSec", "Traffic generation duration", durationSec);
  cmd.AddValue ("trafficStartSec", "Explicit background traffic start time; negative falls back to startSec", trafficStartSec);
  cmd.AddValue ("trafficDurationSec", "Explicit traffic generation duration; negative falls back to durationSec", trafficDurationSec);
  cmd.AddValue ("traceStartSec", "Explicit stats window start time; negative falls back to startSec", traceStartSec);
  cmd.AddValue ("traceDurationSec", "Explicit stats window duration; negative falls back to durationSec", traceDurationSec);
  cmd.AddValue ("settleTailSec", "Drain time after traffic stop", settleTailSec);
  cmd.AddValue ("nHosts", "Number of hosts in the leaf-spine topology", nHosts);
  cmd.AddValue ("hostsPerTor", "Number of hosts attached to each ToR", hostsPerTor);
  cmd.AddValue ("nTors", "Number of ToR switches", nTors);
  cmd.AddValue ("nSpines", "Number of spine switches", nSpines);
  cmd.AddValue ("torSpineRateGbps", "Override ToR-Spine rate in Gbps; <=0 uses trafficConfig default", torSpineRateGbps);
  cmd.AddValue ("traceMsg", "Trace message begin/finish", traceMsg);
  cmd.AddValue ("traceTorQueue", "Sample ToR egress queues over time", traceTorQueue);
  cmd.AddValue ("traceTorQueueSeries", "When true, also write ToR queue time series in addition to peak summary", traceTorQueueSeries);
  cmd.AddValue ("traceGoodput", "Sample aggregate application-level goodput", traceGoodput);
  cmd.AddValue ("queueSampleUs", "ToR queue sampling period in microseconds", queueSampleUs);
  cmd.AddValue ("goodputSampleUs", "Goodput sampling period in microseconds", goodputSampleUs);
  cmd.AddValue ("incastSenders", "Number of burst senders in incast mode", incastSenders);
  cmd.AddValue ("incastMsgBytes", "Per-message size for incast overlay", incastMsgBytes);
  cmd.AddValue ("incastLoadFraction", "Aggregate incast overlay load fraction relative to total background offered load", incastLoadFraction);
  cmd.AddValue ("incastReceiverIdx", "Receiver host index for incast; negative chooses random receiver", incastReceiverIdx);
  cmd.AddValue ("incastSeed", "Seed for deterministic incast sender/receiver selection", incastSeed);
  cmd.AddValue ("clientPortBase", "Base port for background RPC client sockets", clientPortBase);
  cmd.AddValue ("rpcResponseBytes", "Response size in bytes for background RPC replies", rpcResponseBytes);
  cmd.AddValue ("bdpBytes", "Paper sim1 RTT BDP in bytes; defaults to 100KB from Table 2", bdpBytes);
  cmd.AddValue ("payloadSizeBytes", "Payload bytes represented by one workload packet unit", payloadSizeBytes);
  cmd.AddValue ("rttPkts", "RTT BDP in packets; negative derives from bdpBytes/payloadSizeBytes", homaBdpPkts);
  cmd.AddValue ("sirdCreditBudgetPkts", "SIRD global credit budget in packets; negative derives as 1.5xBDP", sirdCreditBudgetPkts);
  cmd.AddValue ("sirdUnschThresholdPkts", "SIRD unscheduled threshold in packets; negative derives as 1.0xBDP", sirdUnschThresholdPkts);
  cmd.AddValue ("sirdEcnMdFactor", "SIRD ECN multiplicative decrease factor", sirdEcnMdFactor);
  cmd.AddValue ("sirdEcnAiStep", "SIRD ECN additive increase step", sirdEcnAiStep);
  cmd.AddValue ("sirdSenderMdFactor", "SIRD sender-feedback multiplicative decrease factor", sirdSenderMdFactor);
  cmd.AddValue ("sirdSenderAiStep", "SIRD sender-feedback additive increase step", sirdSenderAiStep);
  cmd.AddValue ("sirdEcnAlphaGain", "SIRD ECN EWMA gain", sirdEcnAlphaGain);
  cmd.AddValue ("sirdSenderCsnThresholdPkts", "SIRD sender CSN threshold in packets; negative derives as 0.5xBDP", sirdSenderCsnThresholdPkts);
  cmd.AddValue ("deviceQueueMaxSize", "PointToPointNetDevice TxQueue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "SirdQueueDisc MaxSize", qdiscMaxSize);
  cmd.AddValue ("qdiscMarkThreshold", "SirdQueueDisc ECN mark threshold", qdiscMarkThreshold);
  cmd.AddValue ("torQueueIncludeDevice", "When true, count device TxQueue occupancy on top of the ToR queue; false keeps strict queue-only semantics", torQueueIncludeDevice);
  cmd.Parse (argc, argv);

  if (nTors * hostsPerTor != nHosts)
    {
      NS_FATAL_ERROR ("nTors * hostsPerTor must equal nHosts.");
    }
  if (offeredLoad <= 0.0 || offeredLoad > 1.0)
    {
      NS_FATAL_ERROR ("offeredLoad must be in (0, 1].");
    }
  if (incastLoadFraction < 0.0 || incastLoadFraction >= 1.0)
    {
      NS_FATAL_ERROR ("incastLoadFraction must be in [0, 1).");
    }
  if (payloadSizeBytes == 0 || bdpBytes == 0)
    {
      NS_FATAL_ERROR ("payloadSizeBytes and bdpBytes must be positive.");
    }
  if (clientPortBase + nHosts >= 65535)
    {
      NS_FATAL_ERROR ("clientPortBase leaves insufficient room for per-host background RPC ports.");
    }
  g_torQueueIncludeDevice = torQueueIncludeDevice;
  g_rpcServerPort = static_cast<uint16_t> (appPort);
  g_goodputCounters = GoodputCounters ();

  const double effectiveTrafficStartSec = (trafficStartSec >= 0.0) ? trafficStartSec : startSec;
  const double effectiveTrafficDurationSec = (trafficDurationSec >= 0.0) ? trafficDurationSec : durationSec;
  const double effectiveTraceStartSec = (traceStartSec >= 0.0) ? traceStartSec : startSec;
  const double effectiveTraceDurationSec = (traceDurationSec >= 0.0) ? traceDurationSec : durationSec;
  if (effectiveTrafficDurationSec <= 0.0 || effectiveTraceDurationSec <= 0.0)
    {
      NS_FATAL_ERROR ("trafficDurationSec and traceDurationSec must be positive after fallback resolution.");
    }
  if (effectiveTraceStartSec < effectiveTrafficStartSec)
    {
      NS_FATAL_ERROR ("traceStartSec must be greater than or equal to trafficStartSec.");
    }
  if (effectiveTraceStartSec + effectiveTraceDurationSec >
      effectiveTrafficStartSec + effectiveTrafficDurationSec + 1e-12)
    {
      NS_FATAL_ERROR ("trace window must lie within the traffic generation window.");
    }

  const int32_t derivedBdpPkts = std::max<int32_t> (
    1, static_cast<int32_t> (std::lround (static_cast<double> (bdpBytes) / payloadSizeBytes)));
  auto derivePktsFromBdpBytes = [bdpBytes, payloadSizeBytes](double multiplier) {
    return std::max<int32_t> (
      1, static_cast<int32_t> (
           std::lround (multiplier * static_cast<double> (bdpBytes) / payloadSizeBytes)));
  };
  if (homaBdpPkts < 0)
    {
      homaBdpPkts = derivedBdpPkts;
    }
  if (sirdCreditBudgetPkts < 0)
    {
      sirdCreditBudgetPkts = derivePktsFromBdpBytes (1.5);
    }
  if (sirdUnschThresholdPkts < 0)
    {
      sirdUnschThresholdPkts = derivePktsFromBdpBytes (1.0);
    }
  if (sirdSenderCsnThresholdPkts < 0)
    {
      sirdSenderCsnThresholdPkts = derivePktsFromBdpBytes (0.5);
    }
  if (qdiscMarkThreshold.empty ())
    {
      std::ostringstream threshold;
      threshold << derivePktsFromBdpBytes (1.25) << "p";
      qdiscMarkThreshold = threshold.str ();
    }

  if (trafficConfig == "balanced")
    {
      if (torSpineRateGbps <= 0.0)
        {
          torSpineRateGbps = 400.0;
        }
    }
  else if (trafficConfig == "core")
    {
      if (torSpineRateGbps <= 0.0 || torSpineRateGbps == 400.0)
        {
          torSpineRateGbps = 200.0;
        }
    }
  else if (trafficConfig == "incast")
    {
      if (torSpineRateGbps <= 0.0)
        {
          torSpineRateGbps = 400.0;
        }
    }
  else
    {
      NS_FATAL_ERROR ("trafficConfig must be one of: balanced, core, incast.");
    }

  WorkloadSpec workload;
  try
    {
      workload = ReadWorkloadFile (ResolveWorkloadPath (workloadName, workloadFile),
                                   payloadSizeBytes);
    }
  catch (const std::exception& ex)
    {
      NS_FATAL_ERROR (ex.what ());
    }

  Time::SetResolution (Time::NS);
  SeedManager::SetRun (1);

  Config::SetDefault ("ns3::Ipv4GlobalRouting::EcmpMode", EnumValue (Ipv4GlobalRouting::ECMP_RANDOM));
  Config::SetDefault ("ns3::MsgGeneratorApp::PayloadSize", UintegerValue (payloadSizeBytes));
  Config::SetDefault ("ns3::HomaL4Protocol::RttPackets", UintegerValue (static_cast<uint16_t> (homaBdpPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::NumTotalPrioBands", UintegerValue (8));
  Config::SetDefault ("ns3::HomaL4Protocol::NumUnschedPrioBands", UintegerValue (2));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEnabled", BooleanValue (enableSird));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdCreditBudgetPkts", UintegerValue (static_cast<uint16_t> (sirdCreditBudgetPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdUnschThresholdPkts", UintegerValue (static_cast<uint16_t> (sirdUnschThresholdPkts)));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnMdFactor", DoubleValue (sirdEcnMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAiStep", DoubleValue (sirdEcnAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderMdFactor", DoubleValue (sirdSenderMdFactor));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderAiStep", DoubleValue (sirdSenderAiStep));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdEcnAlphaGain", DoubleValue (sirdEcnAlphaGain));
  Config::SetDefault ("ns3::HomaL4Protocol::SirdSenderCsnThreshold", UintegerValue (static_cast<uint16_t> (sirdSenderCsnThresholdPkts)));

  NodeContainer hosts;
  hosts.Create (nHosts);
  NodeContainer tors;
  tors.Create (nTors);
  NodeContainer spines;
  spines.Create (nSpines);

  PointToPointHelper hostTor;
  hostTor.SetDeviceAttribute ("DataRate", StringValue ("100Gbps"));
  hostTor.SetChannelAttribute ("Delay", StringValue (hostTorDelay));
  hostTor.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  PointToPointHelper torSpine;
  {
    std::ostringstream rate;
    rate << torSpineRateGbps << "Gbps";
    torSpine.SetDeviceAttribute ("DataRate", StringValue (rate.str ()));
  }
  torSpine.SetChannelAttribute ("Delay", StringValue (torSpineDelay));
  torSpine.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SirdQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "MarkThreshold", StringValue (qdiscMarkThreshold),
                        "UseEcn", BooleanValue (true));

  std::vector<NetDeviceContainer> hostTorLinks;
  std::vector<QueueDiscContainer> hostTorQdiscs;
  hostTorLinks.reserve (nHosts);
  hostTorQdiscs.reserve (nHosts);
  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      uint32_t torIdx = hostIdx / hostsPerTor;
      NetDeviceContainer link = hostTor.Install (hosts.Get (hostIdx), tors.Get (torIdx));
      hostTorLinks.push_back (link);
    }

  std::vector<NetDeviceContainer> torSpineLinks;
  std::vector<QueueDiscContainer> torSpineQdiscs;
  torSpineLinks.reserve (nTors * nSpines);
  torSpineQdiscs.reserve (nTors * nSpines);
  for (uint32_t torIdx = 0; torIdx < nTors; ++torIdx)
    {
      for (uint32_t spineIdx = 0; spineIdx < nSpines; ++spineIdx)
        {
          NetDeviceContainer link = torSpine.Install (tors.Get (torIdx), spines.Get (spineIdx));
          torSpineLinks.push_back (link);
        }
    }

  InternetStackHelper stack;
  stack.Install (hosts);
  stack.Install (tors);
  stack.Install (spines);

  for (uint32_t hostIdx = 0; hostIdx < hostTorLinks.size (); ++hostIdx)
    {
      QueueDiscContainer qdiscs = tch.Install (hostTorLinks[hostIdx]);
      hostTorQdiscs.push_back (qdiscs);
      uint32_t torIdx = hostIdx / hostsPerTor;
      Ptr<PointToPointNetDevice> torDevice = DynamicCast<PointToPointNetDevice> (hostTorLinks[hostIdx].Get (1));
      std::ostringstream label;
      label << "tor" << torIdx << "_to_host" << hostIdx;
      QueueSampleTarget target;
      target.label = label.str ();
      target.role = "tor_to_host";
      target.torIdx = torIdx;
      target.hostIdx = static_cast<int32_t> (hostIdx);
      target.spineIdx = -1;
      target.queueDisc = qdiscs.Get (1);
      target.deviceQueue = torDevice ? torDevice->GetQueue () : nullptr;
      g_torEgressQueueTargets.push_back (target);
    }
  for (uint32_t linkIdx = 0; linkIdx < torSpineLinks.size (); ++linkIdx)
    {
      QueueDiscContainer qdiscs = tch.Install (torSpineLinks[linkIdx]);
      torSpineQdiscs.push_back (qdiscs);
      uint32_t torIdx = linkIdx / nSpines;
      uint32_t spineIdx = linkIdx % nSpines;
      Ptr<PointToPointNetDevice> torDevice = DynamicCast<PointToPointNetDevice> (torSpineLinks[linkIdx].Get (0));
      std::ostringstream label;
      label << "tor" << torIdx << "_to_spine" << spineIdx;
      QueueSampleTarget target;
      target.label = label.str ();
      target.role = "tor_to_spine";
      target.torIdx = torIdx;
      target.hostIdx = -1;
      target.spineIdx = static_cast<int32_t> (spineIdx);
      target.queueDisc = qdiscs.Get (0);
      target.deviceQueue = torDevice ? torDevice->GetQueue () : nullptr;
      g_torEgressQueueTargets.push_back (target);
    }

  Ipv4AddressHelper address;
  address.SetBase ("10.10.0.0", "255.255.255.0");
  std::vector<Ipv4Address> hostIps;
  hostIps.resize (nHosts);

  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      Ipv4InterfaceContainer ifs = address.Assign (hostTorLinks[hostIdx]);
      hostIps[hostIdx] = ifs.GetAddress (0);
      address.NewNetwork ();
    }
  for (const auto& link : torSpineLinks)
    {
      address.Assign (link);
      address.NewNetwork ();
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  g_statsActiveStart = Seconds (effectiveTraceStartSec);
  g_statsActiveEnd = Seconds (effectiveTraceStartSec + effectiveTraceDurationSec);

  AsciiTraceHelper ascii;
  std::string mkdirCmd = "mkdir -p " + outputDir;
  std::system (mkdirCmd.c_str ());
  std::ostringstream prefix;
  prefix << outputDir << "/sim1_" << simTag;

  if (traceMsg || traceGoodput)
    {
      Ptr<OutputStreamWrapper> msgStream = traceMsg ? ascii.CreateFileStream (prefix.str () + ".msg.tr") : nullptr;
      if (traceMsg)
        {
          Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgBegin",
                                         MakeBoundCallback (&TraceMsgBegin, msgStream));
        }
      Config::ConnectWithoutContext ("/NodeList/*/$ns3::HomaL4Protocol/MsgFinish",
                                     MakeBoundCallback (&TraceMsgFinish, msgStream));
    }

  if (traceGoodput)
    {
      Ptr<OutputStreamWrapper> goodputStream = ascii.CreateFileStream (prefix.str () + ".goodput.tr");
      GoodputCheckpoint* checkpoint = new GoodputCheckpoint ();
      Simulator::Schedule (Seconds (effectiveTraceStartSec) + MicroSeconds (goodputSampleUs),
                           &SampleGoodput,
                           goodputStream,
                           MicroSeconds (goodputSampleUs),
                           checkpoint,
                           nHosts);
    }

  std::vector<Ipv4Address> remoteHosts;
  remoteHosts.reserve (nHosts);
  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      remoteHosts.push_back (hostIps[hostIdx]);
    }

  double backgroundOfferedLoad = offeredLoad;
  int32_t actualIncastReceiver = -1;
  if (trafficConfig == "incast")
    {
      backgroundOfferedLoad = offeredLoad * (1.0 - incastLoadFraction);
    }

  std::vector<Ptr<RpcWorkloadApp>> backgroundApps;
  backgroundApps.reserve (nHosts);
  for (uint32_t hostIdx = 0; hostIdx < nHosts; ++hostIdx)
    {
      const uint16_t clientPort = static_cast<uint16_t> (clientPortBase + hostIdx);
      Ptr<RpcWorkloadApp> app = CreateObject<RpcWorkloadApp> (hostIps[hostIdx],
                                                              static_cast<uint16_t> (appPort),
                                                              clientPort);
      app->Install (hosts.Get (hostIdx), remoteHosts);
      app->SetRequestWorkload (backgroundOfferedLoad, workload.msgSizeCdf, workload.avgMsgSizePkts);
      app->SetResponseBytes (rpcResponseBytes);
      app->Start (Seconds (effectiveTrafficStartSec));
      app->Stop (Seconds (effectiveTrafficStartSec + effectiveTrafficDurationSec));
      backgroundApps.push_back (app);
    }

  if (trafficConfig == "incast")
    {
      std::vector<uint32_t> hostIndices (nHosts);
      std::iota (hostIndices.begin (), hostIndices.end (), 0);
      std::mt19937 rng (incastSeed);

      uint32_t receiver = 0;
      if (incastReceiverIdx >= 0)
        {
          receiver = static_cast<uint32_t> (incastReceiverIdx);
        }
      else
        {
          std::uniform_int_distribution<uint32_t> receiverDist (0, nHosts - 1);
          receiver = receiverDist (rng);
        }
      actualIncastReceiver = static_cast<int32_t> (receiver);

      hostIndices.erase (std::remove (hostIndices.begin (), hostIndices.end (), receiver), hostIndices.end ());
      std::shuffle (hostIndices.begin (), hostIndices.end (), rng);

      if (incastSenders > hostIndices.size ())
        {
          NS_FATAL_ERROR ("incastSenders exceeds available non-receiver hosts.");
        }

      double aggregateIncastGbps = incastLoadFraction * offeredLoad * nHosts * hostTorRateGbps;
      double perSenderGbps = aggregateIncastGbps / incastSenders;
      if (perSenderGbps <= 0.0)
        {
          NS_FATAL_ERROR ("incastLoadFraction leads to non-positive per-sender rate.");
        }

      Time incastInterval = Seconds ((static_cast<double> (incastMsgBytes) * 8.0) /
                                     (perSenderGbps * 1e9));
      Time stopTime = Seconds (effectiveTrafficStartSec + effectiveTrafficDurationSec);

      for (uint32_t i = 0; i < incastSenders; ++i)
        {
          uint32_t senderIdx = hostIndices[i];
          Ptr<SocketFactory> sFactory = hosts.Get (senderIdx)->GetObject<HomaSocketFactory> ();
          Ptr<Socket> senderSock = sFactory->CreateSocket ();
          senderSock->Bind (InetSocketAddress (hostIps[senderIdx], static_cast<uint16_t> (40000 + i)));
          Simulator::Schedule (Seconds (effectiveTrafficStartSec),
                               &SendPeriodic,
                               senderSock,
                               InetSocketAddress (hostIps[receiver], appPort),
                               incastMsgBytes,
                               incastInterval,
                               stopTime);
        }
    }

  if (trafficConfig != "incast")
    {
      actualIncastReceiver = -1;
    }

  ConfigureActiveTorQueueTargets (trafficConfig, actualIncastReceiver);
  g_queueActiveStart = Seconds (effectiveTraceStartSec);
  g_queueActiveEnd = Seconds (effectiveTraceStartSec + effectiveTraceDurationSec);
  AttachQueuePeakCallbacks ();

  const double simulationStopSec = std::max (effectiveTrafficStartSec + effectiveTrafficDurationSec,
                                             effectiveTraceStartSec + effectiveTraceDurationSec) +
                                   settleTailSec;
  Simulator::Stop (Seconds (simulationStopSec));

  if (traceTorQueue)
    {
      Ptr<OutputStreamWrapper> queueStream = ascii.CreateFileStream (prefix.str () + ".tor-egress-queue.tr");
      if (traceTorQueueSeries)
        {
          Simulator::Schedule (Seconds (effectiveTraceStartSec),
                               &SampleTorQueues,
                               queueStream,
                               MicroSeconds (queueSampleUs));
        }
      Time queueSummaryTime = std::max (Seconds (0), Seconds (simulationStopSec) - NanoSeconds (1));
      Simulator::Schedule (queueSummaryTime,
                           &WriteTorQueuePeakSummary,
                           queueStream);
    }

  Simulator::Run ();
  Simulator::Destroy ();
  return 0;
}
