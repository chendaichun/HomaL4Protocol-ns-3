/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Basic control-surface tests for RateControlledFlowApp.
 */

#include "ns3/test.h"
#include "ns3/rate-controlled-flow-app.h"
#include "ns3/data-rate.h"
#include "ns3/uinteger.h"

using namespace ns3;

class RateControlledFlowAppControlTestCase : public TestCase
{
public:
  RateControlledFlowAppControlTestCase ()
    : TestCase ("Sanity check on the rate-controlled flow app control surface")
  {
  }

  void DoRun (void) override
  {
    Ptr<RateControlledFlowApp> app = CreateObject<RateControlledFlowApp> ();

    NS_TEST_EXPECT_MSG_EQ (app->SetAttributeFailSafe ("InitialRate",
                                                      DataRateValue (DataRate ("200Gbps"))),
                           true,
                           "The application should expose InitialRate");
    NS_TEST_EXPECT_MSG_EQ (app->SetAttributeFailSafe ("MinRate",
                                                      DataRateValue (DataRate ("4Gbps"))),
                           true,
                           "The application should expose MinRate");
    NS_TEST_EXPECT_MSG_EQ (app->SetAttributeFailSafe ("MaxRate",
                                                      DataRateValue (DataRate ("400Gbps"))),
                           true,
                           "The application should expose MaxRate");
    NS_TEST_EXPECT_MSG_EQ (app->SetAttributeFailSafe ("FlowId", UintegerValue (11)),
                           true,
                           "The application should expose FlowId");
    NS_TEST_EXPECT_MSG_EQ (app->SetAttributeFailSafe ("SourceId", UintegerValue (2)),
                           true,
                           "The application should expose SourceId");
    NS_TEST_EXPECT_MSG_EQ (app->GetCurrentRate ().GetBitRate (),
                           static_cast<uint64_t> (200000000000ULL),
                           "The current rate should track the configured initial rate");

    app->ApplyMultiplicativeDecrease (0.5);
    NS_TEST_EXPECT_MSG_EQ (app->GetCurrentRate ().GetBitRate (),
                           static_cast<uint64_t> (100000000000ULL),
                           "MD should reduce the current rate");

    app->PauseFlow ();
    NS_TEST_EXPECT_MSG_EQ (app->IsPaused (), true, "PauseFlow should pause the app");

    app->ResumeFlow ();
    NS_TEST_EXPECT_MSG_EQ (app->IsPaused (), false, "ResumeFlow should resume the app");
  }
};

static class RateControlledFlowAppTestSuite : public TestSuite
{
public:
  RateControlledFlowAppTestSuite ()
    : TestSuite ("rate-controlled-flow-app", UNIT)
  {
    AddTestCase (new RateControlledFlowAppControlTestCase (), TestCase::QUICK);
  }
} g_rateControlledFlowAppTestSuite;
