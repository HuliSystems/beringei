/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "BeringeiClientImpl.h"

#include <folly/String.h>
#include <folly/container/Enumerate.h>
#include <folly/gen/Base.h>
#include <folly/synchronization/LifoSem.h>
#include <thrift/lib/cpp2/async/RequestChannel.h>

#include "beringei/lib/GorillaStatsManager.h"
#include "beringei/lib/TimeSeries.h"
#include "beringei/lib/Timer.h"

using namespace apache::thrift;
using namespace folly::gen;
using namespace facebook;

DEFINE_bool(
    gorilla_compare_reads,
    false,
    "whether to compare the data read from different gorilla services");
DEFINE_double(
    gorilla_compare_epsilon,
    0.1,
    "the allowed error between data for comparison");

namespace facebook {
namespace gorilla {

struct BeringeiFutureContext {
  BeringeiFutureContext()
      : readClients{}, clientNames{}, oneComplete(), getFutures{}, either{} {}
  std::vector<std::shared_ptr<BeringeiNetworkClient>> readClients;
  std::vector<std::string> clientNames;
  // Fulfilled when we've received one full copy of the data.
  folly::Promise<folly::Unit> oneComplete;
  std::vector<folly::Future<folly::Unit>> getFutures;
  std::vector<folly::Future<folly::Unit>> either;
};

namespace {
struct BeringeiFutureGetContext : public BeringeiFutureContext {
  BeringeiFutureGetContext() = delete;
  explicit BeringeiFutureGetContext(const GetDataRequest& request)
      : readRequest(request), resultCollector(nullptr), getRequests{} {}

  GetDataRequest readRequest;
  std::unique_ptr<BeringeiGetResultCollector> resultCollector;
  std::vector<BeringeiNetworkClient::MultiGetRequestMap> getRequests;
};

struct BeringeiFutureScanShardContext : public BeringeiFutureContext {
  explicit BeringeiFutureScanShardContext(const ScanShardRequest& requestArg)
      : request(requestArg) {}

