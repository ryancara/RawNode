#include "OfxThreadPoolTests.h"
#include "OfxThreadPoolTestAccess.h"
#include "ofx/OfxHostPriv.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace {
using Access = OfxThreadPoolTestAccess;
using Event = Access::Event;
const auto deadline = std::chrono::seconds(5);

const OfxMultiThreadSuiteV1 &suite() {
  return *static_cast<const OfxMultiThreadSuiteV1 *>(
      gOfxHost.fetchSuite(gOfxHost.host, kOfxMultiThreadSuite, 1));
}

struct Events {
  OfxThreadPool &pool;
  std::mutex mutex;
  std::condition_variable cv;
  std::map<std::uint64_t, std::set<std::thread::id>> completed, parked;
  std::uint64_t callerWaiting = 0;
  bool duplicate = false, stopping = false, stopped = false, joined = false;

  explicit Events(OfxThreadPool &p) : pool(p) {
    Access::observe(pool, this, [](void *arg, Event event) noexcept {
      auto &self = *static_cast<Events *>(arg);
      const auto state = Access::duringObservation(self.pool);
      std::lock_guard<std::mutex> lock(self.mutex);
      const auto thread = std::this_thread::get_id();
      if (event == Event::WorkerCompleted)
        self.duplicate |= !self.completed[state.generation].insert(thread).second;
      if (event == Event::WorkerWaiting && self.completed[state.generation].count(thread))
        self.parked[state.generation].insert(thread);
      if (event == Event::CallerWaiting) self.callerWaiting = state.generation;
      if (event == Event::Stopping) self.stopping = true;
      if (event == Event::Stopped) {
        self.stopped = true;
        self.joined = Access::joined(self.pool);
      }
      self.cv.notify_all();
    });
  }
  ~Events() { Access::observe(pool, nullptr, nullptr); }

  bool waitParked(std::uint64_t generation, unsigned workers, bool caller = true) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, deadline, [&] {
      return duplicate || (parked[generation].size() >= workers &&
                           (!caller || callerWaiting == generation));
    }) && !duplicate;
  }
  bool waitStopping() {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, deadline, [&] { return stopping; });
  }
};

// The caller and each helper must enter one callback before any may finish.
// One helper then stays blocked while all other helpers acknowledge and park.
// This prevents a test from passing because the caller consumed every slice.
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread::id caller;
  unsigned entered = 0, helpers = 0, finished = 0;
  bool release = false, returned = false, failed = false;
};

struct StackJob {
  Gate &gate;
  std::vector<unsigned> hits;
  explicit StackJob(Gate &g, unsigned n) : gate(g), hits(n) {}

  static void slice(unsigned index, unsigned count, void *arg) {
    auto &job = *static_cast<StackJob *>(arg);
    auto &g = job.gate;
    std::unique_lock<std::mutex> lock(g.mutex);
    unsigned reported = ~0u;
    suite().multiThreadIndex(&reported);
    g.failed |= count != job.hits.size() || index >= count || reported != index ||
                !suite().multiThreadIsSpawnedThread();
    if (index < job.hits.size()) ++job.hits[index];
    const bool helper = std::this_thread::get_id() != g.caller;
    const bool slow = helper && ++g.helpers == 1;
    ++g.entered;
    g.cv.notify_all();
    g.cv.wait(lock, [&] { return g.entered == count || g.release; });
    if (slow) g.cv.wait(lock, [&] { return g.release; });
    // Access the caller's stack argument as the very last callback action.
    g.failed |= job.hits[index] != 1;
    ++g.finished;
    g.cv.notify_all();
  }
};

