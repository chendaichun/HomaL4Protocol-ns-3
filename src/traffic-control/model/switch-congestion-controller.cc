/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "switch-congestion-controller.h"

#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("SwitchCongestionController");

NS_OBJECT_ENSURE_REGISTERED (SwitchCongestionController);

TypeId
SwitchCongestionController::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SwitchCongestionController")
    .SetParent<Object> ()
    .SetGroupName ("TrafficControl")
    .AddConstructor<SwitchCongestionController> ();
  return tid;
}

SwitchCongestionController::SwitchCongestionController ()
{
  NS_LOG_FUNCTION (this);
}

SwitchCongestionController::~SwitchCongestionController ()
{
  NS_LOG_FUNCTION (this);
}

void
SwitchCongestionController::RegisterIngress (uint32_t ingressId,
                                             PauseCallback pause,
                                             ResumeCallback resume,
                                             QcnCallback qcn)
{
  NS_LOG_FUNCTION (this << ingressId);
  m_handlers[ingressId].push_back ({pause, resume, qcn, false});
}

void
SwitchCongestionController::RegisterFlow (uint32_t ingressId,
                                          uint32_t flowId,
                                          PauseCallback pause,
                                          ResumeCallback resume,
                                          QcnCallback qcn)
{
  NS_LOG_FUNCTION (this << ingressId << flowId);
  m_handlers[ingressId].push_back ({pause, resume, qcn, false});
  m_flowHandlers[ingressId][flowId].push_back ({pause, resume, qcn, false});
}

void
SwitchCongestionController::NotifyPause (uint32_t ingressId)
{
  NS_LOG_FUNCTION (this << ingressId);
  uint32_t& refcount = m_pauseRefcounts[ingressId];
  refcount++;
  if (refcount > 1)
    {
      return;
    }

  auto it = m_handlers.find (ingressId);
  if (it != m_handlers.end ())
    {
      for (HandlerSet& handler : it->second)
        {
          PauseHandler (handler);
        }
    }
}

void
SwitchCongestionController::NotifyResume (uint32_t ingressId)
{
  NS_LOG_FUNCTION (this << ingressId);
  auto pauseIt = m_pauseRefcounts.find (ingressId);
  if (pauseIt != m_pauseRefcounts.end () && pauseIt->second > 0)
    {
      pauseIt->second--;
      if (pauseIt->second > 0)
        {
          return;
        }
      m_pauseRefcounts.erase (pauseIt);
    }

  auto it = m_handlers.find (ingressId);
  if (it != m_handlers.end ())
    {
      for (HandlerSet& handler : it->second)
        {
          ResumeHandler (handler);
        }
    }
}

void
SwitchCongestionController::NotifyQcn (uint32_t ingressId, uint32_t flowId, double factor)
{
  NS_LOG_FUNCTION (this << ingressId << flowId << factor);
  auto ingressIt = m_flowHandlers.find (ingressId);
  if (ingressIt == m_flowHandlers.end ())
    {
      NotifyAllIngressFlows (ingressId, factor);
      return;
    }

  auto flowIt = ingressIt->second.find (flowId);
  if (flowIt == ingressIt->second.end ())
    {
      NotifyAllIngressFlows (ingressId, factor);
      return;
    }

  for (const HandlerSet& handler : flowIt->second)
    {
      if (!handler.qcn.IsNull ())
        {
          handler.qcn (factor);
        }
    }
}

uint32_t
SwitchCongestionController::GetRegisteredHandlerCount (uint32_t ingressId) const
{
  auto it = m_handlers.find (ingressId);
  return it == m_handlers.end () ? 0 : static_cast<uint32_t> (it->second.size ());
}

void
SwitchCongestionController::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_handlers.clear ();
  m_flowHandlers.clear ();
  m_pauseRefcounts.clear ();
  Object::DoDispose ();
}

void
SwitchCongestionController::PauseHandler (HandlerSet& handler)
{
  if (!handler.paused && !handler.pause.IsNull ())
    {
      handler.pause ();
      handler.paused = true;
    }
}

void
SwitchCongestionController::ResumeHandler (HandlerSet& handler)
{
  if (handler.paused && !handler.resume.IsNull ())
    {
      handler.resume ();
      handler.paused = false;
    }
}

void
SwitchCongestionController::NotifyAllIngressFlows (uint32_t ingressId, double factor)
{
  auto it = m_handlers.find (ingressId);
  if (it == m_handlers.end ())
    {
      return;
    }

  for (const HandlerSet& handler : it->second)
    {
      if (!handler.qcn.IsNull ())
        {
          handler.qcn (factor);
        }
    }
}

} // namespace ns3
