/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Shared buffer accounting object used by switch queue discs installed on the
 * same switch. It keeps the global byte occupancy separate from per-egress
 * queue-disc state.
 */

#ifndef SWITCH_SHARED_BUFFER_POOL_H
#define SWITCH_SHARED_BUFFER_POOL_H

#include "ns3/object.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <unordered_map>

namespace ns3 {

class SwitchSharedBufferPool : public Object
{
public:
  static TypeId GetTypeId (void);

  SwitchSharedBufferPool ();
  ~SwitchSharedBufferPool () override;

  void SetMaxBytes (uint64_t maxBytes);
  uint64_t GetMaxBytes (void) const;
  uint64_t GetUsedBytes (void) const;
  uint64_t GetEgressBytes (uint32_t egressId) const;

  bool Reserve (uint32_t egressId, uint32_t bytes);
  void Release (uint32_t egressId, uint32_t bytes);

protected:
  void DoDispose (void) override;

private:
  uint64_t m_maxBytes; //!< Shared buffer capacity across all attached egresses.
  uint64_t m_usedBytes; //!< Currently occupied shared-buffer bytes.
  std::unordered_map<uint32_t, uint64_t> m_egressBytes; //!< Per-egress occupancy.

  TracedCallback<uint64_t, uint64_t> m_occupancyTrace; //!< usedBytes, maxBytes.
};

} // namespace ns3

#endif /* SWITCH_SHARED_BUFFER_POOL_H */
