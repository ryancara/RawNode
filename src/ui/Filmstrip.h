#pragma once

#include "AppState.h"

#include <functional>
#include <thread>

// Thumbnail resolutions snap to this ladder as the filmstrip is resized.
constexpr int kFilmstripThumbEdges[] = {16, 24, 32, 48, 256, 1024};

inline int snapFilmstripThumbEdge(float devicePx) {
  for (int edge : kFilmstripThumbEdges)
    if (devicePx <= (float)edge) return edge;
  return kFilmstripThumbEdges[sizeof(kFilmstripThumbEdges) / sizeof(kFilmstripThumbEdges[0]) - 1];
}

constexpr int kMaxFilmstripTextures = 64;
constexpr int kFilmstripUploadsPerFrame = 2;

void freeFilmstripTextures(std::vector<FilmstripEntry> &entries);
void clearFilmstripThumbJobs(App &app);
void invalidateFilmstripThumbs(App &app);
void requestFilmstripThumb(App &app, int index, bool front);
void pumpFilmstripThumbs(App &app);
// Trash refresh retains surviving decoded thumbnails; obsolete jobs are still cancelled.
void refreshFilmstrip(App &app, bool preserveThumbnails = false);
bool filmstripEntryVisible(const App &app, const FilmstripEntry &entry);
// Selection only: callers open plain-click targets before committing selection.
void selectFilmstripEntry(App &app, int index, bool toggle = false, bool range = false);
void selectAllVisibleFilmstripEntries(App &app);
// Loader injection is local to this worker, used by deterministic lifetime tests.
using FilmstripThumbnailLoader = std::function<bool(const std::string &, int, std::vector<unsigned char> &, int &, int &)>;
std::thread startFilmstripThumbThread(App *app, FilmstripThumbnailLoader load = loadThumbnailRGBA);
