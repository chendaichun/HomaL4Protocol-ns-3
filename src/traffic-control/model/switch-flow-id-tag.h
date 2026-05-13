/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Packet tag used by switch-control experiments to map packets back to sender
 * and flow state without making traffic-control depend on applications.
 */

#ifndef SWITCH_FLOW_ID_TAG_H
#define SWITCH_FLOW_ID_TAG_H

#include "ns3/tag.h"

namespace ns3 {

class SwitchFlowIdTag : public Tag
{
public:
  static TypeId GetTypeId (void);
  TypeId GetInstanceTypeId (void) const override;

  SwitchFlowIdTag ();
  SwitchFlowIdTag (uint32_t flowId, uint32_t sourceId);

  uint32_t GetSerializedSize (void) const override;
  void Serialize (TagBuffer buf) const override;
  void Deserialize (TagBuffer buf) override;
  void Print (std::ostream& os) const override;

  void SetFlowId (uint32_t flowId);
  uint32_t GetFlowId (void) const;

  void SetSourceId (uint32_t sourceId);
  uint32_t GetSourceId (void) const;

private:
  uint32_t m_flowId;   //!< Scenario-level flow identifier.
  uint32_t m_sourceId; //!< Scenario-level source/ingress identifier.
};

} // namespace ns3

#endif /* SWITCH_FLOW_ID_TAG_H */
