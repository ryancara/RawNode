#include "ui/Filmstrip.h"

#include "imgio/ImageIO.h"
#include "persist/ProjectPersist.h"

#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#include <climits>
#include <filesystem>

namespace fs = std::filesystem;

void freeFilmstripTextures(std::vector<FilmstripEntry> &entries) {
  for (FilmstripEntry &e : entries)
    if (e.tex) glDeleteTextures(1, &e.tex);
  entries.clear();
}

void clearFilmstripThumbJobs(App &app) {
  ++app.filmstripGen;
  std::lock_guard<std::mutex> lock(app.thumbMutex);
  app.thumbQueue.clear();
  app.thumbQueued.clear();
  app.thumbReady.clear();
}

static void releaseFilmstripTex(FilmstripEntry &e) {
  if (e.tex) glDeleteTextures(1, &e.tex);
  e.tex = 0;
  e.tw = e.th = 0;
}

void invalidateFilmstripThumbs(App &app) {
  // Keep existing textures on screen; they reload at the new edge and swap in
  // place when the fresh thumbnail arrives (no blank/flicker in between).
  for (FilmstripEntry &e : app.filmstrip) {
    e.thumbPending = true;
    e.thumbFailed = false;
  }
}

static int countFilmstripTextures(const App &app) {
  int n = 0;
  for (const FilmstripEntry &e : app.filmstrip)
    if (e.tex) ++n;
  return n;
}

static void evictFilmstripLru(App &app, int protectA, int protectB) {
  int victim = -1, oldest = INT_MAX;
  for (int i = 0; i < (int)app.filmstrip.size(); ++i) {
    if (i == protectA || i == protectB) continue;
    const FilmstripEntry &e = app.filmstrip[i];
    if (!e.tex) continue;
    if (e.thumbLru < oldest) {
      oldest = e.thumbLru;
      victim = i;
    }
  }
  if (victim < 0) return;
  FilmstripEntry &e = app.filmstrip[victim];
  releaseFilmstripTex(e);
  e.thumbPending = true;
  e.thumbLoading = false;
}

static int filmstripIndexForPath(const App &app, const std::string &path) {
  for (int i = 0; i < (int)app.filmstrip.size(); ++i) {
    std::error_code ec;
    if (fs::equivalent(app.filmstrip[i].path, path, ec)) return i;
  }
  return -1;
}

static void finishFilmstripThumbJob(App &app, const std::string &path, bool failed) {
  const int i = filmstripIndexForPath(app, path);
  if (i < 0) return;
  FilmstripEntry &e = app.filmstrip[i];
  e.thumbLoading = false;
  if (failed) {
    e.thumbFailed = true;
    e.thumbPending = false;
  }
}

void requestFilmstripThumb(App &app, int index, bool front) {
  if (index < 0 || index >= (int)app.filmstrip.size()) return;
  FilmstripEntry &e = app.filmstrip[index];
  if (e.thumbFailed || e.thumbLoading || !e.thumbPending) return;
  const std::string path = e.path;
  {
    std::lock_guard<std::mutex> lock(app.thumbMutex);
    if (app.thumbQueued.count(path)) return;
    if (front)
      app.thumbQueue.push_front(path);
    else
      app.thumbQueue.push_back(path);
    app.thumbQueued.insert(path);
  }
  e.thumbLoading = true;
  app.thumbCv.notify_one();
}