bool blockedJob(OfxThreadPool &pool, unsigned workers, bool throughSuite, bool stopActive) {
  Events events(pool);
  Gate gate;
  const auto generation = Access::snapshot(pool).generation + 1;
  const unsigned slices = workers + 1;
  std::thread caller([&] {
    gate.caller = std::this_thread::get_id();
    StackJob job(gate, slices);
    const bool ok = throughSuite
        ? suite().multiThread(StackJob::slice, slices, &job) == kOfxStatOK
        : pool.tryRun(StackJob::slice, slices, &job);
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.failed |= !ok || gate.finished != slices ||
        !std::all_of(job.hits.begin(), job.hits.end(), [](unsigned n) { return n == 1; });
    gate.returned = true;
    gate.cv.notify_all();
    // On a broken implementation keep the stack argument alive for cleanup.
    // The assertion above records early return before releasing the last slice.
    gate.cv.wait(lock, [&] { return gate.finished == slices; });
  });
  bool ok = events.waitParked(generation, workers - 1);
  // Acquiring the pool mutex after WorkerWaiting also proves that the helper
  // released it into the condition-variable wait, rather than looping a job.
  const auto state = Access::snapshot(pool);
  ok &= state.active && state.remaining == 1 && state.generation == generation;
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    ok &= gate.helpers == workers && !gate.returned && gate.finished == workers;
  }

  std::thread stopper;
  std::atomic<bool> shutdownReturned{false};
  if (stopActive) {
    stopper = std::thread([&] {
      Access::shutdown(pool);
      shutdownReturned = true;
    });
    ok &= events.waitStopping();
    const auto stopping = Access::snapshot(pool);
    ok &= stopping.active && stopping.stopping && !shutdownReturned;
  }
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.release = true;
    gate.cv.notify_all();
  }
  caller.join();
  if (stopper.joinable()) stopper.join();
  ok &= !gate.failed && gate.finished == slices;
  if (stopActive) ok &= events.stopped && events.joined && shutdownReturned;
  return ok;
}

struct Indices {
  std::mutex mutex;
  std::vector<unsigned> hits;
  bool failed = false;
  int spawned;
  explicit Indices(unsigned n, int expectedSpawned = -1) : hits(n), spawned(expectedSpawned) {}
  static void slice(unsigned index, unsigned count, void *arg) {
    auto &self = *static_cast<Indices *>(arg);
    unsigned reported = ~0u;
    suite().multiThreadIndex(&reported);
    std::lock_guard<std::mutex> lock(self.mutex);
    self.failed |= count != self.hits.size() || index >= count || reported != index;
    self.failed |= self.spawned >= 0 && suite().multiThreadIsSpawnedThread() != self.spawned;
    if (index < self.hits.size()) ++self.hits[index];
  }
  bool exact() const {
    return !failed && std::all_of(hits.begin(), hits.end(), [](unsigned n) { return n == 1; });
  }
};

bool sequentialJobs() {
  OfxThreadPool pool(4);
  Events events(pool);
  for (unsigned n : {2u, 17u, 2u, 1u, 31u, 3u}) {
    Indices indices(n);
    const auto generation = Access::snapshot(pool).generation + 1;
    if (!pool.tryRun(Indices::slice, n, &indices) || !indices.exact() ||
        !events.waitParked(generation, 4)) return false;
    const auto state = Access::snapshot(pool);
    if (state.active || state.remaining || state.generation != generation) return false;
  }
  Access::shutdown(pool);              // All four helpers are parked.
  Access::shutdown(pool);              // Repeated shutdown must also be safe.
  if (!events.joined || !events.stopped || events.duplicate) return false;
  Indices unused(2);
  return !pool.tryRun(Indices::slice, 2, &unused);
}