  ScanShardRequest request;
  std::unique_ptr<BeringeiScanShardResultCollector> resultCollector;
};
} // namespace

DEFINE_int32(
    gorilla_client_writer_threads,
    0,
    "number of threads concurrently writing to Beringei for each service");
DEFINE_int32(
    gorilla_queue_capacity,
    1,
    "number of points to buffer in the Beringei write queue for each service");

// Sleep for 100ms between puts if the queue contains <100 elements.
DEFINE_int32(
    gorilla_min_queue_size,
    100,
    "slow down writes if the queue contains fewer elements than this");
DEFINE_int32(
    gorilla_sleep_per_put_us,
    100000,
    "sleep for this long between puts if the queue is near-empty");

DEFINE_int32(
    gorilla_retry_queue_capacity,
    10000,
    "The number of data points that will fit in the retry queue");
DEFINE_int32(
    gorilla_retry_delay_secs,
    55,
    "Retry delay for failed sends. Keeping this under one minute will "
    "still allow data points to arrive in the correct order (assuming "
    "one minute data)");
DEFINE_int32(
    gorilla_write_retry_threads,
    4,
    "Number of threads for retrying failed writes");
DEFINE_int32(
    gorilla_queue_capacity_size_ratio,
    500,
    "Size ratio between the queue capacity and the actual queue size. "
    "Needed because the queue stores vectors");
DEFINE_bool(gorilla_parallel_scan_shard, false, "Fan-out scanShard operations");

const static std::string kEnqueueDroppedKey = "gorilla_client.enqueue_dropped.";
const static std::string kEnqueuedKey = "gorilla_client.enqueued.";
const static std::string kPutDroppedKey = "gorilla_client.put_dropped.";
const static std::string kPutKey = "gorilla_client.put.";
const static std::string kQueueSizeKey = "gorilla_client.queue_size.";
const static std::string kUsPerPut = "gorilla_client.us_per_put.";
const static std::string kPutRetryKey = "gorilla_client.put_retry.";
const static std::string kReadFailover = "gorilla_client.read_failover";
const static std::string kRetryQueueWriteFailures =
    "gorilla_client.retry_queue_write_failures";
const static std::string kRetryQueueSizeKey = "gorilla_client.retry_queue_size";
const static std::string kBadReadServices = "gorilla_client.bad_read_services";
const static std::string kRedirectForMissingData =
    "gorilla_client.redirect_for_missing_data";

const int BeringeiClientImpl::kDefaultReadServicesUpdateInterval = 15;
const int BeringeiClientImpl::kNoWriterThreads = -1;
const int BeringeiClientImpl::kNoReadServicesUpdates = -1;

const static int kRetryThresholdSecs = 30;

// The vectors can be a lot smaller in the retry queue.
const static int kRetryQueueCapacitySizeRatio = 100;
const static int kMinQueueSize = 10;
const static int kMaxRetryBatchSize = 10000;

BeringeiClientImpl::BeringeiClientImpl(
    std::shared_ptr<BeringeiConfigurationAdapterIf> asyncClientAdapter,
    bool throwExceptionOnTransientFailure)
    : maxNumShards_(0),
      configurationAdapter_(asyncClientAdapter),
      throwExceptionOnTransientFailure_(throwExceptionOnTransientFailure),
      retryQueue_(std::max(
          (int)FLAGS_gorilla_retry_queue_capacity /
              kRetryQueueCapacitySizeRatio,
          kMinQueueSize)),
      numRetryQueuedDataPoints_(0) {}

void BeringeiClientImpl::initialize(
    int queueCapacity,
    int writerThreads,
    int readServicesUpdateInterval) {
  setQueueCapacity(queueCapacity);
  setNumWriterThreads(writerThreads);

  // Select a queue size that is big enough to hold all the data point
  // vectors, given the average size of each vector.
  size_t queueSize = std::max(
      queueCapacity / FLAGS_gorilla_queue_capacity_size_ratio, kMinQueueSize);

  // In production clients are either readers or writers. Never
  // both.
  if (writerThreads == 0) {
    // If readServices fails, just assume there are no gorilla services.
    updateReadServices();

    if (readServicesUpdateInterval != kNoReadServicesUpdates) {
      readServicesUpdateScheduler_.addFunction(
          std::bind(&BeringeiClientImpl::updateReadServices, this),
          std::chrono::seconds(readServicesUpdateInterval),
          "readServicesUpdate",
          std::chrono::seconds(readServicesUpdateInterval));
      readServicesUpdateScheduler_.start();
    }
  } else {
    // Writes
    auto writeServices = configurationAdapter_->getWriteServices();
    for (const auto& service : writeServices) {
      writeClients_.emplace_back(new WriteClient(
          createUniqueNetworkClient(service, configurationAdapter_, false),
          queueCapacity,
          queueSize));
    }
    maxNumShards_ = getMaxNumShards(writeClients_);

    // Shadow
    auto shadowServices = configurationAdapter_->getShadowServices();
    for (const auto& service : shadowServices) {
      writeClients_.emplace_back(new WriteClient(
          createUniqueNetworkClient(service, configurationAdapter_, true),
          queueCapacity,
          queueSize));
    }
  }

  startWriterThreads(writerThreads);

  // Initialize counters.
  GorillaStatsManager::addStatExportType(kRetryQueueSizeKey, AVG);
  GorillaStatsManager::addStatValue(kRetryQueueSizeKey, 0);
  GorillaStatsManager::addStatExportType(kReadFailover, SUM);
  GorillaStatsManager::addStatExportType(kRetryQueueWriteFailures, SUM);
  GorillaStatsManager::addStatExportType(kBadReadServices, SUM);
  GorillaStatsManager::addStatExportType(kRedirectForMissingData, SUM);

  for (auto& writeClient : writeClients_) {
    const std::string service = writeClient->client->getServiceName();
    GorillaStatsManager::addStatExportType(kQueueSizeKey + service, AVG);
    GorillaStatsManager::addStatValue(kQueueSizeKey + service, 0);

    GorillaStatsManager::addStatExportType(kEnqueueDroppedKey + service, SUM);
    GorillaStatsManager::addStatExportType(kEnqueuedKey + service, SUM);
    GorillaStatsManager::addStatExportType(kEnqueuedKey + service, AVG);

    GorillaStatsManager::addStatExportType(kPutDroppedKey + service, SUM);
    GorillaStatsManager::addStatExportType(kPutKey + service, SUM);
    GorillaStatsManager::addStatExportType(kPutKey + service, AVG);
    GorillaStatsManager::addStatExportType(kUsPerPut + service, AVG);
    GorillaStatsManager::addStatExportType(kPutRetryKey + service, SUM);
    GorillaStatsManager::addStatExportType(kPutRetryKey + service, COUNT);
  }
}

void BeringeiClientImpl::initializeTestClients(
    int queueCapacity,
    const std::vector<std::shared_ptr<BeringeiNetworkClient>>& readers,
    const std::vector<BeringeiNetworkClient*>& writers) {
  setQueueCapacity(queueCapacity);
  int writerThreads = writers.empty() ? kNoWriterThreads : writers.size();
  setNumWriterThreads(writerThreads);

  // Select a queue size that is big enough to hold all the data point
  // vectors, given the average size of each vector.
  size_t queueSize = std::max(
      queueCapacity / FLAGS_gorilla_queue_capacity_size_ratio, kMinQueueSize);

  for (auto client : readers) {
    readClients_.push_back(client);
  }
  for (auto client : writers) {
    writeClients_.emplace_back(
        new WriteClient(client, queueCapacity, queueSize));
  }
  maxNumShards_ = getMaxNumShards(writeClients_);
  startWriterThreads(writerThreads);
}

BeringeiClientImpl::~BeringeiClientImpl() {
  stopWriterThreads();
  readServicesUpdateScheduler_.shutdown();
}

void BeringeiClientImpl::getLastUpdateTimes(
    uint32_t minLastUpdateTime,
    uint32_t maxKeysPerRequest,
    uint32_t timeoutSeconds,
    std::function<bool(const std::vector<KeyUpdateTime>& keys)> callback) {
  auto readClientCopy = getReadClientCopy();
  if (!readClientCopy) {
    return;
  }

  readClientCopy->getLastUpdateTimes(
      minLastUpdateTime, maxKeysPerRequest, timeoutSeconds, callback);
}

void BeringeiClientImpl::startWriterThreads(int numWriterThreads) {
  if (numWriterThreads > 0) {
    for (auto& writeClient : writeClients_) {
      for (int i = 0; i < numWriterThreads; i++) {
        writers_.emplace_back(
            &BeringeiClientImpl::writeDataPointsForever,
            this,
            writeClient.get());
      }
    }
    for (int i = 0; i < FLAGS_gorilla_write_retry_threads; i++) {
      retryWriters_.emplace_back(&BeringeiClientImpl::retryThread, this);
    }
  }
}

void BeringeiClientImpl::stopWriterThreads() {
  // Terminate all the writer threads.
  if (writeClients_.size()) {
    int writerThreadsPerClient = writers_.size() / writeClients_.size();
    for (auto& writeClient : writeClients_) {
      writeClient->queue.flush(writerThreadsPerClient);
    }
  }

  for (auto& thread : writers_) {
    thread.join();
  }
  writers_.clear();

  for (auto& thread : retryWriters_) {
    RetryOperation op;
    // Empty data points vector will stop the thread.
    retryQueue_.write(std::move(op));
  }

  for (auto& thread : retryWriters_) {
    thread.join();
  }
  retryWriters_.clear();
}

void BeringeiClientImpl::flushQueue() {
  int writerThreadsPerClient = writers_.size() / writeClients_.size();
  stopWriterThreads();
  startWriterThreads(writerThreadsPerClient);
}

std::shared_ptr<BeringeiNetworkClient> BeringeiClientImpl::createNetworkClient(
    const std::string& serviceName,
    std::shared_ptr<BeringeiConfigurationAdapterIf> configurationAdapter,
    bool shadow) {
  return std::make_shared<BeringeiNetworkClient>(
      serviceName, configurationAdapter, shadow);
}

std::unique_ptr<BeringeiNetworkClient>
BeringeiClientImpl::createUniqueNetworkClient(
    const std::string& serviceName,
    std::shared_ptr<BeringeiConfigurationAdapterIf> configurationAdapter,
    bool shadow) {
  return std::make_unique<BeringeiNetworkClient>(
      serviceName, configurationAdapter, shadow);
}

void BeringeiClientImpl::initBeringeiNetworkClients(
    std::vector<std::shared_ptr<BeringeiNetworkClient>>& clients,
    const std::vector<std::string>& readServices) {
  for (auto& readService : readServices) {
    if (!configurationAdapter_->isValidReadService(readService)) {
      GorillaStatsManager::addStatValue(kBadReadServices);
      continue;
    }

    try {
      clients.push_back(
          createNetworkClient(readService, configurationAdapter_, false));
    } catch (std::exception& e) {
      LOG(ERROR) << e.what();
      GorillaStatsManager::addStatValue(kBadReadServices);
    }
  }

  // Just call the nearest service if no valid ones were found.
  if (clients.empty()) {
    std::string nearestReadService =
        configurationAdapter_->getNearestReadService();
    clients.push_back(
        createNetworkClient(nearestReadService, configurationAdapter_, false));
  }
}

bool BeringeiClientImpl::putDataPoints(std::vector<DataPoint>& values) {
  int numPoints = values.size();
  if (numPoints == 0) {
    LOG(ERROR) << "Empty request";
    return true;
  }

  bool allPushedToAnyScope = false;
  for (int i = 0; i < writeClients_.size(); i++) {
    auto& writeClient = writeClients_[i];

    bool success = false;
    if (i < writeClients_.size() - 1) {
      // Pushing does std::move, need to copy because this is not the
      // last iteration of the loop.
      std::vector<DataPoint> valuesCopy = values;
      success = writeClient->queue.push(valuesCopy);
    } else {
      success = writeClient->queue.push(values);
    }

    size_t queueSize = writeClient->queue.size();
    const std::string service = writeClient->client->getServiceName();
    if (success) {
      GorillaStatsManager::addStatValue(kEnqueuedKey + service, numPoints);
    } else {
      GorillaStatsManager::addStatValue(
          kEnqueueDroppedKey + service, numPoints);
    }
    GorillaStatsManager::addStatValue(kQueueSizeKey + service, queueSize);

    if (success) {
      allPushedToAnyScope = true;
    }
  }

  return allPushedToAnyScope;
}

void BeringeiClientImpl::getWithClient(
    BeringeiNetworkClient& readClient,
    const GetDataRequest& request,
    GetDataResult& result,
    std::vector<Key>& foundKeys,
    std::vector<Key>& failedKeys,
    std::vector<Key>* inProgressKeys,
    std::vector<Key>* partialDataKeys) {
  BeringeiNetworkClient::GetRequestMap requests;
  // Break this up into requests per host.
  for (const auto& key : request.keys) {
    readClient.addKeyToGetRequest(key, requests);
  }

  for (auto& iter : requests) {
    iter.second.first.begin = request.begin;
    iter.second.first.end = request.end;
  }

  // Perform the fetch in parallel.
  readClient.performGet(requests);

  for (auto& iter : requests) {
    GetDataRequest& req = iter.second.first;
    GetDataResult& res = iter.second.second;
    // In the case that the server returns back to us fewer keys than we asked
    // for we will not retry that. This should not happen.
    if (req.keys.size() != res.results.size()) {
      LOG(ERROR) << "Mismatch between number of request keys: "
                 << req.keys.size()
                 << " and result size: " << res.results.size();
    }
    for (int i = 0; i < std::min(res.results.size(), req.keys.size()); i++) {
      switch (res.results[i].status) {
        case StatusCode::OK:
          result.results.push_back(res.results[i]);
          foundKeys.push_back(req.keys[i]);
          break;
        case StatusCode::KEY_MISSING:
          // Don't retry on a missing key
          break;
        case StatusCode::RPC_FAIL:
        case StatusCode::ZIPPY_STORAGE_FAIL:
        case StatusCode::DONT_OWN_SHARD:
          failedKeys.push_back(req.keys[i]);
          break;
        case StatusCode::SHARD_IN_PROGRESS:
          if (inProgressKeys) {
            inProgressKeys->push_back(req.keys[i]);
          } else {
            // Caller doesn't want in progress keys. Treat the results
            // as success if there was any data.
            if (res.results[i].data.size() > 0) {
              result.results.push_back(res.results[i]);
              foundKeys.push_back(req.keys[i]);
            }
          }
          break;
        case StatusCode::MISSING_TOO_MUCH_DATA:
          GorillaStatsManager::addStatValue(kRedirectForMissingData, 1);
          if (partialDataKeys) {
            LOG(INFO) << "Received status to redirect to other coast, "
                      << "will retry";

            partialDataKeys->push_back(req.keys[i]);
          } else {
            LOG(INFO) << "Received status to redirect to other coast, "
                      << "disallowed, nonzero data treated as success: "
                      << (res.results[i].data.size() > 0);

            // Caller doesn't care that there are holes in the data. Treat
            // the results as success.
            if (res.results[i].data.size() > 0) {
              result.results.push_back(res.results[i]);
              foundKeys.push_back(req.keys[i]);
            }
          }
          break;
        case StatusCode::BUCKET_NOT_FINALIZED:
          CHECK(false);
          break;
      }
    }
  }
}

void BeringeiClientImpl::get(
    GetDataRequest& request,
    GetDataResult& result,
    const std::string& serviceOverride) {
  auto readClientCopies = getAllReadClients(serviceOverride);
  std::unordered_map<std::string, int64_t> keyShards;
  for (const auto& key : request.keys) {
    keyShards[key.get_key()] = key.get_shardId();
  }

  // Make a copy of the request we'll use for doing a per client request
  // Then clear keys, so we can reorder them as we get successful responses
  GetDataRequest clientRequest = request;
  request.keys.clear();

  for (int i = 0; i < readClientCopies.size(); i++) {
    std::vector<Key> failedKeys;
    std::vector<Key> partialKeys;
    auto& readClient = readClientCopies[i];
    if (i > 0) {
      GorillaStatsManager::addStatValue(kReadFailover);
      LOG(INFO) << "Retrying to other failure service: "
                << readClient->getServiceName();
    }

    // If this is the last iteration, count shards with partial data (in
    // progress or with recorded gaps) as though they were fully successful.
    // However, if `throwExceptionOnTransientFailure_ is enabled, continue
    // to record in progress shards as failures.
    bool lastIteration = i == readClientCopies.size() - 1;
    getWithClient(
        *readClient.get(),
        clientRequest,
        result,
        request.keys,
        failedKeys,
        throwExceptionOnTransientFailure_ || !lastIteration ? &partialKeys
                                                            : nullptr,
        !lastIteration ? &partialKeys : nullptr);

    // Were there any keys hosts said they didn't own the shard for,
    // shards in progress or RPC failures?
    if (failedKeys.empty() && partialKeys.empty()) {
      break;
    }

    // Don't invalidate the cache for the shards that are in
    // progress. They will be read from the other coast.
    if (!failedKeys.empty()) {
      // Do one retry within a service by invalidating the cached shards and
      // asking DirectoryService for possibly updated shard owners
      std::unordered_set<int64_t> invalidShardIds;
      for (const auto& key : failedKeys) {
        invalidShardIds.insert(key.shardId);
      }
      readClient->invalidateCache(invalidShardIds);
      clientRequest.keys = std::move(failedKeys);
      failedKeys.clear();
      getWithClient(
          *readClient.get(),
          clientRequest,
          result,
          request.keys,
          failedKeys,
          throwExceptionOnTransientFailure_ || !lastIteration ? &partialKeys
                                                              : nullptr,
          !lastIteration ? &partialKeys : nullptr);
    }

    // If this fails, then we'll retry to another failure coast
    if (failedKeys.empty() && partialKeys.empty()) {
      break;
    }

    if (lastIteration && throwExceptionOnTransientFailure_) {
      throw std::runtime_error("Failed reading data from gorilla");
    }
    // Now just reset clientRequest keys and retry with a different client
    clientRequest.keys = std::move(failedKeys);
    clientRequest.keys.insert(
        clientRequest.keys.end(),
        std::make_move_iterator(partialKeys.begin()),
        std::make_move_iterator(partialKeys.end()));
    // Restore the original shardId.
    for (auto& key : clientRequest.keys) {
      if (auto id = folly::get_ptr(keyShards, key.get_key())) {
        key.shardId = *id;
      }
    }
  }
}

void BeringeiClientImpl::get(
    GetDataRequest& request,
    GorillaResultVector& result,
    const std::string& serviceOverride) {
  GetDataResult gorillaResult;
  get(request, gorillaResult, serviceOverride);

  result.resize(gorillaResult.results.size());
  for (int i = 0; i < gorillaResult.results.size(); i++) {
    result[i].first = request.keys[i];
    for (auto& block : gorillaResult.results[i].data) {
      TimeSeries::getValues(
          block, result[i].second, request.begin, request.end);
    }
  }
}

void BeringeiClientImpl::futureContextInit(
    BeringeiFutureContext& context,
    bool parallel,
    const std::string& serviceOverride) {
  // For non-parallel operation get all clients then truncate because there's
  // no getReadClientCopy taking the service override and it's not worth
  // micro-optimizing outside the normal path
  context.readClients = getAllReadClients(serviceOverride);
  if (!parallel && !context.readClients.empty()) {
    context.readClients.resize(1);
  }
  context.clientNames = from(context.readClients) | dereference |
      member(&BeringeiNetworkClient::getServiceName) | as<std::vector>();
}

template <typename R, typename F>
void BeringeiClientImpl::futureContextAddFn(
    BeringeiFutureContext& context,
    folly::Executor* workExecutor,
    folly::Future<R>&& future,
    F&& fn) {
  context.getFutures.push_back(
      future.via(workExecutor)
          .then(std::forward<F>(fn))
          .onError([](const std::exception& e) { LOG(ERROR) << e.what(); }));
}

template <typename F>
auto BeringeiClientImpl::futureContextFinalize(
    BeringeiFutureContext& context,
    F&& fn) {
  // Futures madness.
  // Block until either every result has arrived or we received enough results
  // to construct a full data set and then a timeout occurred.
  context.either.push_back(context.oneComplete.getFuture().then([]() {
    return folly::futures::sleep(
        std::chrono::milliseconds(BeringeiNetworkClient::getTimeoutMs()));
  }));
  context.either.push_back(
      collectAll(context.getFutures)
          .then([](const std::vector<folly::Try<folly::Unit>>&) {}));
  return folly::collectAny(context.either).then(std::forward<F>(fn));
}

folly::Future<BeringeiGetResult> BeringeiClientImpl::futureGet(
    GetDataRequest& getDataRequest,
    folly::EventBase* eb,
    folly::Executor* workExecutor,
    const std::string& serviceOverride) {
  auto getContext = std::make_shared<BeringeiFutureGetContext>(getDataRequest);
  futureContextInit(*getContext, true /* parallel */, serviceOverride);

  const auto& request = getContext->readRequest;
  auto& getRequests = getContext->getRequests;
  auto& readClients = getContext->readClients;

  getContext->getRequests =
      std::vector<BeringeiNetworkClient::MultiGetRequestMap>(
          readClients.size());
  getContext->resultCollector = std::make_unique<BeringeiGetResultCollector>(
      request.keys.size(), readClients.size(), request.begin, request.end);

  for (const auto& client : folly::enumerate(readClients)) {
    for (const auto& key : folly::enumerate(request.keys)) {
      (*client)->addKeyToGetRequest(key.index, *key, getRequests[client.index]);
    }

    for (auto& r : getRequests[client.index]) {
      r.second.first.begin = request.begin;
      r.second.first.end = request.end;
      // TODO: BeringeiGetResult::addResults() blocks on a lock, which we
      //       shouldn't do in the CPU thread pool. Though this approach still
      //       reduces latency compared to everything in one thread.
      //       Maybe split the decompression and merging steps?
      futureContextAddFn(
          *getContext,
          workExecutor,
          (*client)->performGet(r.first, std::move(r.second.first), eb),
          [getContext,
           clientId = client.index,
           indices = std::move(r.second.second)](GetDataResult&& result) {
            if (getContext->resultCollector->addResults(
                    result, indices, clientId)) {
              getContext->oneComplete.setValue();
            }
          });
    }
  }

  return futureContextFinalize(
      *getContext,
      [getContext, shouldThrow = throwExceptionOnTransientFailure_](
          std::pair<unsigned long, folly::Try<folly::Unit>>&&) {
        return getContext->resultCollector->finalize(
            shouldThrow, getContext->clientNames);
      });
}

BeringeiGetResult BeringeiClientImpl::get(
    GetDataRequest& request,
    const std::string& serviceOverride) {
  auto eb = BeringeiNetworkClient::getEventBase();
  return futureGet(request, eb, folly::getCPUExecutor().get(), serviceOverride)
      .getVia(eb);
}

void BeringeiClientImpl::writeDataPointsForever(WriteClient* writeClient) {
  bool keepWriting = true;
  while (keepWriting) {
    BeringeiNetworkClient::PutRequestMap requests;
    std::vector<DataPoint> droppedDataPoints;
    try {
      auto points = writeClient->queue.pop([&](DataPoint& dp) {
        // Add each popped data point to the right request.
        bool addMorePoints = true;
        bool dropped = false;

        if (!writeClient->client->addDataPointToRequest(
                dp, requests, dropped)) {
          addMorePoints = false;
        }
        if (dropped) {
          droppedDataPoints.push_back(dp);
        }

        return addMorePoints && droppedDataPoints.size() < kMaxRetryBatchSize;
      });

      if (!points.first) {
        LOG(WARNING) << "Shutting down Beringei writer thread.";
        keepWriting = false;
      }
      if (points.second == 0) {
        continue;
      }

      // Send all the popped data points.
      std::vector<DataPoint> dropped =
          putWithStats(writeClient->client.get(), points.second, requests);
      if (dropped.size() > 0) {
        droppedDataPoints.insert(
            droppedDataPoints.end(),
            std::make_move_iterator(dropped.begin()),
            std::make_move_iterator(dropped.end()));
      }

      if (droppedDataPoints.size() > 0) {
        // Retry and send the failed data points in another thread
        // after a delay to allow the server to come back up if it's
        // down.
        size_t droppedCount = droppedDataPoints.size();
        RetryOperation op;
        op.client = writeClient->client.get();
        op.dataPoints = std::move(droppedDataPoints);
        op.retryTimeSecs = time(nullptr) + FLAGS_gorilla_retry_delay_secs;
        if (numRetryQueuedDataPoints_ + droppedCount >=
                FLAGS_gorilla_retry_queue_capacity ||
            !retryQueue_.write(std::move(op))) {
          logDroppedDataPoints(
              writeClient->client.get(), droppedCount, "retry queue is full");
          GorillaStatsManager::addStatValue(kRetryQueueWriteFailures);
        } else {
          numRetryQueuedDataPoints_ += droppedCount;
          GorillaStatsManager::addStatValue(
              kPutRetryKey + writeClient->client->getServiceName(),
              droppedCount);
          GorillaStatsManager::addStatValue(
              kRetryQueueSizeKey, numRetryQueuedDataPoints_);
        }
      }

      size_t queueSize = writeClient->queue.size();
      GorillaStatsManager::addStatValue(
          kQueueSizeKey + writeClient->client->getServiceName(), queueSize);

      // Wait for a bit if there isn't much in the queue.
      if (queueSize < FLAGS_gorilla_min_queue_size) {
        usleep(FLAGS_gorilla_sleep_per_put_us);
      }
    } catch (std::exception& e) {
      LOG(ERROR) << e.what();
    }
  }
}

std::vector<std::string> BeringeiClientImpl::selectReadServices() {
  return configurationAdapter_->getReadServices();
}

void BeringeiClientImpl::updateReadServices() {
  const auto readServices = selectReadServices();

  if (readServices.size() != 0 && readServices != currentReadServices_) {
    std::vector<std::shared_ptr<BeringeiNetworkClient>> readClients;
    initBeringeiNetworkClients(readClients, readServices);
    auto maxNumShards = getMaxNumShards(readClients);
    currentReadServices_ = readServices;
    maxNumShards_ = maxNumShards;

    folly::RWSpinLock::WriteHolder guard(&readClientLock_);
    readClients_ = readClients;
  }
}

void BeringeiClientImpl::retryThread() {
  while (true) {
    try {
      BeringeiNetworkClient::PutRequestMap requestMap;
      RetryOperation op;
      retryQueue_.blockingRead(op);
      numRetryQueuedDataPoints_ -= op.dataPoints.size();
      GorillaStatsManager::addStatValue(
          kRetryQueueSizeKey, numRetryQueuedDataPoints_);

      if (op.dataPoints.empty()) {
        LOG(INFO) << "Shutting down retry thread";
        break;
      }

      if (op.retryTimeSecs < time(nullptr) - kRetryThresholdSecs) {
        logDroppedDataPoints(
            op.client, op.dataPoints.size(), "data points are too old");
        continue;
      }

      if (op.retryTimeSecs > time(nullptr)) {
        // Sleeping is fine because it's a FIFO queue with a constant delay.
        sleep(op.retryTimeSecs - time(nullptr));
      }

      // Build the request.
      uint32_t totalDropped = 0;
      for (auto& dp : op.dataPoints) {
        bool dropped = false;
        op.client->addDataPointToRequest(dp, requestMap, dropped);
        if (dropped) {
          totalDropped++;
        }
      }

      // Send the data points.
      std::vector<DataPoint> dropped = putWithStats(
          op.client, op.dataPoints.size() - totalDropped, requestMap);
      totalDropped += dropped.size();
      if (totalDropped > 0) {
        logDroppedDataPoints(op.client, totalDropped, "retry send failed");
      }

    } catch (std::exception& e) {
      LOG(ERROR) << e.what();
    }
  }
}

void BeringeiClientImpl::logDroppedDataPoints(
    BeringeiNetworkClient* client,
    uint32_t dropped,
    const std::string& msg) {
  LOG(WARNING) << "Dropping " << dropped << " data points for service "
               << client->getServiceName() << " because " << msg;
  GorillaStatsManager::addStatValue(
      kPutDroppedKey + client->getServiceName(), dropped);
}

std::vector<DataPoint> BeringeiClientImpl::putWithStats(
    BeringeiNetworkClient* client,
    int points,
    BeringeiNetworkClient::PutRequestMap& requestMap) {
  Timer timer(true);
  std::vector<DataPoint> dropped = client->performPut(requestMap);
  GorillaStatsManager::addStatValue(
      kUsPerPut + client->getServiceName(), timer.get());
  GorillaStatsManager::addStatValue(
      kPutKey + client->getServiceName(), points - dropped.size());

  return dropped;
}

void BeringeiClientImpl::stopRequests() {
  auto readClientCopy = getReadClientCopy();
  if (!readClientCopy) {
    return;
  }

  readClientCopy->stopRequests();
}

std::vector<std::shared_ptr<BeringeiNetworkClient>>
BeringeiClientImpl::getAllReadClients(const std::string& serviceOverride) {
  std::vector<std::shared_ptr<BeringeiNetworkClient>> readClientCopies;

  {
    folly::RWSpinLock::ReadHolder guard(&readClientLock_);
    readClientCopies = readClients_;
  }

  if (!serviceOverride.empty()) {
    bool found = false;
    for (auto& client : readClientCopies) {
      if (client->isCorrespondingService(serviceOverride)) {
        found = true;
        readClientCopies = {client};
      }
    }

    if (!found) {
      // Service wasn't on the list. Try making a new temporary
      // client. If the service is invalid, let the exception go to the
      // caller.

      if (!configurationAdapter_->isValidReadService(serviceOverride)) {
        GorillaStatsManager::addStatValue(kBadReadServices);
      } else {
        auto client =
            createNetworkClient(serviceOverride, configurationAdapter_, false);

        // Don't stick this in readClients_ because we don't want normal queries
        // to use the overwritten service.
        readClientCopies = {client};
      }
    }
  }

  return readClientCopies;
}

std::shared_ptr<BeringeiNetworkClient> BeringeiClientImpl::getReadClientCopy() {
  folly::RWSpinLock::ReadHolder guard(&readClientLock_);
  if (readClients_.empty()) {
    LOG(ERROR) << "No read clients enabled for Beringei";
    return std::shared_ptr<BeringeiNetworkClient>();
  }

  return readClients_[0];
}

int64_t BeringeiClientImpl::getMaxNumShards() const {
  return maxNumShards_;
}

int64_t BeringeiClientImpl::getNumShardsFromWriteClient() {
  if (writeClients_.size() == 0) {
    return 0;
  }

  return writeClients_[0]->client->getNumShards();
}

void BeringeiClientImpl::scanShard(
    const ScanShardRequest& request,
    ScanShardResult& result) {
  auto readClientCopy = getReadClientCopy();
  if (!readClientCopy) {
    result.status = StatusCode::RPC_FAIL;
    return;
  }

  readClientCopy->performScanShard(request, result);
}

folly::Future<BeringeiScanShardResult> BeringeiClientImpl::futureScanShard(
    const ScanShardRequest& request,
    folly::EventBase* eb,
    folly::Executor* workExecutor,
    const std::string& serviceOverride) {
  auto context = std::make_shared<BeringeiFutureScanShardContext>(request);
  futureContextInit(
      *context, FLAGS_gorilla_parallel_scan_shard, serviceOverride);

  context->resultCollector = std::make_unique<BeringeiScanShardResultCollector>(
      context->readClients.size(), request);

  for (const auto& client : folly::enumerate(context->readClients)) {
    std::pair<std::string, int> hostInfo;
    if ((*client)->getHostForScanShard(request, hostInfo)) {
      futureContextAddFn(
          *context,
          workExecutor,
          (*client)->performScanShard(hostInfo, request, eb),
          [context, clientId = client.index](ScanShardResult&& result) {
            if (context->resultCollector->addResult(
                    std::move(result), clientId)) {
              context->oneComplete.setValue();
            }
          });
    }
  }

  return futureContextFinalize(
      *context,
      [context, shouldThrow = throwExceptionOnTransientFailure_](
          std::pair<unsigned long, folly::Try<folly::Unit>>&&) {
        return context->resultCollector->finalize(
            shouldThrow, context->clientNames);
      });
}

BeringeiScanShardResult BeringeiClientImpl::scanShard(
    const ScanShardRequest& request,
    const std::string& serviceOverride) {
  auto eb = BeringeiNetworkClient::getEventBase();
  return futureScanShard(
             request, eb, folly::getCPUExecutor().get(), serviceOverride)
      .getVia(eb);
}

void BeringeiClientImpl::setQueueCapacity(int& queueCapacity) {
  queueCapacity = queueCapacity ? queueCapacity : FLAGS_gorilla_queue_capacity;
}

void BeringeiClientImpl::setNumWriterThreads(int& writerThreads) {
  // Figure out the real number of writer threads.
  if (writerThreads != kNoWriterThreads) {
    writerThreads =
        writerThreads ? writerThreads : FLAGS_gorilla_client_writer_threads;
  } else {
    writerThreads = 0;
  }
}

} // namespace gorilla
} // namespace facebook
