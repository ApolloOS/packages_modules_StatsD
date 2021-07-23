/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "EventMetricProducer.h"

#include <limits.h>
#include <stdlib.h>

#include "flags/FlagProvider.h"
#include "metrics/parsing_utils/metrics_manager_util.h"
#include "stats_log_util.h"
#include "stats_util.h"

using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_BOOL;
using android::util::FIELD_TYPE_FLOAT;
using android::util::FIELD_TYPE_INT32;
using android::util::FIELD_TYPE_INT64;
using android::util::FIELD_TYPE_STRING;
using android::util::FIELD_TYPE_MESSAGE;
using android::util::ProtoOutputStream;
using std::map;
using std::string;
using std::unordered_map;
using std::vector;
using std::shared_ptr;

namespace android {
namespace os {
namespace statsd {

// for StatsLogReport
const int FIELD_ID_ID = 1;
const int FIELD_ID_EVENT_METRICS = 4;
const int FIELD_ID_IS_ACTIVE = 14;
// for EventMetricDataWrapper
const int FIELD_ID_DATA = 1;
// for EventMetricData
const int FIELD_ID_ELAPSED_TIMESTAMP_NANOS = 1;
const int FIELD_ID_ATOMS = 2;
const int FIELD_ID_AGGREGATED_ATOM = 4;
// for AggregatedAtomInfo
const int FIELD_ID_ATOM = 1;
const int FIELD_ID_ATOM_TIMESTAMPS = 2;

EventMetricProducer::EventMetricProducer(
        const ConfigKey& key, const EventMetric& metric, const int conditionIndex,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const uint64_t protoHash, const int64_t startTimeNs,
        const unordered_map<int, shared_ptr<Activation>>& eventActivationMap,
        const unordered_map<int, vector<shared_ptr<Activation>>>& eventDeactivationMap,
        const vector<int>& slicedStateAtoms,
        const unordered_map<int, unordered_map<int, int64_t>>& stateGroupMap)
    : MetricProducer(metric.id(), key, startTimeNs, conditionIndex, initialConditionCache, wizard,
                     protoHash, eventActivationMap, eventDeactivationMap, slicedStateAtoms,
                     stateGroupMap, /*splitBucketForAppUpgrade=*/false) {
    if (metric.links().size() > 0) {
        for (const auto& link : metric.links()) {
            Metric2Condition mc;
            mc.conditionId = link.condition();
            translateFieldMatcher(link.fields_in_what(), &mc.metricFields);
            translateFieldMatcher(link.fields_in_condition(), &mc.conditionFields);
            mMetric2ConditionLinks.push_back(mc);
        }
        mConditionSliced = true;
    }
    mProto = std::make_unique<ProtoOutputStream>();
    mUseAtomAggregation =
            FlagProvider::getInstance().getBootFlagBool(AGGREGATE_ATOMS_FLAG, FLAG_FALSE);
    VLOG("metric %lld created. bucket size %lld start_time: %lld", (long long)metric.id(),
         (long long)mBucketSizeNs, (long long)mTimeBaseNs);
}

EventMetricProducer::~EventMetricProducer() {
    VLOG("~EventMetricProducer() called");
}

bool EventMetricProducer::onConfigUpdatedLocked(
        const StatsdConfig& config, const int configIndex, const int metricIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const sp<EventMatcherWizard>& matcherWizard,
        const vector<sp<ConditionTracker>>& allConditionTrackers,
        const unordered_map<int64_t, int>& conditionTrackerMap, const sp<ConditionWizard>& wizard,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation) {
    if (!MetricProducer::onConfigUpdatedLocked(
                config, configIndex, metricIndex, allAtomMatchingTrackers,
                oldAtomMatchingTrackerMap, newAtomMatchingTrackerMap, matcherWizard,
                allConditionTrackers, conditionTrackerMap, wizard, metricToActivationMap,
                trackerToMetricMap, conditionToMetricMap, activationAtomTrackerToMetricMap,
                deactivationAtomTrackerToMetricMap, metricsWithActivation)) {
        return false;
    }

    const EventMetric& metric = config.event_metric(configIndex);
    int trackerIndex;
    // Update appropriate indices, specifically mConditionIndex and MetricsManager maps.
    if (!handleMetricWithAtomMatchingTrackers(metric.what(), metricIndex, false,
                                              allAtomMatchingTrackers, newAtomMatchingTrackerMap,
                                              trackerToMetricMap, trackerIndex)) {
        return false;
    }

    if (metric.has_condition() &&
        !handleMetricWithConditions(metric.condition(), metricIndex, conditionTrackerMap,
                                    metric.links(), allConditionTrackers, mConditionTrackerIndex,
                                    conditionToMetricMap)) {
        return false;
    }
    return true;
}

void EventMetricProducer::dropDataLocked(const int64_t dropTimeNs) {
    mProto->clear();
    mAggregatedAtoms.clear();
    StatsdStats::getInstance().noteBucketDropped(mMetricId);
}

void EventMetricProducer::onSlicedConditionMayChangeLocked(bool overallCondition,
                                                           const int64_t eventTime) {
}

std::unique_ptr<std::vector<uint8_t>> serializeProtoLocked(ProtoOutputStream& protoOutput) {
    size_t bufferSize = protoOutput.size();

    std::unique_ptr<std::vector<uint8_t>> buffer(new std::vector<uint8_t>(bufferSize));

    size_t pos = 0;
    sp<android::util::ProtoReader> reader = protoOutput.data();
    while (reader->readBuffer() != NULL) {
        size_t toRead = reader->currentToRead();
        std::memcpy(&((*buffer)[pos]), reader->readBuffer(), toRead);
        pos += toRead;
        reader->move(toRead);
    }

    return buffer;
}

void EventMetricProducer::clearPastBucketsLocked(const int64_t dumpTimeNs) {
    mProto->clear();
    mAggregatedAtoms.clear();
}

void EventMetricProducer::onDumpReportLocked(const int64_t dumpTimeNs,
                                             const bool include_current_partial_bucket,
                                             const bool erase_data,
                                             const DumpLatency dumpLatency,
                                             std::set<string> *str_set,
                                             ProtoOutputStream* protoOutput) {
    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_ID, (long long)mMetricId);
    protoOutput->write(FIELD_TYPE_BOOL | FIELD_ID_IS_ACTIVE, isActiveLocked());
    if (!mUseAtomAggregation) {
        if (mProto->size() <= 0) {
            return;
        }

        size_t bufferSize = mProto->size();
        VLOG("metric %lld dump report now... proto size: %zu ", (long long)mMetricId, bufferSize);
        std::unique_ptr<std::vector<uint8_t>> buffer = serializeProtoLocked(*mProto);

        protoOutput->write(FIELD_TYPE_MESSAGE | FIELD_ID_EVENT_METRICS,
                           reinterpret_cast<char*>(buffer.get()->data()), buffer.get()->size());

        if (erase_data) {
            mProto->clear();
        }
    } else {
        uint64_t protoToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_EVENT_METRICS);
        for (const auto& [atomDimensionKey, elapsedTimestampsNs] : mAggregatedAtoms) {
            uint64_t wrapperToken =
                    protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_DATA);