bool nestedAndOverlapping() {
  struct Nested {
    std::atomic<unsigned> calls{0};
    std::atomic<bool> failed{false};
  } nested;
  auto callback = [](unsigned index, unsigned, void *arg) {
    auto &self = *static_cast<Nested *>(arg);
    const int spawned = suite().multiThreadIsSpawnedThread();
    for (unsigned count : {1u, 3u}) {
      Indices inner(count, count == 1 ? 0 : 1);
      if (suite().multiThread(Indices::slice, count, &inner) != kOfxStatOK || !inner.exact())
        self.failed = true;
      unsigned restored = ~0u;
      suite().multiThreadIndex(&restored);
      if (restored != index || suite().multiThreadIsSpawnedThread() != spawned) self.failed = true;
    }
    ++self.calls;
  };
  if (suite().multiThread(callback, 5, &nested) != kOfxStatOK || nested.failed || nested.calls != 5)
    return false;
  unsigned index = ~0u;
  suite().multiThreadIndex(&index);
  if (index != 0 || suite().multiThreadIsSpawnedThread()) return false;

  // Hold every outer callback, then issue a concurrent call. It must complete
  // via the joined fallback without replacing or waiting on the occupied job.
  Gate gate;
  std::thread outer([&] {
    suite().multiThread([](unsigned, unsigned, void *arg) {
      auto &g = *static_cast<Gate *>(arg);
      std::unique_lock<std::mutex> lock(g.mutex);
      ++g.entered;
      g.cv.notify_all();
      g.cv.wait(lock, [&] { return g.release; });
      ++g.finished;
    }, 2, &gate);
  });
  bool entered;
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    entered = gate.cv.wait_for(lock, deadline, [&] { return gate.entered >= 1; });
  }
  Indices overlap(7);
  bool ok = entered && suite().multiThread(Indices::slice, 7, &overlap) == kOfxStatOK && overlap.exact();
  {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.release = true;
    gate.cv.notify_all();
  }
  outer.join();
  return ok && gate.finished == 2;
}
} // namespace

bool testOfxThreadPool() {
  auto check = [](bool ok, const char *name) {
    std::fprintf(stderr, "%s OFX multithread: %s\n", ok ? "ok " : "FAIL", name);
    return ok;
  };
  // Register this destructor before the first suite job. The host owner must
  // already exist, so it outlives destructors registered by plugin loading.
  struct LateSuiteCall {
    ~LateSuiteCall() {
      const auto before = Access::snapshot(OfxThreadPool::host());
      Indices indices(2);
      if (before.stopping || suite().multiThread(Indices::slice, 2, &indices) != kOfxStatOK ||
          !indices.exact() || Access::snapshot(OfxThreadPool::host()).generation != before.generation + 1)
        std::abort();
      std::fprintf(stderr, "ok  OFX multithread: suite call during static teardown\n");
    }
  };
  static LateSuiteCall lateSuiteCall;
  {
    OfxThreadPool neverStarted(2);
    Access::shutdown(neverStarted);
    Access::shutdown(neverStarted);
  }
  for (bool stopping : {false, true}) {
    OfxThreadPool pool(2);
    if (!check(blockedJob(pool, 2, false, stopping),
               stopping ? "active shutdown waits and joins" : "one completion per worker; stack lifetime")) return false;
  } // A used fixture's destructor must join without an explicit shutdown.
  if (!check(sequentialJobs(), "sequential generations, fewer slices, parked/repeated shutdown")) return false;
  for (unsigned requested : {0u, 1u, 2u, 9u, 2u}) {
    Indices indices(std::max(1u, requested), requested <= 1 ? 0 : 1);
    if (!check(suite().multiThread(Indices::slice, requested, &indices) == kOfxStatOK && indices.exact(),
               "suite indices/count and n <= 1")) return false;
  }
  if (!check(suite().multiThread(nullptr, 2, nullptr) == kOfxStatFailed, "null callback")) return false;
  if (!check(nestedAndOverlapping(), "nested identity restoration and overlapping fallback")) return false;
  unsigned cpus = 0;
  if (suite().multiThreadNumCPUs(&cpus) != kOfxStatOK || cpus == 0) return false;
  if (cpus > 1) {
    for (int i = 0; i < 3; ++i)
      if (!check(blockedJob(OfxThreadPool::host(), cpus - 1, true, false),
                 "real suite helpers on repeated calls")) return false;
  }
  // The real host pool remains used and parked until normal process teardown.
  return true;
}
