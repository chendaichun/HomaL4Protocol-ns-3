/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "switch-shared-buffer-pool.h"

#include "ns3/log.h"
#include "ns3/uinteger.h"

#include <algorithm>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("SwitchSharedBufferPool");

NS_OBJECT_ENSURE_REGISTERED (SwitchSharedBufferPool);

TypeId
SwitchSharedBufferPool::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SwitchSharedBufferPool")
    .SetParent<Object> ()
    .SetGroupName ("TrafficControl")
    .AddConstructor<SwitchSharedBufferPool> ()
    .AddAttribute ("MaxBytes",
                   "Shared buffer capacity in bytes.",
                   UintegerValue (82000000),
                   MakeUintegerAccessor (&SwitchSharedBufferPool::SetMaxBytes,
                                         &SwitchSharedBufferPool::GetMaxBytes),
                   MakeUintegerChecker<uint64_t> ())
    .AddTraceSource ("Occupancy",
                     "Shared buffer occupancy update: usedBytes, maxBytes.",
                     MakeTraceSourceAccessor (&SwitchSharedBufferPool::m_occupancyTrace),
                     "ns3::TracedCallback::Uint64Uint64");
  return tid;
}

SwitchSharedBufferPool::SwitchSharedBufferPool ()
  : m_maxBytes (82000000),
    m_usedBytes (0)
{
  NS_LOG_FUNCTION (this);
}

SwitchSharedBufferPool::~SwitchSharedBufferPool ()
{
  NS_LOG_FUNCTION (this);
}

void
SwitchSharedBufferPool::SetMaxBytes (uint64_t maxBytes)
{
  NS_LOG_FUNCTION (this << maxBytes);
  m_maxBytes = maxBytes;
}

uint64_t
SwitchSharedBufferPool::GetMaxBytes (void) const
{
  return m_maxBytes;
}

uint64_t
SwitchSharedBufferPool::GetUsedBytes (void) const
{
  return m_usedBytes;
}

uint64_t
SwitchSharedBufferPool::GetEgressBytes (uint32_t egressId) const
{
  auto it = m_egressBytes.find (egressId);
  return it == m_egressBytes.end () ? 0 : it->second;
}

bool
SwitchSharedBufferPool::Reserve (uint32_t egressId, uint32_t bytes)
{
  NS_LOG_FUNCTION (this << egressId << bytes);
  if (m_usedBytes + bytes > m_maxBytes)
    {
      return false;
    }

  m_usedBytes += bytes;
  m_egressBytes[egressId] += bytes;
  m_occupancyTrace (m_usedBytes, m_maxBytes);
  return true;
}

void
SwitchSharedBufferPool::Release (uint32_t egressId, uint32_t bytes)
{
  NS_LOG_FUNCTION (this << egressId << bytes);
  auto it = m_egressBytes.find (egressId);
  if (it != m_egressBytes.end ())
    {
      uint64_t released = std::min<uint64_t> (it->second, bytes);
      it->second -= released;
      m_usedBytes -= std::min<uint64_t> (m_usedBytes, released);
      if (it->second == 0)
        {
          m_egressBytes.erase (it);
        }
    }
  m_occupancyTrace (m_usedBytes, m_maxBytes);
}

void
SwitchSharedBufferPool::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  m_egressBytes.clear ();
  Object::DoDispose ();
}

} // namespace ns3