            uint64_t aggregatedToken =
                    protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_AGGREGATED_ATOM);

            uint64_t atomToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_ATOM);
            writeFieldValueTreeToStream(atomDimensionKey.getAtomTag(),
                                        atomDimensionKey.getAtomFieldValues().getValues(),
                                        protoOutput);
            protoOutput->end(atomToken);
            for (int64_t timestampNs : elapsedTimestampsNs) {
                protoOutput->write(
                        FIELD_TYPE_INT64 | FIELD_COUNT_REPEATED | FIELD_ID_ATOM_TIMESTAMPS,
                        (long long)timestampNs);
            }
            protoOutput->end(aggregatedToken);
            protoOutput->end(wrapperToken);
        }
        protoOutput->end(protoToken);
        if (erase_data) {
            mAggregatedAtoms.clear();
        }
    }
}

void EventMetricProducer::onConditionChangedLocked(const bool conditionMet,
                                                   const int64_t eventTime) {
    VLOG("Metric %lld onConditionChanged", (long long)mMetricId);
    mCondition = conditionMet ? ConditionState::kTrue : ConditionState::kFalse;
}

void EventMetricProducer::onMatchedLogEventInternalLocked(
        const size_t matcherIndex, const MetricDimensionKey& eventKey,
        const ConditionKey& conditionKey, bool condition, const LogEvent& event,
        const map<int, HashableDimensionKey>& statePrimaryKeys) {
    if (!condition) {
        return;
    }

    const int64_t elapsedTimeNs = truncateTimestampIfNecessary(event);
    if (!mUseAtomAggregation) {
        uint64_t wrapperToken =
                mProto->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_DATA);
        mProto->write(FIELD_TYPE_INT64 | FIELD_ID_ELAPSED_TIMESTAMP_NANOS,
                      (long long)elapsedTimeNs);

        uint64_t eventToken = mProto->start(FIELD_TYPE_MESSAGE | FIELD_ID_ATOMS);
        event.ToProto(*mProto);
        mProto->end(eventToken);
        mProto->end(wrapperToken);
    } else {
        AtomDimensionKey key(event.GetTagId(), HashableDimensionKey(event.getValues()));

        std::vector<int64_t>& aggregatedTimestampsNs = mAggregatedAtoms[key];
        aggregatedTimestampsNs.push_back(elapsedTimeNs);
    }
}

size_t EventMetricProducer::byteSizeLocked() const {
    if (mUseAtomAggregation) {
        size_t totalSize = 0;
        for (const auto& [atomDimensionKey, elapsedTimestampsNs] : mAggregatedAtoms) {
            totalSize +=
                    sizeof(FieldValue) * atomDimensionKey.getAtomFieldValues().getValues().size();
            totalSize += sizeof(int64_t) * elapsedTimestampsNs.size();
        }
        return totalSize;
    }
    return mProto->bytesWritten();
}

}  // namespace statsd
}  // namespace os
}  // namespace android
