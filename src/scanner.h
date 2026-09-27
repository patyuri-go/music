#ifndef SCANNER_H
#define SCANNER_H

#include <functional>
#include <string>
#include <vector>

namespace scanner {

struct ScanRootProgress {
  int id = 0;
  std::string path;
  int files_processed = 0;
  int files_added = 0;
  int files_updated = 0;
  int files_removed = 0;
  int errors = 0;
  bool scanning = false;
  std::string current_file;
};

struct Progress {
  bool scanning = false;
  int total_roots = 0;
  int current_root_index = 0;
  std::vector<ScanRootProgress> roots;
};

using ProgressCb = std::function<void(const Progress &)>;

struct JobStatus {
  int id = 0;
  std::string kind;
  std::string state; // queued | running | done | error
  int processed = 0;
  int added = 0;
  int updated = 0;
  int removed = 0;
  int errors = 0;
  std::string current;
  std::vector<std::string> current_paths; // per-worker "now scanning" paths
  int threads = 0;                        // number of active scan workers
  std::string error;
};

void startScan(ProgressCb cb = nullptr);
void stopScan();
Progress status();
void scanDir(int root_id, const std::string &dir_path);

// Scan a set of paths (directories and/or files) for one root as a single
// multi-threaded unit of work. Used so that one "import" task becomes one job.
void scanDirBatch(int root_id, const std::vector<std::string> &paths);

// Run a full scan of all roots synchronously (blocking).
void scanAllBlocking();

// Enqueue a background job executed serially by a dedicated worker thread.
// Returns the job id. Jobs touching remote storage must go through here so the
// request thread is never blocked.
int enqueueJob(const std::string &kind, std::function<void()> work);
JobStatus jobStatus(int id);

// Return a snapshot of all known jobs (queued / running / done / error),
// with the running job enriched with the per-worker "now scanning" paths.
std::vector<JobStatus> jobList();

// Report the file currently being handled by the running job (used by sync
// jobs, which are not directory-pool based). Cleared automatically when the
// running job finishes.
void setCurrentFile(const std::string &path);

// Per-worker "currently handling" slots for parallel jobs (e.g. batch sync).
// Call beginActivePaths(n) before spawning n workers, setActiveSlot(slot, path)
// from each worker, and endActivePaths() when done.
void activePathsBegin(int n);
void activePathsEnd();
void activePathsSet(int slot, const std::string &path);

} // namespace scanner

#endif