static void uploadFilmstripThumbData(App &app, int index, const std::vector<unsigned char> &rgba, int w, int h) {
  if (index < 0 || index >= (int)app.filmstrip.size() || w <= 0 || h <= 0) return;
  while (countFilmstripTextures(app) >= kMaxFilmstripTextures)
    evictFilmstripLru(app, index, app.filmstripIndex);
  FilmstripEntry &e = app.filmstrip[index];
  if (!e.tex) glGenTextures(1, &e.tex);
  glBindTexture(GL_TEXTURE_2D, e.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  e.tw = w;
  e.th = h;
  e.thumbPending = false;
  e.thumbLoading = false;
  e.thumbLru = ++app.thumbLruTick;
}

void pumpFilmstripThumbs(App &app) {
  const int wantEdge = app.filmstripThumbEdge.load();
  for (int n = 0; n < kFilmstripUploadsPerFrame; ++n) {
    ThumbReady ready;
    {
      std::lock_guard<std::mutex> lock(app.thumbMutex);
      if (app.thumbReady.empty()) break;
      ready = std::move(app.thumbReady.front());
      app.thumbReady.pop_front();
    }
    if (ready.kind == ThumbReady::Kind::Canceled) {
      finishFilmstripThumbJob(app, ready.path, false);
      continue;
    }
    const int index = filmstripIndexForPath(app, ready.path);
    if (index < 0) continue;
    if (ready.edge != wantEdge) {
      // Rendered at a stale resolution: drop it and re-queue at the current size.
      finishFilmstripThumbJob(app, ready.path, false);
      FilmstripEntry &e = app.filmstrip[index];
      e.thumbFailed = false;
      e.thumbPending = true;
      continue;
    }
    if (ready.kind == ThumbReady::Kind::Fail) {
      finishFilmstripThumbJob(app, ready.path, true);
      continue;
    }
    uploadFilmstripThumbData(app, index, ready.rgba, ready.w, ready.h);
  }
}

static void filmstripThumbWorker(App *app) {
  while (!app->quit) {
    std::string path;
    int gen = 0;
    int edge = 0;
    {
      std::unique_lock<std::mutex> lock(app->thumbMutex);
      app->thumbCv.wait(lock, [&] { return app->quit || !app->thumbQueue.empty(); });
      if (app->quit) break;
      path = app->thumbQueue.front();
      app->thumbQueue.pop_front();
      app->thumbQueued.erase(path);
      gen = app->filmstripGen.load();
      edge = app->filmstripThumbEdge.load();
    }
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    const bool ok = loadThumbnailRGBA(path, edge, rgba, w, h);
    std::lock_guard<std::mutex> lock(app->thumbMutex);
    if (gen != app->filmstripGen.load()) {
      app->thumbReady.push_back({path, {}, 0, 0, edge, ThumbReady::Kind::Canceled});
      continue;
    }
    if (!ok)
      app->thumbReady.push_back({path, {}, 0, 0, edge, ThumbReady::Kind::Fail});
    else
      app->thumbReady.push_back({path, std::move(rgba), w, h, edge, ThumbReady::Kind::Ok});
  }
}

#if defined(__APPLE__)
std::thread startFilmstripThumbThread(App *app) {
  struct Ctx {
    App *target;
  };
  auto *ctx = new Ctx{app};
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
  const int err = pthread_create(
      &tid, &attr,
      +[](void *p) -> void * {
        auto *c = static_cast<Ctx *>(p);
        filmstripThumbWorker(c->target);
        delete c;
        return nullptr;
      },
      ctx);
  pthread_attr_destroy(&attr);
  if (err != 0) {
    delete ctx;
    return std::thread(filmstripThumbWorker, app);
  }
  return std::thread([tid]() { pthread_join(tid, nullptr); });
}
#else
std::thread startFilmstripThumbThread(App *app) { return std::thread(filmstripThumbWorker, app); }
#endif

void refreshFilmstrip(App &app) {
  clearFilmstripThumbJobs(app);
  freeFilmstripTextures(app.filmstrip);
  if (app.folderDir.empty()) {
    app.filmstripIndex = -1;
    app.filmstripSelection.clear();
    app.filmstripAnchor.clear();
    return;
  }
  const auto paths = listFolderImages(app.folderDir);
  std::unordered_set<std::string> survivingSelection;
  app.filmstrip.reserve(paths.size());
  for (const std::string &p : paths) {
    if (app.filmstripSelection.count(p)) survivingSelection.insert(p);
    FilmstripEntry e;
    e.path = p;
    PersistFile file;
    const auto status = readGraphSidecar(inputSidecarPath(p), file, e.markError);
    if (status != SidecarReadStatus::Error) e.mark = photoMark(file);
    std::error_code ec;
    if (app.isPhotoProtected() && fs::equivalent(p, app.path, ec)) {
      // A valid file on disk cannot clear active source protection; only a
      // successful reopen has validated the saved processor settings too.
      e.mark.reset();
      e.markError = app.persistenceWarning;
    }
    app.filmstrip.push_back(std::move(e));
  }
  app.filmstripSelection.swap(survivingSelection);
  if (std::find(paths.begin(), paths.end(), app.filmstripAnchor) == paths.end()) app.filmstripAnchor.clear();
  app.filmstripIndex = -1;
  if (!app.path.empty()) {
    for (int i = 0; i < (int)app.filmstrip.size(); ++i) {
      std::error_code ec;
      if (fs::equivalent(app.filmstrip[i].path, app.path, ec)) {
        app.filmstripIndex = i;
        break;
      }
    }
  }
}

bool filmstripEntryVisible(const App &app, const FilmstripEntry &entry) {
  if (app.filmstripTab == 1 && !isRawImagePath(entry.path)) return false;
  if (app.filmstripTab == 2 && isRawImagePath(entry.path)) return false;
  return !app.filmstripMarkFilter || entry.mark == app.filmstripMarkFilter;
}

void selectFilmstripEntry(App &app, int index, bool toggle, bool range) {
  if (index < 0 || index >= (int)app.filmstrip.size() || !filmstripEntryVisible(app, app.filmstrip[index])) return;
  const auto &path = app.filmstrip[index].path;
  if (range) {
    for (int anchor = 0; anchor < (int)app.filmstrip.size(); ++anchor) {
      if (app.filmstrip[anchor].path != app.filmstripAnchor || !filmstripEntryVisible(app, app.filmstrip[anchor])) continue;
      if (!toggle) app.filmstripSelection.clear();
      for (int i = std::min(anchor, index); i <= std::max(anchor, index); ++i)
        if (filmstripEntryVisible(app, app.filmstrip[i])) app.filmstripSelection.insert(app.filmstrip[i].path);
      return;
    }
  }
  // A hidden/absent anchor starts a new range at the clicked photograph.
  if (!toggle) app.filmstripSelection.clear();
  if (toggle && !range && app.filmstripSelection.count(path)) app.filmstripSelection.erase(path);
  else app.filmstripSelection.insert(path);
  app.filmstripAnchor = path;
}

void selectAllVisibleFilmstripEntries(App &app) {
  app.filmstripSelection.clear();
  app.filmstripAnchor.clear();
  for (const auto &entry : app.filmstrip) {
    if (!filmstripEntryVisible(app, entry)) continue;
    app.filmstripSelection.insert(entry.path);
    if (app.filmstripAnchor.empty()) app.filmstripAnchor = entry.path;
  }
}
