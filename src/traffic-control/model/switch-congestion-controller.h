/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Callback-based controller that bridges switch congestion events to senders.
 */

#ifndef SWITCH_CONGESTION_CONTROLLER_H
#define SWITCH_CONGESTION_CONTROLLER_H

#include <unordered_map>
#include <vector>

#include "ns3/callback.h"
#include "ns3/object.h"

namespace ns3 {

class SwitchCongestionController : public Object
{
public:
  typedef Callback<void> PauseCallback;
  typedef Callback<void> ResumeCallback;
  typedef Callback<void, double> QcnCallback;

  static TypeId GetTypeId (void);

  SwitchCongestionController ();
  ~SwitchCongestionController () override;

  void RegisterIngress (uint32_t ingressId,
                        PauseCallback pause,
                        ResumeCallback resume,
                        QcnCallback qcn);
  void RegisterFlow (uint32_t ingressId,
                     uint32_t flowId,
                     PauseCallback pause,
                     ResumeCallback resume,
                     QcnCallback qcn);

  void NotifyPause (uint32_t ingressId);
  void NotifyResume (uint32_t ingressId);
  void NotifyQcn (uint32_t ingressId, uint32_t flowId, double factor);
  uint32_t GetRegisteredHandlerCount (uint32_t ingressId) const;

protected:
  void DoDispose (void) override;

private:
  struct HandlerSet
  {
    PauseCallback pause;
    ResumeCallback resume;
    QcnCallback qcn;
    bool paused;
  };

  typedef std::unordered_map<uint32_t, std::vector<HandlerSet>> FlowHandlers;

  void PauseHandler (HandlerSet& handler);
  void ResumeHandler (HandlerSet& handler);
  void NotifyAllIngressFlows (uint32_t ingressId, double factor);

  std::unordered_map<uint32_t, std::vector<HandlerSet>> m_handlers;
  std::unordered_map<uint32_t, FlowHandlers> m_flowHandlers;
  std::unordered_map<uint32_t, uint32_t> m_pauseRefcounts;
};

} // namespace ns3

#endif /* SWITCH_CONGESTION_CONTROLLER_H */
