/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * 400G single-switch all-to-all scenario with shared-buffer ECN, PFC-style
 * pause/resume, QCN-style sender feedback, and per-flow initial rates.
 */

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("Qcn400gAllToAll");

namespace {

struct FlowConfig
{
  uint32_t srcHost;
  uint32_t dstHost;
  uint32_t flowId;
  uint16_t port;
  uint64_t bytes;
  double startSec;
  double initialRateFraction;
};

struct FlowRuntime
{
  FlowConfig config;
  Ptr<RateControlledFlowApp> app;
  Ptr<PacketSink> sink;
};

struct ExperimentStats
{
  std::array<uint64_t, 4> queueEvents;
  std::array<uint32_t, 4> maxQueuePackets;
  std::array<uint32_t, 4> maxQueueBytes;
  std::array<uint64_t, 4> maxEgressSharedBytes;
  std::array<uint64_t, 4> ecnAttempts;
  std::array<uint64_t, 4> ecnMarks;
  std::array<uint64_t, 4> qcnEvents;
  std::array<uint64_t, 4> pfcPauseEvents;
  std::array<uint64_t, 4> pfcResumeEvents;
  uint64_t poolEvents;
  uint64_t maxSharedBytes;

  ExperimentStats ()
    : queueEvents ({0, 0, 0, 0}),
      maxQueuePackets ({0, 0, 0, 0}),
      maxQueueBytes ({0, 0, 0, 0}),
      maxEgressSharedBytes ({0, 0, 0, 0}),
      ecnAttempts ({0, 0, 0, 0}),
      ecnMarks ({0, 0, 0, 0}),
      qcnEvents ({0, 0, 0, 0}),
      pfcPauseEvents ({0, 0, 0, 0}),
      pfcResumeEvents ({0, 0, 0, 0}),
      poolEvents (0),
      maxSharedBytes (0)
  {
  }
};

DataRate
Gbps (double value)
{
  return DataRate (static_cast<uint64_t> (value * 1e9));
}

std::string
GbpsString (double value)
{
  std::ostringstream out;
  out << value << "Gbps";
  return out.str ();
}

uint32_t
OriginalHostId (uint32_t zeroBasedHost)
{
  return zeroBasedHost + 1;
}

void
TraceQueueState (Ptr<OutputStreamWrapper> stream,
                 ExperimentStats* stats,
                 bool writeEvents,
                 uint32_t egressId,
                 uint32_t packets,
                 uint32_t bytes,
                 uint64_t sharedBytes,
                 bool enqueue)
{
  if (stats && egressId < stats->queueEvents.size ())
    {
      stats->queueEvents[egressId]++;
      stats->maxQueuePackets[egressId] =
          std::max (stats->maxQueuePackets[egressId], packets);
      stats->maxQueueBytes[egressId] =
          std::max (stats->maxQueueBytes[egressId], bytes);
      stats->maxEgressSharedBytes[egressId] =
          std::max (stats->maxEgressSharedBytes[egressId], sharedBytes);
    }

  if (!writeEvents)
    {
      return;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " egressHost=" << OriginalHostId (egressId)
                        << " qPkts=" << packets
                        << " qBytes=" << bytes
                        << " sharedBytes=" << sharedBytes
                        << " op=" << (enqueue ? "enqueue" : "dequeue")
                        << std::endl;
}

void
TracePfcEvent (Ptr<OutputStreamWrapper> stream,
               ExperimentStats* stats,
               bool writeEvents,
               uint32_t egressId,
               uint32_t sourceId,
               uint32_t packets,
               uint32_t pause)
{
  if (stats && egressId < stats->pfcPauseEvents.size ())
    {
      if (pause)
        {
          stats->pfcPauseEvents[egressId]++;
        }
      else
        {
          stats->pfcResumeEvents[egressId]++;
        }
    }

  if (!writeEvents)
    {
      return;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " egressHost=" << OriginalHostId (egressId)
                        << " sourceHost=" << OriginalHostId (sourceId)
                        << " qPkts=" << packets
                        << " event=" << (pause ? "pause" : "resume")
                        << std::endl;
}

void
TraceQcnEvent (Ptr<OutputStreamWrapper> stream,
               ExperimentStats* stats,
               bool writeEvents,
               uint32_t egressId,
               uint32_t sourceId,
               uint32_t flowId,
               uint32_t packets,
               double factor)
{
  if (stats && egressId < stats->qcnEvents.size ())
    {
      stats->qcnEvents[egressId]++;
    }

  if (!writeEvents)
    {
      return;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " egressHost=" << OriginalHostId (egressId)
                        << " sourceHost=" << OriginalHostId (sourceId)
                        << " flowId=" << flowId
                        << " qPkts=" << packets
                        << " mdFactor=" << factor
                        << std::endl;
}

void
TraceEcnAttempt (Ptr<OutputStreamWrapper> stream,
                 ExperimentStats* stats,
                 bool writeEvents,
                 uint32_t egressId,
                 uint32_t sourceId,
                 uint32_t flowId,
                 uint32_t packets)
{
  if (stats && egressId < stats->ecnAttempts.size ())
    {
      stats->ecnAttempts[egressId]++;
    }

  if (!writeEvents)
    {
      return;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " egressHost=" << OriginalHostId (egressId)
                        << " sourceHost=" << OriginalHostId (sourceId)
                        << " flowId=" << flowId
                        << " qPkts=" << packets
                        << " event=attempt"
                        << std::endl;
}

void
TraceEcnEvent (Ptr<OutputStreamWrapper> stream,
               ExperimentStats* stats,
               bool writeEvents,
               uint32_t egressId,
               uint32_t sourceId,
               uint32_t flowId,
               uint32_t packets)
{
  if (stats && egressId < stats->ecnMarks.size ())
    {
      stats->ecnMarks[egressId]++;
    }

  if (!writeEvents)
    {
      return;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " egressHost=" << OriginalHostId (egressId)
                        << " sourceHost=" << OriginalHostId (sourceId)
                        << " flowId=" << flowId
                        << " qPkts=" << packets
                        << std::endl;
}

void
TraceSharedPool (Ptr<OutputStreamWrapper> stream,
                 ExperimentStats* stats,
                 bool writeEvents,
                 uint64_t usedBytes,
                 uint64_t maxBytes)
{
  if (stats)
    {
      stats->poolEvents++;
      stats->maxSharedBytes = std::max (stats->maxSharedBytes, usedBytes);
    }

  if (!writeEvents)
    {
      return;
    }

  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " usedBytes=" << usedBytes
                        << " maxBytes=" << maxBytes
                        << std::endl;
}

void
TraceAppRate (Ptr<OutputStreamWrapper> stream,
              uint32_t flowId,
              uint64_t oldRate,
              uint64_t newRate)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " flowId=" << flowId
                        << " oldGbps=" << oldRate / 1e9
                        << " newGbps=" << newRate / 1e9
                        << std::endl;
}

void
TraceAppPause (Ptr<OutputStreamWrapper> stream, uint32_t flowId, bool paused)
{
  *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                        << " flowId=" << flowId
                        << " state=" << (paused ? "pause" : "resume")
                        << std::endl;
}

void
SampleFlows (Ptr<OutputStreamWrapper> stream,
             const std::vector<FlowRuntime>* flows,
             Time interval,
             Time stopTime)
{
  for (const FlowRuntime& flow : *flows)
    {
      uint64_t txBytes = flow.app ? flow.app->GetTotalBytes () : 0;
      uint64_t rxBytes = flow.sink ? flow.sink->GetTotalRx () : 0;
      *stream->GetStream () << Simulator::Now ().GetNanoSeconds ()
                            << " flowId=" << flow.config.flowId
                            << " src=" << OriginalHostId (flow.config.srcHost)
                            << " dst=" << OriginalHostId (flow.config.dstHost)
                            << " txBytes=" << txBytes
                            << " rxBytes=" << rxBytes
                            << " rateGbps="
                            << (flow.app ? flow.app->GetCurrentRate ().GetBitRate () / 1e9 : 0)
                            << " paused=" << (flow.app && flow.app->IsPaused () ? 1 : 0)
                            << std::endl;
    }

  if (Simulator::Now () + interval <= stopTime)
    {
      Simulator::Schedule (interval, &SampleFlows, stream, flows, interval, stopTime);
    }
}

void
WriteSummary (const std::string& path,
              const std::vector<FlowRuntime>& flows,
              const ExperimentStats& stats,
              double linkRateGbps,
              double stopTime,
              uint32_t packetSize,
              uint32_t deviceMtu,
              const std::string& sharedBufferSize,
              uint32_t kmin,
              uint32_t kmax,
              double pmax)
{
  std::ofstream out (path.c_str ());
  out << "linkRateGbps,stopTimeSec,packetPayloadBytes,deviceMtuBytes,sharedBufferSize,kmin,kmax,pmax\n";
  out << linkRateGbps << "," << stopTime << "," << packetSize << "," << deviceMtu << ","
      << sharedBufferSize << "," << kmin << "," << kmax << "," << pmax << "\n\n";
  out << "flowId,srcHost,dstHost,initialRateGbps,bytes,targetBytes,completionRatio\n";
  for (const FlowRuntime& flow : flows)
    {
      uint64_t rxBytes = flow.sink ? flow.sink->GetTotalRx () : 0;
      double initGbps = flow.config.initialRateFraction * linkRateGbps;
      double completion = flow.config.bytes == 0
                              ? 0.0
                              : static_cast<double> (rxBytes) / flow.config.bytes;
      out << flow.config.flowId << ","
          << OriginalHostId (flow.config.srcHost) << ","
          << OriginalHostId (flow.config.dstHost) << ","
          << initGbps << ","
          << rxBytes << ","
          << flow.config.bytes << ","
          << completion << "\n";
    }

  out << "\n";
  out << "egressHost,queueEvents,maxQueuePackets,maxQueueBytes,maxObservedSharedBytes,"
         "ecnAttempts,ecnMarks,qcnEvents,pfcPauseEvents,pfcResumeEvents\n";
  for (uint32_t egressId = 0; egressId < stats.queueEvents.size (); ++egressId)
    {
      out << OriginalHostId (egressId) << ","
          << stats.queueEvents[egressId] << ","
          << stats.maxQueuePackets[egressId] << ","
          << stats.maxQueueBytes[egressId] << ","
          << stats.maxEgressSharedBytes[egressId] << ","
          << stats.ecnAttempts[egressId] << ","
          << stats.ecnMarks[egressId] << ","
          << stats.qcnEvents[egressId] << ","
          << stats.pfcPauseEvents[egressId] << ","
          << stats.pfcResumeEvents[egressId] << "\n";
    }

  out << "\n";
  out << "poolEvents,maxSharedBytes\n";
  out << stats.poolEvents << "," << stats.maxSharedBytes << "\n";
}

} // namespace

int
main (int argc, char* argv[])
{
  double linkRateGbps = 400.0;
  std::string linkDelay = "4us";
  uint32_t packetSize = 4000;
  uint32_t deviceMtu = 0;
  double stopTime = 0.5;
  double startTime = 40e-6;
  std::string deviceQueueMaxSize = "1p";
  std::string qdiscMaxSize = "100000p";
  std::string sharedBufferSize = "82MB";
  uint32_t kmin = 700;
  uint32_t kmax = 1600;
  double pmax = 0.2;
  bool useEcn = true;
  bool usePfc = true;
  bool useDynamicPfcThreshold = true;
  bool useQcn = true;
  uint32_t pauseThreshold = 1600;
  uint32_t resumeThreshold = 700;
  uint32_t minDynamicPauseThreshold = 700;
  double dynamicPauseHeadroomFraction = 0.25;
  double qcnMdFactor = 0.5;
  uint32_t qcnIntervalPackets = 64;
  double aiStepGbps = 1.0;
  double aiIntervalUs = 50.0;
  std::string outputDir = "outputs/qcn_400g_alltoall";
  std::string simTag = "default";
  double sampleIntervalUs = 10.0;
  bool traceQueueEvents = false;
  bool traceControlEvents = false;
  bool tracePoolEvents = false;

  CommandLine cmd (__FILE__);
  cmd.AddValue ("linkRateGbps", "Host-switch link rate in Gbps", linkRateGbps);
  cmd.AddValue ("linkDelay", "One-way propagation delay", linkDelay);
  cmd.AddValue ("packetSize", "Application packet payload size", packetSize);
  cmd.AddValue ("deviceMtu", "PointToPointNetDevice MTU; 0 uses packetSize + 64", deviceMtu);
  cmd.AddValue ("stopTime", "Simulation stop time in seconds", stopTime);
  cmd.AddValue ("startTime", "Flow start time in seconds", startTime);
  cmd.AddValue ("deviceQueueMaxSize", "PointToPointNetDevice TxQueue MaxSize", deviceQueueMaxSize);
  cmd.AddValue ("qdiscMaxSize", "Switch queue disc MaxSize", qdiscMaxSize);
  cmd.AddValue ("sharedBufferSize", "Switch shared buffer size", sharedBufferSize);
  cmd.AddValue ("kmin", "ECN Kmin in packets", kmin);
  cmd.AddValue ("kmax", "ECN Kmax in packets", kmax);
  cmd.AddValue ("pmax", "ECN Pmax", pmax);
  cmd.AddValue ("useEcn", "Enable ECN marking", useEcn);
  cmd.AddValue ("usePfc", "Enable PFC-style pause/resume", usePfc);
  cmd.AddValue ("useDynamicPfcThreshold", "Enable dynamic PFC threshold", useDynamicPfcThreshold);
  cmd.AddValue ("useQcn", "Enable QCN-style sender feedback", useQcn);
  cmd.AddValue ("pauseThreshold", "Static PFC pause threshold in packets", pauseThreshold);
  cmd.AddValue ("resumeThreshold", "Static PFC resume threshold in packets", resumeThreshold);
  cmd.AddValue ("minDynamicPauseThreshold", "Dynamic PFC pause-threshold lower bound in packets", minDynamicPauseThreshold);
  cmd.AddValue ("dynamicPauseHeadroomFraction", "Shared headroom fraction used for dynamic PFC threshold", dynamicPauseHeadroomFraction);
  cmd.AddValue ("qcnMdFactor", "QCN multiplicative decrease factor", qcnMdFactor);
  cmd.AddValue ("qcnIntervalPackets", "Minimum queued-packet delta between QCN events", qcnIntervalPackets);
  cmd.AddValue ("aiStepGbps", "Sender additive recovery step in Gbps", aiStepGbps);
  cmd.AddValue ("aiIntervalUs", "Sender additive recovery interval in microseconds", aiIntervalUs);
  cmd.AddValue ("outputDir", "Directory for output trace files", outputDir);
  cmd.AddValue ("simTag", "Prefix tag for output files", simTag);
  cmd.AddValue ("sampleIntervalUs", "Flow sample interval in microseconds", sampleIntervalUs);
  cmd.AddValue ("traceQueueEvents", "Write every queue enqueue/dequeue event", traceQueueEvents);
  cmd.AddValue ("traceControlEvents", "Write every ECN/PFC/QCN control event", traceControlEvents);
  cmd.AddValue ("tracePoolEvents", "Write every shared-pool occupancy event", tracePoolEvents);
  cmd.Parse (argc, argv);

  Time::SetResolution (Time::NS);
  std::system (("mkdir -p " + outputDir).c_str ());

  if (deviceMtu == 0)
    {
      deviceMtu = packetSize + 64;
    }

  NodeContainer hosts;
  hosts.Create (4);
  Ptr<Node> sw = CreateObject<Node> ();

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue (GbpsString (linkRateGbps)));
  p2p.SetDeviceAttribute ("Mtu", UintegerValue (deviceMtu));
  p2p.SetChannelAttribute ("Delay", StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue (deviceQueueMaxSize));

  std::array<NetDeviceContainer, 4> links;
  for (uint32_t i = 0; i < 4; ++i)
    {
      NodeContainer pair;
      pair.Add (sw);
      pair.Add (hosts.Get (i));
      links[i] = p2p.Install (pair);
    }

  InternetStackHelper internet;
  internet.SetIpv6StackInstall (false);
  internet.Install (hosts);
  internet.Install (sw);

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::SwitchSharedBufferQueueDisc",
                        "MaxSize", StringValue (qdiscMaxSize),
                        "SharedBufferSize", StringValue (sharedBufferSize),
                        "Kmin", UintegerValue (kmin),
                        "Kmax", UintegerValue (kmax),
                        "Pmax", DoubleValue (pmax),
                        "UseEcn", BooleanValue (useEcn),
                        "UsePfc", BooleanValue (usePfc),
                        "UseDynamicPfcThreshold", BooleanValue (useDynamicPfcThreshold),
                        "PauseThreshold", UintegerValue (pauseThreshold),
                        "ResumeThreshold", UintegerValue (resumeThreshold),
                        "MinDynamicPauseThreshold", UintegerValue (minDynamicPauseThreshold),
                        "DynamicThresholdPacketBytes", UintegerValue (packetSize),
                        "DynamicPauseHeadroomFraction", DoubleValue (dynamicPauseHeadroomFraction),
                        "UseQcn", BooleanValue (useQcn),
                        "QcnMdFactor", DoubleValue (qcnMdFactor),
                        "QcnIntervalPackets", UintegerValue (qcnIntervalPackets));

  Ptr<SwitchSharedBufferPool> sharedPool = CreateObject<SwitchSharedBufferPool> ();
  sharedPool->SetMaxBytes (QueueSize (sharedBufferSize).GetValue ());

  Ptr<SwitchCongestionController> controller = CreateObject<SwitchCongestionController> ();
  ExperimentStats stats;
  AsciiTraceHelper ascii;
  std::ostringstream prefix;
  prefix << outputDir << "/qcn400g_" << simTag;

  Ptr<OutputStreamWrapper> queueStream = ascii.CreateFileStream (prefix.str () + ".queue.tr");
  Ptr<OutputStreamWrapper> pfcStream = ascii.CreateFileStream (prefix.str () + ".pfc.tr");
  Ptr<OutputStreamWrapper> qcnStream = ascii.CreateFileStream (prefix.str () + ".qcn.tr");
  Ptr<OutputStreamWrapper> ecnStream = ascii.CreateFileStream (prefix.str () + ".ecn.tr");
  Ptr<OutputStreamWrapper> poolStream = ascii.CreateFileStream (prefix.str () + ".shared-pool.tr");
  Ptr<OutputStreamWrapper> rateStream = ascii.CreateFileStream (prefix.str () + ".rate.tr");
  Ptr<OutputStreamWrapper> pauseStream = ascii.CreateFileStream (prefix.str () + ".app-pause.tr");
  Ptr<OutputStreamWrapper> flowStream = ascii.CreateFileStream (prefix.str () + ".flow-samples.tr");

  sharedPool->TraceConnectWithoutContext ("Occupancy",
                                          MakeBoundCallback (&TraceSharedPool,
                                                             poolStream,
                                                             &stats,
                                                             tracePoolEvents));

  std::array<Ptr<SwitchSharedBufferQueueDisc>, 4> qdiscs;
  for (uint32_t i = 0; i < 4; ++i)
    {
      QueueDiscContainer installed = tch.Install (NetDeviceContainer (links[i].Get (0)));
      NS_ABORT_MSG_IF (installed.GetN () == 0,
                       "Switch queue-disc installation returned an empty container for egress "
                           << i);
      qdiscs[i] = DynamicCast<SwitchSharedBufferQueueDisc> (installed.Get (0));
      NS_ABORT_MSG_IF (qdiscs[i] == 0,
                       "Installed root queue disc is not SwitchSharedBufferQueueDisc on egress "
                           << i);
      qdiscs[i]->SetAttribute ("EgressId", UintegerValue (i));
      qdiscs[i]->SetSharedBufferPool (sharedPool);
      qdiscs[i]->SetPauseCallback (MakeCallback (&SwitchCongestionController::NotifyPause, controller));
      qdiscs[i]->SetResumeCallback (MakeCallback (&SwitchCongestionController::NotifyResume, controller));
      qdiscs[i]->SetQcnCallback (MakeCallback (&SwitchCongestionController::NotifyQcn, controller));
      qdiscs[i]->TraceConnectWithoutContext ("QueueState",
                                             MakeBoundCallback (&TraceQueueState,
                                                                queueStream,
                                                                &stats,
                                                                traceQueueEvents));
      qdiscs[i]->TraceConnectWithoutContext ("PfcEvent",
                                             MakeBoundCallback (&TracePfcEvent,
                                                                pfcStream,
                                                                &stats,
                                                                traceControlEvents));
      qdiscs[i]->TraceConnectWithoutContext ("QcnEvent",
                                             MakeBoundCallback (&TraceQcnEvent,
                                                                qcnStream,
                                                                &stats,
                                                                traceControlEvents));
      qdiscs[i]->TraceConnectWithoutContext ("EcnEvent",
                                             MakeBoundCallback (&TraceEcnEvent,
                                                                ecnStream,
                                                                &stats,
                                                                traceControlEvents));
      qdiscs[i]->TraceConnectWithoutContext ("EcnAttempt",
                                             MakeBoundCallback (&TraceEcnAttempt,
                                                                ecnStream,
                                                                &stats,
                                                                traceControlEvents));
    }

  Ipv4AddressHelper ipv4;
  std::array<Ipv4InterfaceContainer, 4> ifaces;
  for (uint32_t i = 0; i < 4; ++i)
    {
      std::ostringstream subnet;
      subnet << "10.0." << (i + 1) << ".0";
      ipv4.SetBase (subnet.str ().c_str (), "255.255.255.0");
      ifaces[i] = ipv4.Assign (links[i]);
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  const std::vector<FlowConfig> configs = {
    {1, 0, 10000, 10000, 100000000, startTime, 0.01},
    {2, 0, 10001, 10001, 100000000, startTime, 0.01},
    {3, 0, 10002, 10002, 100000000, startTime, 1.0},
    {0, 1, 10003, 10003, 100000000, startTime, 0.5},
    {2, 1, 10004, 10004, 100000000, startTime, 0.5},
    {3, 1, 10005, 10005, 100000000, startTime, 0.01},
    {0, 2, 10006, 10006, 100000000, startTime, 0.5},
    {1, 2, 10007, 10007, 100000000, startTime, 0.5},
    {3, 2, 10008, 10008, 100000000, startTime, 0.01},
    {0, 3, 10009, 10009, 100000000, startTime, 0.01},
    {1, 3, 10010, 10010, 100000000, startTime, 0.5},
    {2, 3, 10011, 10011, 100000000, startTime, 0.5},
  };

  std::vector<FlowRuntime> flows;
  flows.reserve (configs.size ());

  for (const FlowConfig& flow : configs)
    {
      const Ipv4Address dstAddr = ifaces[flow.dstHost].GetAddress (1);
      InetSocketAddress remote (dstAddr, flow.port);
      remote.SetTos (0x02);

      PacketSinkHelper sinkHelper ("ns3::UdpSocketFactory",
                                   InetSocketAddress (Ipv4Address::GetAny (), flow.port));
      ApplicationContainer sinkApp = sinkHelper.Install (hosts.Get (flow.dstHost));
      sinkApp.Start (Seconds (0.0));
      sinkApp.Stop (Seconds (stopTime));
      Ptr<PacketSink> sink = DynamicCast<PacketSink> (sinkApp.Get (0));

      Ptr<RateControlledFlowApp> app = CreateObject<RateControlledFlowApp> ();
      double initialRateGbps = flow.initialRateFraction * linkRateGbps;
      app->SetAttribute ("Remote", AddressValue (remote));
      app->SetAttribute ("Protocol", TypeIdValue (UdpSocketFactory::GetTypeId ()));
      app->SetAttribute ("PacketSize", UintegerValue (packetSize));
      app->SetAttribute ("MaxBytes", UintegerValue (flow.bytes));
      app->SetAttribute ("InitialRate", DataRateValue (Gbps (initialRateGbps)));
      app->SetAttribute ("MinRate", DataRateValue (Gbps (0.1)));
      app->SetAttribute ("MaxRate", DataRateValue (Gbps (initialRateGbps)));
      app->SetAttribute ("AiStep", DataRateValue (Gbps (aiStepGbps)));
      app->SetAttribute ("AiInterval", TimeValue (MicroSeconds (aiIntervalUs)));
      app->SetAttribute ("IpTos", UintegerValue (0x02));
      app->SetAttribute ("FlowId", UintegerValue (flow.flowId));
      app->SetAttribute ("SourceId", UintegerValue (flow.srcHost));
      hosts.Get (flow.srcHost)->AddApplication (app);
      app->SetStartTime (Seconds (flow.startSec));
      app->SetStopTime (Seconds (stopTime));

      app->TraceConnectWithoutContext ("RateChange",
                                       MakeBoundCallback (&TraceAppRate,
                                                          rateStream,
                                                          flow.flowId));
      app->TraceConnectWithoutContext ("PauseState",
                                       MakeBoundCallback (&TraceAppPause,
                                                          pauseStream,
                                                          flow.flowId));

      controller->RegisterFlow (flow.srcHost,
                                flow.flowId,
                                MakeCallback (&RateControlledFlowApp::PauseFlow, app),
                                MakeCallback (&RateControlledFlowApp::ResumeFlow, app),
                                MakeCallback (&RateControlledFlowApp::ApplyMultiplicativeDecrease, app));

      flows.push_back ({flow, app, sink});
    }

  Time stop = Seconds (stopTime);
  Simulator::Schedule (MicroSeconds (sampleIntervalUs),
                       &SampleFlows,
                       flowStream,
                       &flows,
                       MicroSeconds (sampleIntervalUs),
                       stop);

  Simulator::Stop (stop);
  Simulator::Run ();

  WriteSummary (prefix.str () + ".summary.csv",
                flows,
                stats,
                linkRateGbps,
                stopTime,
                packetSize,
                deviceMtu,
                sharedBufferSize,
                kmin,
                kmax,
                pmax);

  Simulator::Destroy ();
  return 0;
}
