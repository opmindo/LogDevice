/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <boost/noncopyable.hpp>
#include <folly/small_vector.h>
#include <zookeeper/zookeeper.h>

#include "logdevice/common/UpdateableSharedPtr.h"
#include "logdevice/common/ZookeeperClientBase.h"
#include "logdevice/common/debug.h"

namespace facebook { namespace logdevice {

/**
 * Production Zookeper factory used to create ZookeeperClient instances,
 * which connect to Zookeeper servers.
 */
std::unique_ptr<ZookeeperClientBase>
zkFactoryProd(const configuration::ZookeeperConfig& config);

class ZookeeperClient : public ZookeeperClientBase {
 public:
  /**
   * Attempts to establish a ZK session and create a ZK handle.
   *
   * @param quorum               comma-separated ip:port strings describing
   *                             the Zookeeper quorum to use
   * @param session_timeout      ZK session timeout
   *
   * @throws  ConstructorFailed() if zookeeper_init() failed because the quorum
   *          string was invalid (sets err to INVALID_PARAM) or the
   *          file descriptor limit was reached (sets err to SYSLIMIT).
   */
  ZookeeperClient(std::string quorum,
                  std::chrono::milliseconds session_timeout);

  std::shared_ptr<zhandle_t> getHandle() const {
    return zh_.get();
  }

  /**
   * Get the state of the zookeeper connection.
   */
  int state() override;

  /**
   * Sets the data associated with a node.
   */
  int setData(const char* znode_path,
              const char* znode_value,
              int znode_value_size,
              int version,
              stat_completion_t completion,
              const void* data) override;

  /**
   *  Gets the data associated with a node.
   */
  int getData(const char* znode_path,
              data_completion_t completion,
              const void* data) override;

  /**
   * Performs multiple ZK operations as a single atomic operation
   */
  int multiOp(int count,
              const zoo_op_t* ops,
              zoo_op_result_t* results,
              void_completion_t completion,
              const void* data) override;

  /**
   * Converts a ZK *_STATE constant into a human-readable string.
   */
  static std::string stateString(int state);

  /**
   * Sets Zookeeper process-wide debug level to a value corresponding to
   * the given LogDevice debug level.
   */
  static void setDebugLevel(dbg::Level loglevel);

  ~ZookeeperClient() override;

 private:
  std::chrono::milliseconds session_timeout_; // ZK session timeout

  UpdateableSharedPtr<zhandle_t> zh_;

  std::mutex mutex_; // reconnect() checks and replaces zh_ under this lock

  /**
   * (re)-establish a session
   *
   * @prev   previous session, must be in EXPIRED_SESSION_STATE, or nullptr
   *         if we are in constructor and no prior session exists.
   *
   * @return 0 on success, -1 if zookeeper_init() failed. err is set to
   *         STALE if prev is not nullptr and does not match zh_,
   *         SYSLIMIT if process is out of fds, INTERNAL if zookeeper_init()
   *         reports an unexpected status (debug build asserts)
   */
  int reconnect(zhandle_t* prev) override;

  // ZK session state watcher function, used to track session state
  // transitions
  static void sessionWatcher(zhandle_t* zh,
                             int type,
                             int state,
                             const char* path,
                             void* watcherCtx);

  //////// New API ////////
 public:
  int getData(std::string path, data_callback_t cb) override;
  int setData(std::string path,
              std::string data,
              stat_callback_t cb,
              zk::version_t base_version) override;

  int multiOp(std::vector<zk::Op> ops, multi_op_callback_t cb) override;

 private:
  static void getDataCompletion(int rc,
                                const char* value,
                                int value_len,
                                const struct Stat* stat,
                                const void* context);
  static void setDataCompletion(int rc,
                                const struct Stat* stat,
                                const void* context);
  static void multiOpCompletion(int rc, const void* context);

  struct MultiOpContext {
    static constexpr size_t kInlineOps = 4;

   public:
    static zk::OpResponse toOpResponse(const zoo_op_result_t& op_result);
    static std::vector<zk::OpResponse> toOpResponses(
        const folly::small_vector<zoo_op_result_t, kInlineOps>& op_results);

    explicit MultiOpContext(std::vector<zk::Op> ops, multi_op_callback_t cb)
        : ops_(std::move(ops)),
          cb_(std::move(cb)),
          // Note: space efficiency here could be improved, but for simplicity,
          // we resize everything even if some ops don't need some of these
          // fields.
          c_acl_vectors_(ops_.size()),
          c_acl_vector_data_(ops_.size()),
          c_path_buffers_(ops_.size()),
          c_stats_(ops_.size()),
          c_results_(ops_.size()) {
      c_ops_.resize(ops_.size());
      for (size_t i = 0; i < ops_.size(); ++i) {
        addCOp(ops_.at(i), i);
      }
    }

   private:
    // The context object needs to stay in the same memory location so that the
    // pointers captured by ZK remain valid, should not be moved or copied.
    MultiOpContext(const MultiOpContext&) = delete;
    MultiOpContext& operator=(const MultiOpContext&) = delete;
    MultiOpContext(MultiOpContext&&) = delete;
    MultiOpContext& operator=(MultiOpContext&&) = delete;

    void addCCreateOp(const zk::Op& op, size_t index);
    void addCDeleteOp(const zk::Op& op, size_t index);
    void addCSetOp(const zk::Op& op, size_t index);
    void addCCheckOp(const zk::Op& op, size_t index);
    void addCOp(const zk::Op& op, size_t index);

   public:
    std::vector<zk::Op> ops_;
    multi_op_callback_t cb_; // could be empty

    folly::small_vector<::ACL_vector, kInlineOps> c_acl_vectors_;
    folly::small_vector<folly::small_vector<::ACL, kInlineOps>, kInlineOps>
        c_acl_vector_data_;
    folly::small_vector<std::string, kInlineOps> c_path_buffers_;
    folly::small_vector<struct ::Stat, kInlineOps> c_stats_;
    folly::small_vector<zoo_op_t, kInlineOps> c_ops_;
    folly::small_vector<zoo_op_result_t, kInlineOps> c_results_;
  };

  friend class ZookeeperClientInMemory;
};

}} // namespace facebook::logdevice
