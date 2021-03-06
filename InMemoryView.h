/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#pragma once
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "CookieSync.h"
#include "QueryableView.h"
#include "watchman_config.h"
#include "watchman_perf.h"
#include "watchman_query.h"
#include "watchman_string.h"

struct Watcher;

namespace watchman {

/** Keeps track of the state of the filesystem in-memory. */
struct InMemoryView : public QueryableView {
  ClockPosition getMostRecentRootNumberAndTickValue() const override;
  uint32_t getLastAgeOutTickValue() const override;
  time_t getLastAgeOutTimeStamp() const override;
  w_string getCurrentClockString() const override;

  explicit InMemoryView(w_root_t* root, std::shared_ptr<Watcher> watcher);

  void ageOut(w_perf_t& sample, std::chrono::seconds minAge) override;

  bool doAnyOfTheseFilesExist(
      const std::vector<w_string>& fileNames) const override;

  /** Perform a time-based (since) query and emit results to the supplied
   * query context */
  bool timeGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  /** Walks all files with the suffix(es) configured in the query */
  bool suffixGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  /** Walks files that match the supplied set of paths */
  bool pathGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  bool globGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  bool allFilesGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      int64_t* num_walked) const override;

  std::shared_future<void> waitUntilReadyToQuery(
      const std::shared_ptr<w_root_t>& root) override;

  void startThreads(const std::shared_ptr<w_root_t>& root) override;
  void signalThreads() override;
  void clientModeCrawl(const std::shared_ptr<w_root_t>& root);

  const w_string& getName() const override;
  const std::shared_ptr<Watcher>& getWatcher() const;

 private:
  /* Holds the list head for files of a given suffix */
  struct file_list_head {
    watchman_file* head{nullptr};
  };

  struct view {
    /* the most recently changed file */
    struct watchman_file* latest_file{0};

    /* Holds the list heads for all known suffixes */
    std::unordered_map<w_string, std::unique_ptr<file_list_head>> suffixes;

    std::unique_ptr<watchman_dir> root_dir;
    // The most recently observed tick value of an item in the view
    uint32_t mostRecentTick{1};
    /* root number */
    uint32_t rootNumber{0};

    explicit view(const w_string& root_path);

    void insertAtHeadOfFileList(struct watchman_file* file);
  };
  using SyncView = watchman::Synchronized<view>;

  void ageOutFile(
      std::unordered_set<w_string>& dirs_to_erase,
      watchman_file* file);

  // Consume entries from pending and apply them to the InMemoryView
  bool processPending(
      const std::shared_ptr<w_root_t>& root,
      SyncView::LockedPtr& view,
      PendingCollection::LockedPtr& pending,
      bool pullFromRoot = false);
  void processPath(
      const std::shared_ptr<w_root_t>& root,
      SyncView::LockedPtr& view,
      PendingCollection::LockedPtr& coll,
      const w_string& full_path,
      struct timeval now,
      int flags,
      const watchman_dir_ent* pre_stat);

  /** Updates the otime for the file and bubbles it to the front of recency
   * index */
  void markFileChanged(
      SyncView::LockedPtr& view,
      watchman_file* file,
      const struct timeval& now);

  /** Mark a directory as being removed from the view.
   * Marks the contained set of files as deleted.
   * If recursive is true, is recursively invoked on child dirs. */
  void markDirDeleted(
      SyncView::LockedPtr& view,
      struct watchman_dir* dir,
      const struct timeval& now,
      bool recursive);

  watchman_dir*
  resolveDir(SyncView::LockedPtr& view, const w_string& dirname, bool create);
  const watchman_dir* resolveDir(
      SyncView::ConstLockedPtr& view,
      const w_string& dirname) const;

  /** Returns the direct child file named name if it already
   * exists, else creates that entry and returns it */
  watchman_file* getOrCreateChildFile(
      SyncView::LockedPtr& view,
      watchman_dir* dir,
      const w_string& file_name,
      const struct timeval& now);

  /** Recursively walks files under a specified dir */
  bool dirGenerator(
      w_query* query,
      struct w_query_ctx* ctx,
      const watchman_dir* dir,
      uint32_t depth,
      int64_t* num_walked) const;
  bool globGeneratorTree(
      struct w_query_ctx* ctx,
      int64_t* num_walked,
      const struct watchman_glob_tree* node,
      const struct watchman_dir* dir) const;
  bool globGeneratorDoublestar(
      struct w_query_ctx* ctx,
      int64_t* num_walked,
      const struct watchman_dir* dir,
      const struct watchman_glob_tree* node,
      const char* dir_name,
      uint32_t dir_name_len) const;
  void crawler(
      const std::shared_ptr<w_root_t>& root,
      SyncView::LockedPtr& view,
      PendingCollection::LockedPtr& coll,
      const w_string& dir_name,
      struct timeval now,
      bool recursive);
  void notifyThread(const std::shared_ptr<w_root_t>& root);
  void ioThread(const std::shared_ptr<w_root_t>& root);
  void handleShouldRecrawl(const std::shared_ptr<w_root_t>& root);
  void fullCrawl(
      const std::shared_ptr<w_root_t>& root,
      PendingCollection::LockedPtr& pending);
  void statPath(
      const std::shared_ptr<w_root_t>& root,
      SyncView::LockedPtr& view,
      PendingCollection::LockedPtr& coll,
      const w_string& full_path,
      struct timeval now,
      int flags,
      const watchman_dir_ent* pre_stat);

  CookieSync& cookies_;
  Configuration& config_;

  SyncView view_;
  w_string root_path;

  // This allows a client to wait for a recrawl to complete.
  // The primary use of this is so that "watch-project" doesn't
  // send its return PDU to the client until after the initial
  // crawl is complete.  Note that a recrawl can happen at any
  // point, so this is a bit of a weak promise that a query can
  // be immediately executed, but is good enough assuming that
  // the system isn't in a perpetual state of recrawl.
  struct crawl_state {
    std::unique_ptr<std::promise<void>> promise;
    std::shared_future<void> future;
  };
  watchman::Synchronized<crawl_state> crawlState_;

  uint32_t last_age_out_tick{0};
  time_t last_age_out_timestamp{0};

  /* queue of items that we need to stat/process */
  PendingCollection pending_;

  std::atomic<bool> stopThreads_{false};
  std::shared_ptr<Watcher> watcher_;
};
}
