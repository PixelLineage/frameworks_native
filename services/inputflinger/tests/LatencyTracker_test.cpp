/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../dispatcher/LatencyTracker.h"
#include "../InputDeviceMetricsSource.h"

#include <android-base/properties.h>
#include <binder/Binder.h>
#include <gtest/gtest.h>
#include <inttypes.h>
#include <linux/input.h>
#include <log/log.h>

#define TAG "LatencyTracker_test"

using android::base::HwTimeoutMultiplier;
using android::inputdispatcher::InputEventTimeline;
using android::inputdispatcher::LatencyTracker;

namespace android::inputdispatcher {

namespace {

constexpr DeviceId DEVICE_ID = 100;

static InputDeviceInfo generateTestDeviceInfo(uint16_t vendorId, uint16_t productId,
                                              DeviceId deviceId) {
    InputDeviceIdentifier identifier;
    identifier.vendor = vendorId;
    identifier.product = productId;
    auto info = InputDeviceInfo();
    info.initialize(deviceId, /*generation=*/1, /*controllerNumber=*/1, identifier, "Test Device",
                    /*isExternal=*/false, /*hasMic=*/false, ui::LogicalDisplayId::INVALID);
    return info;
}

void setDefaultInputDeviceInfo(LatencyTracker& tracker) {
    InputDeviceInfo deviceInfo = generateTestDeviceInfo(
            /*vendorId=*/0, /*productId=*/0, DEVICE_ID);
    tracker.setInputDevices({deviceInfo});
}

} // namespace

const std::chrono::duration ANR_TIMEOUT = std::chrono::milliseconds(
        android::os::IInputConstants::UNMULTIPLIED_DEFAULT_DISPATCHING_TIMEOUT_MILLIS *
        HwTimeoutMultiplier());

InputEventTimeline getTestTimeline() {
    InputEventTimeline t(
            /*eventTime=*/2,
            /*readTime=*/3,
            /*vendorId=*/0,
            /*productId=*/0,
            /*sources=*/{InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType=*/InputEventActionType::UNKNOWN_INPUT_EVENT);
    ConnectionTimeline expectedCT(/*deliveryTime=*/6, /*consumeTime=*/7, /*finishTime=*/8);
    std::array<nsecs_t, GraphicsTimeline::SIZE> graphicsTimeline;
    graphicsTimeline[GraphicsTimeline::GPU_COMPLETED_TIME] = 9;
    graphicsTimeline[GraphicsTimeline::PRESENT_TIME] = 10;
    expectedCT.setGraphicsTimeline(std::move(graphicsTimeline));
    t.connectionTimelines.emplace(sp<BBinder>::make(), std::move(expectedCT));
    return t;
}

// --- LatencyTrackerTest ---
class LatencyTrackerTest : public testing::Test, public InputEventTimelineProcessor {
protected:
    std::unique_ptr<LatencyTracker> mTracker;
    sp<IBinder> connection1;
    sp<IBinder> connection2;

    void SetUp() override {
        connection1 = sp<BBinder>::make();
        connection2 = sp<BBinder>::make();

        mTracker = std::make_unique<LatencyTracker>(this);
        setDefaultInputDeviceInfo(*mTracker);
    }
    void TearDown() override {}

    void triggerEventReporting(nsecs_t lastEventTime);

    void assertReceivedTimeline(const InputEventTimeline& timeline);
    /**
     * Timelines can be received in any order (order is not guaranteed). So if we are expecting more
     * than 1 timeline, use this function to check that the set of received timelines matches
     * what we expected.
     */
    void assertReceivedTimelines(const std::vector<InputEventTimeline>& timelines);

private:
    void processTimeline(const InputEventTimeline& timeline) override {
        mReceivedTimelines.push_back(timeline);
    }
    std::deque<InputEventTimeline> mReceivedTimelines;
};

/**
 * Send an event that would trigger the reporting of all of the events that are at least as old as
 * the provided 'lastEventTime'.
 */
void LatencyTrackerTest::triggerEventReporting(nsecs_t lastEventTime) {
    const nsecs_t triggerEventTime =
            lastEventTime + std::chrono::nanoseconds(ANR_TIMEOUT).count() + 1;
    mTracker->trackListener(/*inputEventId=*/1, triggerEventTime,
                            /*readTime=*/3, DEVICE_ID,
                            /*sources=*/{InputDeviceUsageSource::UNKNOWN},
                            AMOTION_EVENT_ACTION_CANCEL, InputEventType::MOTION);
}

void LatencyTrackerTest::assertReceivedTimeline(const InputEventTimeline& timeline) {
    ASSERT_FALSE(mReceivedTimelines.empty());
    const InputEventTimeline& t = mReceivedTimelines.front();
    ASSERT_EQ(timeline, t);
    mReceivedTimelines.pop_front();
}

/**
 * We are essentially comparing two multisets, but without constructing them.
 * This comparison is inefficient, but it avoids having to construct a set, and also avoids the
 * declaration of copy constructor for ConnectionTimeline.
 * We ensure that collections A and B have the same size, that for every element in A, there is an
 * equal element in B, and for every element in B there is an equal element in A.
 */
void LatencyTrackerTest::assertReceivedTimelines(const std::vector<InputEventTimeline>& timelines) {
    ASSERT_EQ(timelines.size(), mReceivedTimelines.size());
    for (const InputEventTimeline& expectedTimeline : timelines) {
        bool found = false;
        for (const InputEventTimeline& receivedTimeline : mReceivedTimelines) {
            if (receivedTimeline == expectedTimeline) {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found) << "Could not find expected timeline with eventTime="
                           << expectedTimeline.eventTime;
    }
    for (const InputEventTimeline& receivedTimeline : mReceivedTimelines) {
        bool found = false;
        for (const InputEventTimeline& expectedTimeline : timelines) {
            if (receivedTimeline == expectedTimeline) {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found) << "Could not find received timeline with eventTime="
                           << receivedTimeline.eventTime;
    }
    mReceivedTimelines.clear();
}

/**
 * Ensure that calling 'trackListener' in isolation only creates an inputflinger timeline, without
 * any additional ConnectionTimeline's.
 */
TEST_F(LatencyTrackerTest, TrackListener_DoesNotTriggerReporting) {
    mTracker->trackListener(/*inputEventId=*/1, /*eventTime=*/2,
                            /*readTime=*/3, DEVICE_ID, {InputDeviceUsageSource::UNKNOWN},
                            AMOTION_EVENT_ACTION_CANCEL, InputEventType::MOTION);
    triggerEventReporting(/*eventTime=*/2);
    assertReceivedTimeline(
            InputEventTimeline{/*eventTime=*/2,
                               /*readTime=*/3, /*vendorId=*/0, /*productID=*/0,
                               /*sources=*/{InputDeviceUsageSource::UNKNOWN},
                               /*inputEventActionType=*/InputEventActionType::UNKNOWN_INPUT_EVENT});
}

/**
 * A single call to trackFinishedEvent should not cause a timeline to be reported.
 */
TEST_F(LatencyTrackerTest, TrackFinishedEvent_DoesNotTriggerReporting) {
    mTracker->trackFinishedEvent(/*inputEventId=*/1, connection1, /*deliveryTime=*/2,
                                 /*consumeTime=*/3, /*finishTime=*/4);
    triggerEventReporting(/*eventTime=*/4);
    assertReceivedTimelines({});
}

/**
 * A single call to trackGraphicsLatency should not cause a timeline to be reported.
 */
TEST_F(LatencyTrackerTest, TrackGraphicsLatency_DoesNotTriggerReporting) {
    std::array<nsecs_t, GraphicsTimeline::SIZE> graphicsTimeline;
    graphicsTimeline[GraphicsTimeline::GPU_COMPLETED_TIME] = 2;
    graphicsTimeline[GraphicsTimeline::PRESENT_TIME] = 3;
    mTracker->trackGraphicsLatency(/*inputEventId=*/1, connection2, graphicsTimeline);
    triggerEventReporting(/*eventTime=*/3);
    assertReceivedTimelines({});
}

TEST_F(LatencyTrackerTest, TrackAllParameters_ReportsFullTimeline) {
    constexpr int32_t inputEventId = 1;
    InputEventTimeline expected = getTestTimeline();

    const auto& [connectionToken, expectedCT] = *expected.connectionTimelines.begin();

    mTracker->trackListener(inputEventId, expected.eventTime, expected.readTime, DEVICE_ID,
                            {InputDeviceUsageSource::UNKNOWN}, AMOTION_EVENT_ACTION_CANCEL,
                            InputEventType::MOTION);
    mTracker->trackFinishedEvent(inputEventId, connectionToken, expectedCT.deliveryTime,
                                 expectedCT.consumeTime, expectedCT.finishTime);
    mTracker->trackGraphicsLatency(inputEventId, connectionToken, expectedCT.graphicsTimeline);

    triggerEventReporting(expected.eventTime);
    assertReceivedTimeline(expected);
}

/**
 * Send 2 events with the same inputEventId, but different eventTime's. Ensure that no crash occurs,
 * and that the tracker drops such events completely.
 */
TEST_F(LatencyTrackerTest, WhenDuplicateEventsAreReported_DoesNotCrash) {
    constexpr nsecs_t inputEventId = 1;
    constexpr nsecs_t readTime = 3; // does not matter for this test

    // In the following 2 calls to trackListener, the inputEventId's are the same, but event times
    // are different.
    mTracker->trackListener(inputEventId, /*eventTime=*/1, readTime, DEVICE_ID,
                            {InputDeviceUsageSource::UNKNOWN}, AMOTION_EVENT_ACTION_CANCEL,
                            InputEventType::MOTION);
    mTracker->trackListener(inputEventId, /*eventTime=*/2, readTime, DEVICE_ID,
                            {InputDeviceUsageSource::UNKNOWN}, AMOTION_EVENT_ACTION_CANCEL,
                            InputEventType::MOTION);

    triggerEventReporting(/*eventTime=*/2);
    // Since we sent duplicate input events, the tracker should just delete all of them, because it
    // does not have enough information to properly track them.
    assertReceivedTimelines({});
}

TEST_F(LatencyTrackerTest, MultipleEvents_AreReportedConsistently) {
    constexpr int32_t inputEventId1 = 1;
    InputEventTimeline timeline1(
            /*eventTime*/ 2,
            /*readTime*/ 3,
            /*vendorId=*/0,
            /*productId=*/0,
            /*sources=*/{InputDeviceUsageSource::UNKNOWN},
            /*inputEventType=*/InputEventActionType::UNKNOWN_INPUT_EVENT);
    timeline1.connectionTimelines.emplace(connection1,
                                          ConnectionTimeline(/*deliveryTime*/ 6, /*consumeTime*/ 7,
                                                             /*finishTime*/ 8));
    ConnectionTimeline& connectionTimeline1 = timeline1.connectionTimelines.begin()->second;
    std::array<nsecs_t, GraphicsTimeline::SIZE> graphicsTimeline1;
    graphicsTimeline1[GraphicsTimeline::GPU_COMPLETED_TIME] = 9;
    graphicsTimeline1[GraphicsTimeline::PRESENT_TIME] = 10;
    connectionTimeline1.setGraphicsTimeline(std::move(graphicsTimeline1));

    constexpr int32_t inputEventId2 = 10;
    InputEventTimeline timeline2(
            /*eventTime=*/20,
            /*readTime=*/30,
            /*vendorId=*/0,
            /*productId=*/0,
            /*sources=*/{InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType=*/InputEventActionType::UNKNOWN_INPUT_EVENT);
    timeline2.connectionTimelines.emplace(connection2,
                                          ConnectionTimeline(/*deliveryTime=*/60,
                                                             /*consumeTime=*/70,
                                                             /*finishTime=*/80));
    ConnectionTimeline& connectionTimeline2 = timeline2.connectionTimelines.begin()->second;
    std::array<nsecs_t, GraphicsTimeline::SIZE> graphicsTimeline2;
    graphicsTimeline2[GraphicsTimeline::GPU_COMPLETED_TIME] = 90;
    graphicsTimeline2[GraphicsTimeline::PRESENT_TIME] = 100;
    connectionTimeline2.setGraphicsTimeline(std::move(graphicsTimeline2));

    // Start processing first event
    mTracker->trackListener(inputEventId1, timeline1.eventTime, timeline1.readTime, DEVICE_ID,
                            {InputDeviceUsageSource::UNKNOWN}, AMOTION_EVENT_ACTION_CANCEL,
                            InputEventType::MOTION);
    // Start processing second event
    mTracker->trackListener(inputEventId2, timeline2.eventTime, timeline2.readTime, DEVICE_ID,
                            {InputDeviceUsageSource::UNKNOWN}, AMOTION_EVENT_ACTION_CANCEL,
                            InputEventType::MOTION);
    mTracker->trackFinishedEvent(inputEventId1, connection1, connectionTimeline1.deliveryTime,
                                 connectionTimeline1.consumeTime, connectionTimeline1.finishTime);

    mTracker->trackFinishedEvent(inputEventId2, connection2, connectionTimeline2.deliveryTime,
                                 connectionTimeline2.consumeTime, connectionTimeline2.finishTime);
    mTracker->trackGraphicsLatency(inputEventId1, connection1,
                                   connectionTimeline1.graphicsTimeline);
    mTracker->trackGraphicsLatency(inputEventId2, connection2,
                                   connectionTimeline2.graphicsTimeline);
    // Now both events should be completed
    triggerEventReporting(timeline2.eventTime);
    assertReceivedTimelines({timeline1, timeline2});
}

/**
 * Check that LatencyTracker consistently tracks events even if there are many incomplete events.
 */
TEST_F(LatencyTrackerTest, IncompleteEvents_AreHandledConsistently) {
    InputEventTimeline timeline = getTestTimeline();
    std::vector<InputEventTimeline> expectedTimelines;
    const ConnectionTimeline& expectedCT = timeline.connectionTimelines.begin()->second;
    const sp<IBinder>& token = timeline.connectionTimelines.begin()->first;

    for (size_t i = 1; i <= 100; i++) {
        mTracker->trackListener(/*inputEventId=*/i, timeline.eventTime, timeline.readTime,
                                /*deviceId=*/DEVICE_ID,
                                /*sources=*/{InputDeviceUsageSource::UNKNOWN},
                                AMOTION_EVENT_ACTION_CANCEL, InputEventType::MOTION);
        expectedTimelines.push_back(InputEventTimeline{timeline.eventTime, timeline.readTime,
                                                       timeline.vendorId, timeline.productId,
                                                       timeline.sources,
                                                       timeline.inputEventActionType});
    }
    // Now, complete the first event that was sent.
    mTracker->trackFinishedEvent(/*inputEventId=*/1, token, expectedCT.deliveryTime,
                                 expectedCT.consumeTime, expectedCT.finishTime);
    mTracker->trackGraphicsLatency(/*inputEventId=*/1, token, expectedCT.graphicsTimeline);

    expectedTimelines[0].connectionTimelines.emplace(token, std::move(expectedCT));
    triggerEventReporting(timeline.eventTime);
    assertReceivedTimelines(expectedTimelines);
}

/**
 * For simplicity of the implementation, LatencyTracker only starts tracking an event when
 * 'trackListener' is invoked.
 * Both 'trackFinishedEvent' and 'trackGraphicsLatency' should not start a new event.
 * If they are received before 'trackListener' (which should not be possible), they are ignored.
 */
TEST_F(LatencyTrackerTest, EventsAreTracked_WhenTrackListenerIsCalledFirst) {
    constexpr int32_t inputEventId = 1;
    InputEventTimeline expected = getTestTimeline();
    const ConnectionTimeline& expectedCT = expected.connectionTimelines.begin()->second;
    mTracker->trackFinishedEvent(inputEventId, connection1, expectedCT.deliveryTime,
                                 expectedCT.consumeTime, expectedCT.finishTime);
    mTracker->trackGraphicsLatency(inputEventId, connection1, expectedCT.graphicsTimeline);

    mTracker->trackListener(inputEventId, expected.eventTime, expected.readTime, DEVICE_ID,
                            {InputDeviceUsageSource::UNKNOWN}, AMOTION_EVENT_ACTION_CANCEL,
                            InputEventType::MOTION);
    triggerEventReporting(expected.eventTime);
    assertReceivedTimeline(InputEventTimeline{expected.eventTime, expected.readTime,
                                              expected.vendorId, expected.productId,
                                              expected.sources, expected.inputEventActionType});
}

/**
 * Check that LatencyTracker has the received timeline that contains the correctly
 * resolved product ID, vendor ID and source for a particular device ID from
 * among a list of devices.
 */
TEST_F(LatencyTrackerTest, TrackListenerCheck_DeviceInfoFieldsInputEventTimeline) {
    constexpr int32_t inputEventId = 1;
    InputEventTimeline timeline(
            /*eventTime*/ 2, /*readTime*/ 3,
            /*vendorId=*/50, /*productId=*/60,
            /*sources=*/
            {InputDeviceUsageSource::TOUCHSCREEN, InputDeviceUsageSource::STYLUS_DIRECT},
            /*inputEventActionType=*/InputEventActionType::UNKNOWN_INPUT_EVENT);
    InputDeviceInfo deviceInfo1 = generateTestDeviceInfo(
            /*vendorId=*/5, /*productId=*/6, /*deviceId=*/DEVICE_ID + 1);
    InputDeviceInfo deviceInfo2 = generateTestDeviceInfo(
            /*vendorId=*/50, /*productId=*/60, /*deviceId=*/DEVICE_ID);

    mTracker->setInputDevices({deviceInfo1, deviceInfo2});
    mTracker->trackListener(inputEventId, timeline.eventTime, timeline.readTime, DEVICE_ID,
                            {InputDeviceUsageSource::TOUCHSCREEN,
                             InputDeviceUsageSource::STYLUS_DIRECT},
                            AMOTION_EVENT_ACTION_CANCEL, InputEventType::MOTION);
    triggerEventReporting(timeline.eventTime);
    assertReceivedTimeline(timeline);
}

/**
 * Check that InputEventActionType is correctly assigned to InputEventTimeline in trackListener.
 */
TEST_F(LatencyTrackerTest, TrackListenerCheck_InputEventActionTypeFieldInputEventTimeline) {
    constexpr int32_t inputEventId = 1;
    // Create timelines for different event types (Motion, Key)
    InputEventTimeline motionDownTimeline(
            /*eventTime*/ 2, /*readTime*/ 3,
            /*vendorId*/ 0, /*productId*/ 0,
            /*sources*/ {InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType*/ InputEventActionType::MOTION_ACTION_DOWN);

    InputEventTimeline motionMoveTimeline(
            /*eventTime*/ 4, /*readTime*/ 5,
            /*vendorId*/ 0, /*productId*/ 0,
            /*sources*/ {InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType*/ InputEventActionType::MOTION_ACTION_MOVE);

    InputEventTimeline motionUpTimeline(
            /*eventTime*/ 6, /*readTime*/ 7,
            /*vendorId*/ 0, /*productId*/ 0,
            /*sources*/ {InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType*/ InputEventActionType::MOTION_ACTION_UP);

    InputEventTimeline keyDownTimeline(
            /*eventTime*/ 8, /*readTime*/ 9,
            /*vendorId*/ 0, /*productId*/ 0,
            /*sources*/ {InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType*/ InputEventActionType::KEY);

    InputEventTimeline keyUpTimeline(
            /*eventTime*/ 10, /*readTime*/ 11,
            /*vendorId*/ 0, /*productId*/ 0,
            /*sources*/ {InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType*/ InputEventActionType::KEY);

    InputEventTimeline unknownTimeline(
            /*eventTime*/ 12, /*readTime*/ 13,
            /*vendorId*/ 0, /*productId*/ 0,
            /*sources*/ {InputDeviceUsageSource::UNKNOWN},
            /*inputEventActionType*/ InputEventActionType::UNKNOWN_INPUT_EVENT);

    mTracker->trackListener(inputEventId, motionDownTimeline.eventTime, motionDownTimeline.readTime,
                            DEVICE_ID, motionDownTimeline.sources, AMOTION_EVENT_ACTION_DOWN,
                            InputEventType::MOTION);
    mTracker->trackListener(inputEventId + 1, motionMoveTimeline.eventTime,
                            motionMoveTimeline.readTime, DEVICE_ID, motionMoveTimeline.sources,
                            AMOTION_EVENT_ACTION_MOVE, InputEventType::MOTION);
    mTracker->trackListener(inputEventId + 2, motionUpTimeline.eventTime, motionUpTimeline.readTime,
                            DEVICE_ID, motionUpTimeline.sources, AMOTION_EVENT_ACTION_UP,
                            InputEventType::MOTION);
    mTracker->trackListener(inputEventId + 3, keyDownTimeline.eventTime, keyDownTimeline.readTime,
                            DEVICE_ID, keyDownTimeline.sources, AKEY_EVENT_ACTION_DOWN,
                            InputEventType::KEY);
    mTracker->trackListener(inputEventId + 4, keyUpTimeline.eventTime, keyUpTimeline.readTime,
                            DEVICE_ID, keyUpTimeline.sources, AKEY_EVENT_ACTION_UP,
                            InputEventType::KEY);
    mTracker->trackListener(inputEventId + 5, unknownTimeline.eventTime, unknownTimeline.readTime,
                            DEVICE_ID, unknownTimeline.sources, AMOTION_EVENT_ACTION_POINTER_DOWN,
                            InputEventType::MOTION);

    triggerEventReporting(unknownTimeline.eventTime);

    std::vector<InputEventTimeline> expectedTimelines = {motionDownTimeline, motionMoveTimeline,
                                                         motionUpTimeline,   keyDownTimeline,
                                                         keyUpTimeline,      unknownTimeline};
    assertReceivedTimelines(expectedTimelines);
}

} // namespace android::inputdispatcher
