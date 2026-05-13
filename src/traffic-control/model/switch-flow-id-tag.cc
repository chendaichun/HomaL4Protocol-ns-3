/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "switch-flow-id-tag.h"

#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("SwitchFlowIdTag");

NS_OBJECT_ENSURE_REGISTERED (SwitchFlowIdTag);

TypeId
SwitchFlowIdTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SwitchFlowIdTag")
    .SetParent<Tag> ()
    .SetGroupName ("TrafficControl")
    .AddConstructor<SwitchFlowIdTag> ();
  return tid;
}

TypeId
SwitchFlowIdTag::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

SwitchFlowIdTag::SwitchFlowIdTag ()
  : m_flowId (0),
    m_sourceId (0)
{
  NS_LOG_FUNCTION (this);
}

SwitchFlowIdTag::SwitchFlowIdTag (uint32_t flowId, uint32_t sourceId)
  : m_flowId (flowId),
    m_sourceId (sourceId)
{
  NS_LOG_FUNCTION (this << flowId << sourceId);
}

uint32_t
SwitchFlowIdTag::GetSerializedSize (void) const
{
  return 8;
}

void
SwitchFlowIdTag::Serialize (TagBuffer buf) const
{
  buf.WriteU32 (m_flowId);
  buf.WriteU32 (m_sourceId);
}

void
SwitchFlowIdTag::Deserialize (TagBuffer buf)
{
  m_flowId = buf.ReadU32 ();
  m_sourceId = buf.ReadU32 ();
}

void
SwitchFlowIdTag::Print (std::ostream& os) const
{
  os << "flowId=" << m_flowId << " sourceId=" << m_sourceId;
}

void
SwitchFlowIdTag::SetFlowId (uint32_t flowId)
{
  m_flowId = flowId;
}

uint32_t
SwitchFlowIdTag::GetFlowId (void) const
{
  return m_flowId;
}

void
SwitchFlowIdTag::SetSourceId (uint32_t sourceId)
{
  m_sourceId = sourceId;
}

uint32_t
SwitchFlowIdTag::GetSourceId (void) const
{
  return m_sourceId;
}

} // namespace ns3
