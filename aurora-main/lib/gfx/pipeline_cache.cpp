#include "pipeline_cache.hpp"

#include "clear.hpp"
#include "../gx/pipeline.hpp"
#include "../fs_helper.hpp"
#include "../sqlite_utils.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL_iostream.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

namespace aurora::gfx {
static Module Log("aurora::gfx::pipeline_cache");

constexpr int PipelineCacheSchema = 1;
constexpr const char* InitialPipelineCacheName = "initial_pipeline_cache.db";
constexpr const char* SdlVfsName = "aurora_pipeline_cache_sdl_vfs";

struct CachedPipeline {
  wgpu::RenderPipeline pipeline;
  uint32_t firstFrameUsed = UINT32_MAX;
};

struct PendingPipeline {
  PipelineRef hash;
  uint32_t firstFrameUsed = UINT32_MAX;
  NewPipelineCallback create;
};

struct PipelineCacheWrite {
  ShaderType type;
  PipelineRef hash;
  uint32_t configVersion;
  ByteBuffer config;
  uint32_t firstFrameUsed = UINT32_MAX;
};

struct SdlVfsSqliteFile {
  sqlite3_file base;
  SDL_IOStream* io = nullptr;
};

static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = false;
static bool g_pipelineFrameActive = false;
static std::atomic_bool g_skipUnreadyGxPipelines = false;
static size_t g_pipelinesPerFrame = 0;
// Keep first-use compilation bounded. The render command stream remains ordered;
// it waits for a queued pipeline only when the corresponding draw is consumed.
constexpr size_t MaxQueuedPipelineBuilds = 256;
// First-use compilation works best as a short parallel burst. Leave two logical processors for the
// render and game threads, and cap large hosts to limit driver submissions and memory use.
constexpr size_t ReservedLogicalProcessors = 2;
constexpr size_t MaxPipelineWorkers = 22;
// Cached clear and GX pipelines are prewarmed using the full worker pool.
constexpr size_t MaxBackgroundPipelineWorkers = MaxPipelineWorkers;
// For synchronous pipeline fallback (OpenGL)
#ifdef NDEBUG
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif
static std::vector<std::thread> g_pipelineThreads;
static bool g_pipelineThreadEnd = false;
static size_t g_activeBackgroundPipelineWorkers = 0;
static std::condition_variable g_pipelineCv;
static absl::flat_hash_map<PipelineRef, CachedPipeline> g_pipelines;
static std::deque<PendingPipeline> g_priorityPipelines;
static std::deque<PendingPipeline> g_backgroundPipelines;
static absl::flat_hash_set<PipelineRef> g_pendingPipelines;

static sqlite3* g_pipelineCacheDb = nullptr;
static sqlite3_stmt* g_pipelineCacheLoadStmt = nullptr;
static sqlite3_stmt* g_pipelineCacheUpsertStmt = nullptr;
static bool g_pipelineCacheBroken = false;
static std::thread g_pipelineCacheWriterThread;
static std::condition_variable g_pipelineCacheWriterCv;
static std::mutex g_pipelineCacheWriterMutex;
static std::deque<PipelineCacheWrite> g_pipelineCacheWriteQueue;
static absl::flat_hash_set<PipelineRef> g_pipelineCachePendingWrites;
static bool g_pipelineCacheWriterStop = false;
static int g_sdlVfsRegisterResult = SQLITE_ERROR;

static SdlVfsSqliteFile* sdl_vfs_file(sqlite3_file* file) {
  return reinterpret_cast<SdlVfsSqliteFile*>(file);
}

static sqlite3_vfs* default_vfs(sqlite3_vfs* vfs) {
  return static_cast<sqlite3_vfs*>(vfs->pAppData);
}

static int sdl_vfs_close(sqlite3_file* file) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io != nullptr) {
    SDL_CloseIO(vfsFile->io);
    vfsFile->io = nullptr;
  }
  return SQLITE_OK;
}

static int sdl_vfs_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || offset < 0 || amount < 0) {
    return SQLITE_IOERR_READ;
  }
  if (SDL_SeekIO(vfsFile->io, offset, SDL_IO_SEEK_SET) < 0) {
    return SQLITE_IOERR_SEEK;
  }

  auto* dst = static_cast<uint8_t*>(buffer);
  int total = 0;
  while (total < amount) {
    const size_t read = SDL_ReadIO(vfsFile->io, dst + total, static_cast<size_t>(amount - total));
    if (read == 0) {
      if (SDL_GetIOStatus(vfsFile->io) == SDL_IO_STATUS_EOF) {
        std::memset(dst + total, 0, static_cast<size_t>(amount - total));
        return SQLITE_IOERR_SHORT_READ;
      }
      return SQLITE_IOERR_READ;
    }
    if (read > static_cast<size_t>(std::numeric_limits<int>::max() - total)) {
      return SQLITE_IOERR_READ;
    }
    total += static_cast<int>(read);
  }
  return SQLITE_OK;
}

static int sdl_vfs_write(sqlite3_file*, const void*, int, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_truncate(sqlite3_file*, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_sync(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_file_size(sqlite3_file* file, sqlite3_int64* size) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || size == nullptr) {
    return SQLITE_IOERR_FSTAT;
  }

  const auto ioSize = SDL_GetIOSize(vfsFile->io);
  if (ioSize < 0) {
    return SQLITE_IOERR_FSTAT;
  }
  *size = static_cast<sqlite3_int64>(ioSize);
  return SQLITE_OK;
}

static int sdl_vfs_lock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_unlock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_check_reserved_lock(sqlite3_file*, int* reserved) {
  if (reserved != nullptr) {
    *reserved = 0;
  }
  return SQLITE_OK;
}

static int sdl_vfs_file_control(sqlite3_file*, int op, void* arg) {
  switch (op) {
  case SQLITE_FCNTL_LOCKSTATE:
    *static_cast<int*>(arg) = SQLITE_LOCK_NONE;
    return SQLITE_OK;
  case SQLITE_FCNTL_HAS_MOVED:
    *static_cast<int*>(arg) = 0;
    return SQLITE_OK;
  case SQLITE_FCNTL_SIZE_HINT:
    return SQLITE_OK;
  default:
    return SQLITE_NOTFOUND;
  }
}

static int sdl_vfs_sector_size(sqlite3_file*) { return 4096; }

static int sdl_vfs_device_characteristics(sqlite3_file*) {
  return SQLITE_IOCAP_IMMUTABLE | SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
}

static constexpr sqlite3_io_methods SdlVfsIoMethods{
    .iVersion = 1,
    .xClose = sdl_vfs_close,
    .xRead = sdl_vfs_read,
    .xWrite = sdl_vfs_write,
    .xTruncate = sdl_vfs_truncate,
    .xSync = sdl_vfs_sync,
    .xFileSize = sdl_vfs_file_size,
    .xLock = sdl_vfs_lock,
    .xUnlock = sdl_vfs_unlock,
    .xCheckReservedLock = sdl_vfs_check_reserved_lock,
    .xFileControl = sdl_vfs_file_control,
    .xSectorSize = sdl_vfs_sector_size,
    .xDeviceCharacteristics = sdl_vfs_device_characteristics,
};

static int sdl_vfs_open(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* file, int flags, int* outFlags) {
  auto* vfsFile = sdl_vfs_file(file);
  vfsFile->base.pMethods = nullptr;
  vfsFile->io = nullptr;

  if (name == nullptr || (flags & SQLITE_OPEN_READWRITE) != 0 || (flags & SQLITE_OPEN_READONLY) == 0) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->io = SDL_IOFromFile(name, "rb");
  if (vfsFile->io == nullptr) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->base.pMethods = &SdlVfsIoMethods;
  if (outFlags != nullptr) {
    *outFlags = SQLITE_OPEN_READONLY;
  }
  return SQLITE_OK;
}

static int sdl_vfs_delete(sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; }

static int sdl_vfs_access(sqlite3_vfs*, const char* name, int flags, int* result) {
  if (result == nullptr) {
    return SQLITE_IOERR_ACCESS;
  }
  if (name == nullptr || flags == SQLITE_ACCESS_READWRITE) {
    *result = 0;
    return SQLITE_OK;
  }

  auto* io = SDL_IOFromFile(name, "rb");
  *result = io != nullptr ? 1 : 0;
  if (io != nullptr) {
    SDL_CloseIO(io);
  }
  return SQLITE_OK;
}

static int sdl_vfs_full_pathname(sqlite3_vfs*, const char* name, int outSize, char* out) {
  if (name == nullptr || out == nullptr || outSize <= 0) {
    return SQLITE_CANTOPEN;
  }
  sqlite3_snprintf(outSize, out, "%s", name);
  return SQLITE_OK;
}

static void* sdl_vfs_dl_open(sqlite3_vfs*, const char*) { return nullptr; }

static void sdl_vfs_dl_error(sqlite3_vfs*, int bytes, char* message) {
  if (message != nullptr && bytes > 0) {
    sqlite3_snprintf(bytes, message, "%s", "Dynamic loading is unsupported");
  }
}

static void (*sdl_vfs_dl_sym(sqlite3_vfs*, void*, const char*))(void) { return nullptr; }

static void sdl_vfs_dl_close(sqlite3_vfs*, void*) {}

static int sdl_vfs_randomness(sqlite3_vfs* vfs, int bytes, char* out) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xRandomness != nullptr) {
    return base->xRandomness(base, bytes, out);
  }
  if (out != nullptr && bytes > 0) {
    std::memset(out, 0, static_cast<size_t>(bytes));
  }
  return bytes;
}

static int sdl_vfs_sleep(sqlite3_vfs* vfs, int microseconds) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xSleep != nullptr) {
    return base->xSleep(base, microseconds);
  }
  return microseconds;
}

static int sdl_vfs_current_time(sqlite3_vfs* vfs, double* time) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xCurrentTime != nullptr) {
    return base->xCurrentTime(base, time);
  }
  if (time != nullptr) {
    *time = 2440587.5;
  }
  return SQLITE_OK;
}

static int sdl_vfs_get_last_error(sqlite3_vfs*, int, char*) { return SQLITE_OK; }

static bool register_sdl_vfs() {
  static std::once_flag registerOnce;
  std::call_once(registerOnce, [] {
    auto* baseVfs = sqlite3_vfs_find(nullptr);
    static sqlite3_vfs sdlVfs{
        .iVersion = 1,
        .szOsFile = static_cast<int>(sizeof(SdlVfsSqliteFile)),
        .mxPathname = 4096,
        .pNext = nullptr,
        .zName = SdlVfsName,
        .pAppData = baseVfs,
        .xOpen = sdl_vfs_open,
        .xDelete = sdl_vfs_delete,
        .xAccess = sdl_vfs_access,
        .xFullPathname = sdl_vfs_full_pathname,
        .xDlOpen = sdl_vfs_dl_open,
        .xDlError = sdl_vfs_dl_error,
        .xDlSym = sdl_vfs_dl_sym,
        .xDlClose = sdl_vfs_dl_close,
        .xRandomness = sdl_vfs_randomness,
        .xSleep = sdl_vfs_sleep,
        .xCurrentTime = sdl_vfs_current_time,
        .xGetLastError = sdl_vfs_get_last_error,
    };
    g_sdlVfsRegisterResult = sqlite3_vfs_register(&sdlVfs, 0);
  });
  return g_sdlVfsRegisterResult == SQLITE_OK;
}

#if defined(__cpp_lib_atomic_ref)
static std::atomic_ref queuedPipelines{g_stats.queuedPipelines};
static std::atomic_ref createdPipelines{g_stats.createdPipelines};
#else
struct AtomicStatRef {
  uint32_t& ref;
  void operator++() { __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  void operator--() { __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  void operator++(int) { __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  void operator--(int) { __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  void operator=(uint32_t val) { __atomic_store_n(&ref, val, __ATOMIC_RELAXED); }
  uint32_t load() const { return __atomic_load_n(&ref, __ATOMIC_RELAXED); }
};
static AtomicStatRef queuedPipelines{g_stats.queuedPipelines};
static AtomicStatRef createdPipelines{g_stats.createdPipelines};
#endif

template <typename PipelineConfig>
static PipelineCacheWrite make_pipeline_cache_write(ShaderType type, PipelineRef hash, const PipelineConfig& config,
                                                    uint32_t firstFrameUsed) {
  static_assert(std::has_unique_object_representations_v<PipelineConfig>);

  PipelineCacheWrite write{
      .type = type,
      .hash = hash,
      .configVersion = config.version,
      .config = ByteBuffer(sizeof(config)),
      .firstFrameUsed = firstFrameUsed,
  };
  std::memcpy(write.config.data(), &config, sizeof(config));
  return write;
}

static void enqueue_pipeline_cache_write(PipelineCacheWrite write) {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr) {
    return;
  }

  {
    std::lock_guard lock{g_pipelineCacheWriterMutex};
    if (!g_pipelineCachePendingWrites.insert(write.hash).second) {
      return;
    }
    g_pipelineCacheWriteQueue.emplace_back(std::move(write));
  }
  g_pipelineCacheWriterCv.notify_one();
}

template <typename Queue>
static auto find_pending_pipeline(Queue& queue, PipelineRef hash) {
  return std::find_if(queue.begin(), queue.end(), [=](const PendingPipeline& pending) { return pending.hash == hash; });
}

static PendingPipeline* touch_pending_pipeline(PipelineRef hash, bool prioritize) {
  auto priorityIt = find_pending_pipeline(g_priorityPipelines, hash);
  if (priorityIt != g_priorityPipelines.end()) {
    return &*priorityIt;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelines, hash);
  if (backgroundIt == g_backgroundPipelines.end()) {
    return nullptr;
  }

  if (!prioritize) {
    return &*backgroundIt;
  }

  g_priorityPipelines.emplace_back(std::move(*backgroundIt));
  g_backgroundPipelines.erase(backgroundIt);
  return &g_priorityPipelines.back();
}

// A persistent resolve is the last chance to complete its producing draw, so promote its queued
// build ahead of first-use work. One already taken by a worker is compiling and has no entry.
static bool promote_pending_pipeline_for_wait(PipelineRef hash) {
  auto priorityIt = find_pending_pipeline(g_priorityPipelines, hash);
  if (priorityIt != g_priorityPipelines.end()) {
    if (priorityIt != g_priorityPipelines.begin()) {
      PendingPipeline pending = std::move(*priorityIt);
      g_priorityPipelines.erase(priorityIt);
      g_priorityPipelines.emplace_front(std::move(pending));
    }
    return true;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelines, hash);
  if (backgroundIt == g_backgroundPipelines.end()) {
    return false;
  }
  PendingPipeline pending = std::move(*backgroundIt);
  g_backgroundPipelines.erase(backgroundIt);
  g_priorityPipelines.emplace_front(std::move(pending));
  return true;
}

template <typename Queue>
static bool remove_pending_pipeline(Queue& queue, PipelineRef hash) {
  const auto it = find_pending_pipeline(queue, hash);
  if (it == queue.end()) {
    return false;
  }
  queue.erase(it);
  return true;
}

template <typename PipelineConfig>
static PipelineRef find_pipeline_impl(ShaderType type, const PipelineConfig& config, NewPipelineCallback&& cb,
                                      bool persist, std::optional<uint32_t> firstFrameUsedOverride) {
  ZoneScoped;

  const PipelineRef hash = xxh3_hash(config, static_cast<HashType>(type));
  const uint32_t firstFrameUsed = firstFrameUsedOverride.value_or(current_frame());
  const bool cachePreload = firstFrameUsedOverride.has_value();
  bool notifyWorker = false;
  bool syncCreate = false;
  bool removedPending = false;
  bool notifyWaiters = false;
  std::optional<PipelineCacheWrite> cacheWrite;
  {
    std::scoped_lock guard{g_pipelineMutex};
    const bool deferGxPipeline = g_hasPipelineThread && g_pipelineFrameActive && type == ShaderType::GX;
    const bool skipUnreadyGxPipeline = deferGxPipeline && g_skipUnreadyGxPipelines.load(std::memory_order_relaxed);
    auto pipelineIt = g_pipelines.find(hash);
    if (pipelineIt != g_pipelines.end()) {
      if (persist && firstFrameUsed < pipelineIt->second.firstFrameUsed) {
        pipelineIt->second.firstFrameUsed = firstFrameUsed;
        cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
      }
    } else if (g_pendingPipelines.contains(hash)) {
      auto* pending = touch_pending_pipeline(hash, g_pipelineFrameActive);
      if (pending != nullptr && firstFrameUsed < pending->firstFrameUsed) {
        pending->firstFrameUsed = firstFrameUsed;
        if (persist) {
          cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
        }
      }
      if (g_pipelineFrameActive && !deferGxPipeline) {
        syncCreate = true;
      }
    } else {
      if (g_pipelineFrameActive && !deferGxPipeline) {
        syncCreate = true;
      } else {
        if (!cachePreload && deferGxPipeline && g_pendingPipelines.size() >= MaxQueuedPipelineBuilds &&
            !g_backgroundPipelines.empty()) {
          g_pendingPipelines.erase(g_backgroundPipelines.back().hash);
          g_backgroundPipelines.pop_back();
          --queuedPipelines;
        }

        // Keep the pipeline queued past the cap rather than compiling synchronously: the recorded draw
        // references this hash forever, and dropping it baked one-shot EFB copies incomplete.
        if (!cachePreload && g_pendingPipelines.size() >= MaxQueuedPipelineBuilds && !skipUnreadyGxPipeline) {
          syncCreate = true;
        } else {
          auto& targetQueue = deferGxPipeline ? g_priorityPipelines : g_backgroundPipelines;
          targetQueue.emplace_back(PendingPipeline{
              .hash = hash,
              .firstFrameUsed = firstFrameUsed,
              .create = std::move(cb),
          });
          g_pendingPipelines.insert(hash);
          ++queuedPipelines;
          if (persist) {
            cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
          }
          notifyWorker = true;
        }
      }
    }
  }

  if (syncCreate) {
    const auto pipeline = cb();
    {
      std::scoped_lock guard{g_pipelineMutex};
      removedPending = remove_pending_pipeline(g_priorityPipelines, hash);
      removedPending = remove_pending_pipeline(g_backgroundPipelines, hash) || removedPending;
      if (removedPending) {
        g_pendingPipelines.erase(hash);
      }
      auto [it, inserted] = g_pipelines.try_emplace(hash, CachedPipeline{
                                                              .pipeline = pipeline,
                                                              .firstFrameUsed = firstFrameUsed,
                                                          });
      if (!inserted && persist && firstFrameUsed < it->second.firstFrameUsed) {
        it->second.firstFrameUsed = firstFrameUsed;
      }
      if (inserted) {
        ++createdPipelines;
      }
      if (persist) {
        cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
      }
      notifyWaiters = true;
    }
  }

  if (cacheWrite) {
    enqueue_pipeline_cache_write(std::move(*cacheWrite));
  }

  if (notifyWorker) {
    g_pipelineCv.notify_one();
  }
  if (notifyWaiters) {
    g_pipelineCv.notify_all();
  }
  if (removedPending) {
    --queuedPipelines;
  }

  return hash;
}

static void pipeline_cache_abort() {
  g_pipelineCacheBroken = true;
  if (g_pipelineCacheLoadStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheLoadStmt);
    g_pipelineCacheLoadStmt = nullptr;
  }
  if (g_pipelineCacheUpsertStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheUpsertStmt);
    g_pipelineCacheUpsertStmt = nullptr;
  }
  if (g_pipelineCacheDb != nullptr) {
    sqlite3_close(g_pipelineCacheDb);
    g_pipelineCacheDb = nullptr;
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write);

static std::string pipeline_cache_seed_path() {
  if (g_config.resourcesPath == nullptr || g_config.resourcesPath[0] == '\0') {
    return InitialPipelineCacheName;
  }

  std::string path{g_config.resourcesPath};
  if (path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += InitialPipelineCacheName;
  return path;
}

static sqlite3* open_pipeline_cache_seed_db(const std::string& path) {
  if (!register_sdl_vfs()) {
    Log.warn("Failed to register SDL pipeline cache seed VFS");
    return nullptr;
  }

  sqlite3* seedDb = nullptr;
  const auto ret =
      sqlite3_open_v2(path.c_str(), &seedDb, SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, SdlVfsName);
  if (ret != SQLITE_OK) {
    if (seedDb != nullptr) {
      sqlite3_close(seedDb);
    }
    Log.info("No bundled initial pipeline cache found at '{}'", path);
    return nullptr;
  }

  bool schemaMatch = false;
  const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
  const auto schemaRet =
      sqlite::exec(seedDb, schemaQuery.c_str(), [&schemaMatch](int, char**, char**) { schemaMatch = true; });
  if (schemaRet != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache schema from '{}': {}", path, sqlite3_errmsg(seedDb));
    sqlite3_close(seedDb);
    return nullptr;
  }
  if (!schemaMatch) {
    Log.warn("Bundled pipeline cache '{}' does not use schema version {}", path, PipelineCacheSchema);
    sqlite3_close(seedDb);
    return nullptr;
  }

  return seedDb;
}

static void seed_pipeline_cache() {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr || g_pipelineCacheUpsertStmt == nullptr) {
    return;
  }

  const auto seedPath = pipeline_cache_seed_path();
  sqlite3* seedDb = open_pipeline_cache_seed_db(seedPath);
  if (seedDb == nullptr) {
    return;
  }

  sqlite3_stmt* seedStmt = nullptr;
  auto closeSeed = [&] {
    if (seedStmt != nullptr) {
      sqlite3_finalize(seedStmt);
      seedStmt = nullptr;
    }
    sqlite3_close(seedDb);
  };

  auto ret = sqlite3_prepare_v3(seedDb,
                                "SELECT type, hash, config_version, config_size, config, first_frame_used "
                                "FROM pipeline_cache",
                                -1, 0, &seedStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
    closeSeed();
    return;
  }

  bool writeFailed = false;
  bool readFailed = false;
  uint32_t mergedRows = 0;
  uint32_t skippedRows = 0;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache seed transaction");
      closeSeed();
      pipeline_cache_abort();
      return;
    }

    while ((ret = sqlite3_step(seedStmt)) == SQLITE_ROW) {
      const auto typeValue = sqlite3_column_int(seedStmt, 0);
      const auto hashValue = sqlite3_column_int64(seedStmt, 1);
      const auto configVersionValue = sqlite3_column_int(seedStmt, 2);
      const auto configSizeValue = sqlite3_column_int(seedStmt, 3);
      const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(seedStmt, 4));
      const auto configBlobSize = sqlite3_column_bytes(seedStmt, 4);
      const auto firstFrameUsedValue = sqlite3_column_int64(seedStmt, 5);
      constexpr auto MaxShaderTypeValue = std::numeric_limits<std::underlying_type_t<ShaderType>>::max();
      if (typeValue < 0 || typeValue > MaxShaderTypeValue || configVersionValue < 0 || configSizeValue < 0 ||
          configSizeValue != configBlobSize || (configBlobSize > 0 && configBlob == nullptr) ||
          firstFrameUsedValue < 0 || firstFrameUsedValue > std::numeric_limits<uint32_t>::max()) {
        ++skippedRows;
        continue;
      }

      PipelineCacheWrite write{
          .type = static_cast<ShaderType>(typeValue),
          .hash = static_cast<PipelineRef>(hashValue),
          .configVersion = static_cast<uint32_t>(configVersionValue),
          .config = ByteBuffer(static_cast<size_t>(configBlobSize)),
          .firstFrameUsed = static_cast<uint32_t>(firstFrameUsedValue),
      };
      if (configBlobSize > 0) {
        std::memcpy(write.config.data(), configBlob, static_cast<size_t>(configBlobSize));
      }

      if (!write_pipeline_cache_record(write)) {
        writeFailed = true;
        break;
      }
      ++mergedRows;
    }

    if (!writeFailed && ret != SQLITE_DONE) {
      Log.warn("Failed while reading bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
      readFailed = true;
    }

    if (!writeFailed && !readFailed) {
      tx.commit();
    }
  }

  closeSeed();

  if (writeFailed) {
    pipeline_cache_abort();
    return;
  }

  if (!readFailed) {
    Log.info("Seeded pipeline cache from '{}' ({} rows merged, {} rows skipped)", seedPath, mergedRows,
             skippedRows);
  }
}

static bool prepare_pipeline_cache_db() {
  if (g_pipelineCacheBroken) {
    return false;
  }
  if (g_pipelineCacheDb != nullptr) {
    return true;
  }

  const auto path = fs_path_to_string(fs_path_from_string(g_config.pipelineCachePath) / "pipeline_cache.db");
  auto ret = sqlite3_open(path.c_str(), &g_pipelineCacheDb);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open pipeline cache database: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite::exec(g_pipelineCacheDb, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pipeline cache pragmas: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  bool schemaFailed = false;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache schema transaction");
      schemaFailed = true;
    } else {
      ret = sqlite::exec(g_pipelineCacheDb, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
      if (ret != SQLITE_OK) {
        Log.error("Failed to create pipeline cache schema table: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    bool schemaMatch = false;
    if (!schemaFailed) {
      const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaQuery.c_str(),
                         [&schemaMatch](int, char**, char**) { schemaMatch = true; });
      if (ret != SQLITE_OK) {
        Log.error("Failed to read pipeline cache schema version: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed && !schemaMatch) {
      const auto schemaSql = fmt::format(
          R"(DROP TABLE IF EXISTS pipeline_cache;
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
          PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaSql.c_str());
      if (ret != SQLITE_OK) {
        Log.error("Failed to initialize pipeline cache schema: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed) {
      tx.commit();
    }
  }

  if (schemaFailed) {
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(g_pipelineCacheDb,
                           "SELECT hash, config, first_frame_used FROM pipeline_cache "
                           "WHERE type = ? AND config_version = ? "
                           "ORDER BY first_frame_used ASC, rowid ASC",
                           -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheLoadStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache load statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  ret = sqlite3_prepare_v3(
      g_pipelineCacheDb,
      "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(type, hash) DO UPDATE SET "
      "config_version = excluded.config_version, "
      "config_size = excluded.config_size, "
      "config = excluded.config, "
      "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
      -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheUpsertStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare pipeline cache upsert statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return false;
  }

  seed_pipeline_cache();
  if (g_pipelineCacheBroken) {
    return false;
  }

  return true;
}

static void prune_old_pipeline_cache_versions() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  const auto clearDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                       underlying(ShaderType::Clear), clear::ClearPipelineConfigVersion);
  auto ret = sqlite::exec(g_pipelineCacheDb, clearDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune clear pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

  const auto gxDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                    underlying(ShaderType::GX), gx::GXPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, gxDelete.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune GX pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write) {
  const auto fail = [&]() {
    sqlite3_reset(g_pipelineCacheUpsertStmt);
    sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
    return false;
  };

  auto ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 1, underlying(write.type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 2, static_cast<sqlite3_int64>(write.hash));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache hash: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 3, static_cast<int>(write.configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 4, static_cast<int>(write.config.size()));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config size: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_blob64(g_pipelineCacheUpsertStmt, 5, write.config.data(), write.config.size(), SQLITE_TRANSIENT);
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config blob: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 6, static_cast<sqlite3_int64>(write.firstFrameUsed));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache first-frame-used: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  ret = sqlite3_step(g_pipelineCacheUpsertStmt);
  if (ret != SQLITE_DONE) {
    Log.error("Failed to upsert pipeline cache row: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  sqlite3_reset(g_pipelineCacheUpsertStmt);
  sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
  return true;
}

static void pipeline_cache_writer() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline cache writer thread");
#endif

  while (true) {
    std::deque<PipelineCacheWrite> batch;
    {
      std::unique_lock lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterCv.wait(lock,
                                   [] { return g_pipelineCacheWriterStop || !g_pipelineCacheWriteQueue.empty(); });
      if (g_pipelineCacheWriterStop && g_pipelineCacheWriteQueue.empty()) {
        return;
      }
      batch.swap(g_pipelineCacheWriteQueue);
      g_pipelineCachePendingWrites.clear();
    }

    bool writeFailed = false;
    {
      sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
      if (!tx) {
        Log.error("Failed to begin pipeline cache write transaction");
        writeFailed = true;
      } else {
        for (const auto& write : batch) {
          if (!write_pipeline_cache_record(write)) {
            writeFailed = true;
            break;
          }
        }

        if (!writeFailed) {
          tx.commit();
        }
      }
    }

    if (writeFailed) {
      pipeline_cache_abort();
      return;
    }
  }
}

// Boot prewarm telemetry: how long the cached-recipe rebuild takes and how well the
// Dawn blob cache served it. Logged once, when the launch queue first drains.
static std::atomic_bool g_prewarmActive{false};
static std::chrono::steady_clock::time_point g_prewarmStart{};
static uint32_t g_prewarmCount = 0;

static void note_pipeline_queue_drained() {
  if (!g_prewarmActive.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  // Persist the monolithic Vulkan pipeline cache once after boot prewarm only; doing it on
  // every drained burst stalls the device lock mid-race. Later first-use compiles are
  // covered by the shutdown serialize.
  webgpu::serialize_pipeline_caches();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_prewarmStart);
  const auto stats = webgpu::blob_cache_stats();
  Log.info("Pipeline prewarm finished: {} pipelines in {:.1f} s (Dawn blob cache: {}/{} hits, {} stores, {:.1f} MiB "
           "loaded)",
           g_prewarmCount, elapsed.count() / 1000.0, stats.hits, stats.lookups, stats.stores,
           static_cast<double>(stats.hitBytes) / (1024.0 * 1024.0));
}

static void compile_pending_pipeline(PendingPipeline pending) {
  auto result = pending.create();
  {
    std::lock_guard lock{g_pipelineMutex};
    const auto [_, inserted] = g_pipelines.try_emplace(pending.hash, CachedPipeline{
                                                                         .pipeline = std::move(result),
                                                                         .firstFrameUsed = pending.firstFrameUsed,
                                                                     });
    g_pendingPipelines.erase(pending.hash);
    if (inserted) {
      ++createdPipelines;
    }
  }
  g_pipelineCv.notify_all();
  --queuedPipelines;
  if (queuedPipelines.load() == 0) {
    note_pipeline_queue_drained();
  }
}

static void pipeline_worker() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif

  while (true) {
    PendingPipeline pending;
    bool background = false;
    {
      std::unique_lock lock{g_pipelineMutex};
      g_pipelineCv.wait(lock, [] {
        return !g_priorityPipelines.empty() ||
               (!g_backgroundPipelines.empty() &&
                g_activeBackgroundPipelineWorkers < MaxBackgroundPipelineWorkers) ||
               g_pipelineThreadEnd;
      });
      if (g_pipelineThreadEnd) {
        return;
      }
      background = g_priorityPipelines.empty();
      auto& source = background ? g_backgroundPipelines : g_priorityPipelines;
      pending = std::move(source.front());
      source.pop_front();
      if (background) {
        ++g_activeBackgroundPipelineWorkers;
      }
    }
    compile_pending_pipeline(std::move(pending));
    if (background) {
      {
        std::lock_guard lock{g_pipelineMutex};
        --g_activeBackgroundPipelineWorkers;
      }
      g_pipelineCv.notify_all();
    }
  }
}

static void build_synchronous_pipelines_for_frame() {
  while (g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    {
      std::lock_guard lock{g_pipelineMutex};
      if (g_priorityPipelines.empty() && g_backgroundPipelines.empty()) {
        return;
      }
      auto& source = !g_priorityPipelines.empty() ? g_priorityPipelines : g_backgroundPipelines;
      pending = std::move(source.front());
      source.pop_front();
    }
    compile_pending_pipeline(std::move(pending));
    ++g_pipelinesPerFrame;
  }
}

static size_t pipeline_worker_count() {
  const size_t logicalProcessors = std::thread::hardware_concurrency();
  if (logicalProcessors == 0) {
    return 1;
  }
  const size_t availableWorkers =
      logicalProcessors > ReservedLogicalProcessors ? logicalProcessors - ReservedLogicalProcessors : 1;
  return std::clamp(availableWorkers, size_t{1}, MaxPipelineWorkers);
}

template <typename PipelineConfig, typename CreateFn>
static void load_pipeline_cache_entries(ShaderType type, uint32_t configVersion, CreateFn&& create) {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  auto ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 1, underlying(type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }
  ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 2, static_cast<int>(configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return;
  }

  while ((ret = sqlite3_step(g_pipelineCacheLoadStmt)) == SQLITE_ROW) {
    const auto storedHash = static_cast<PipelineRef>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 0));
    const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(g_pipelineCacheLoadStmt, 1));
    const auto configSize = sqlite3_column_bytes(g_pipelineCacheLoadStmt, 1);
    const auto firstFrameUsed = static_cast<uint32_t>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 2));
    if (configSize != static_cast<int>(sizeof(PipelineConfig)) || (configSize != 0 && configBlob == nullptr)) {
      continue;
    }
    if (xxh3_hash_s(configBlob, static_cast<size_t>(configSize), static_cast<HashType>(type)) != storedHash) {
      Log.warn("Skipping cached pipeline configuration with mismatched content hash");
      continue;
    }
    PipelineConfig config;
    std::memcpy(&config, configBlob, sizeof(config));
    if (config.version != configVersion) {
      continue;
    }
    if constexpr (std::is_same_v<PipelineConfig, gx::PipelineConfig>) {
      if (!gx::valid_pipeline_config(config)) {
        Log.warn("Skipping invalid cached GX pipeline configuration");
        continue;
      }
    }

    find_pipeline_impl(type, config, [=] { return create(config); }, false, firstFrameUsed);
  }

  if (ret != SQLITE_DONE) {
    Log.error("Failed to read pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
  }

  sqlite3_reset(g_pipelineCacheLoadStmt);
  sqlite3_clear_bindings(g_pipelineCacheLoadStmt);
}

static void load_pipeline_cache() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }
  prune_old_pipeline_cache_versions();
  if (g_pipelineCacheBroken) {
    return;
  }
  load_pipeline_cache_entries<clear::PipelineConfig>(ShaderType::Clear, clear::ClearPipelineConfigVersion,
                                                     clear::create_pipeline);
  load_pipeline_cache_entries<gx::PipelineConfig>(ShaderType::GX, gx::GXPipelineConfigVersion,
                                                  gx::create_pipeline);
  const auto queued = queuedPipelines.load();
  if (queued > 0) {
    g_prewarmCount = queued;
    g_prewarmStart = std::chrono::steady_clock::now();
    g_prewarmActive.store(true, std::memory_order_release);
  }
}

static void start_pipeline_cache_writer() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  g_pipelineCacheWriterStop = false;
  g_pipelineCacheWriterThread = std::thread(pipeline_cache_writer);
}

static void stop_pipeline_cache_writer() {
  if (g_pipelineCacheWriterThread.joinable()) {
    {
      std::lock_guard lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterStop = true;
    }
    g_pipelineCacheWriterCv.notify_one();
    g_pipelineCacheWriterThread.join();
  } else {
    g_pipelineCacheWriterStop = false;
  }

  g_pipelineCacheWriteQueue.clear();
  g_pipelineCachePendingWrites.clear();
}

template <>
PipelineRef find_pipeline(ShaderType type, const clear::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb), true, std::nullopt);
}

template <>
PipelineRef find_pipeline(ShaderType type, const gx::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb), true, std::nullopt);
}

void initialize_pipeline_cache() {
  g_pipelineCacheBroken = false;
  g_pipelineCacheWriterStop = false;
  g_pipelineFrameActive = false;
  g_pipelineThreadEnd = false;
  g_activeBackgroundPipelineWorkers = 0;

  if (webgpu::g_backendType == wgpu::BackendType::OpenGL || webgpu::g_backendType == wgpu::BackendType::OpenGLES ||
      webgpu::g_backendType == wgpu::BackendType::WebGPU) {
    g_hasPipelineThread = false;
  } else {
    g_hasPipelineThread = true;
    const size_t workerCount = pipeline_worker_count();
    g_pipelineThreads.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
      g_pipelineThreads.emplace_back(pipeline_worker);
    }
    Log.info("Enabled {} priority pipeline compilation workers ({} background prewarm)",
             workerCount, MaxBackgroundPipelineWorkers);
  }

  load_pipeline_cache();
  if (!g_pipelineCacheBroken) {
    start_pipeline_cache_writer();
  }
}

void shutdown_pipeline_cache() {
  if (g_hasPipelineThread) {
    {
      std::lock_guard lock{g_pipelineMutex};
      g_pipelineThreadEnd = true;
    }
    g_pipelineCv.notify_all();
    for (auto& thread : g_pipelineThreads) {
      thread.join();
    }
  }
  g_pipelineThreads.clear();
  g_hasPipelineThread = false;
  g_activeBackgroundPipelineWorkers = 0;

  stop_pipeline_cache_writer();
  pipeline_cache_abort();
  g_pipelineCacheBroken = false;
  g_pipelineFrameActive = false;
  g_pipelinesPerFrame = 0;
  g_pipelines.clear();
  g_priorityPipelines.clear();
  g_backgroundPipelines.clear();
  g_pendingPipelines.clear();

  queuedPipelines = 0;
  createdPipelines = 0;
}

void begin_pipeline_frame() {
  g_pipelineFrameActive = true;
  if (!g_hasPipelineThread) {
    g_pipelinesPerFrame = 0;
  }
}

void end_pipeline_frame() {
  g_pipelineFrameActive = false;
  if (!g_hasPipelineThread) {
    build_synchronous_pipelines_for_frame();
  }
}

void set_skip_unready_pipelines(bool enabled) noexcept {
  g_skipUnreadyGxPipelines.store(enabled, std::memory_order_relaxed);
}

bool skip_unready_pipelines() noexcept { return g_skipUnreadyGxPipelines.load(std::memory_order_relaxed); }

uint32_t queued_pipeline_count() noexcept {
#if defined(__cpp_lib_atomic_ref)
  return queuedPipelines.load(std::memory_order_relaxed);
#else
  return queuedPipelines.load();
#endif
}

bool try_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline) {
  std::scoped_lock lock{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end()) {
    return false;
  }
  pipeline = it->second.pipeline;
  return true;
}

static bool wait_pipeline_impl(PipelineRef ref, wgpu::RenderPipeline& pipeline, bool fatalIfMissing,
                               bool persistentPass) {
  std::unique_lock lock{g_pipelineMutex};
  if (!g_pipelines.contains(ref) && g_pendingPipelines.contains(ref)) {
    ZoneScopedN("wait_pipeline");
    const auto finished = [ref] {
      return g_pipelines.contains(ref) || !g_pendingPipelines.contains(ref) || g_pipelineThreadEnd;
    };
    if (persistentPass) {
      if (promote_pending_pipeline_for_wait(ref)) {
        g_pipelineCv.notify_all();
      }
      // A persistent resolve is the last chance to produce its texture, so timing out here corrupts it
      // permanently. Only a pass marked requireReadyPipelines takes this path.
    }
    g_pipelineCv.wait(lock, finished);
  }
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end()) {
    lock.unlock();
    if (fatalIfMissing) {
      Log.fatal("Pipeline 0x{:x} is unavailable at its ordered draw boundary", static_cast<uint64_t>(ref));
    }
    Log.error("Pipeline 0x{:x} is unavailable for a persistent resolve pass; dropping its draw",
              static_cast<uint64_t>(ref));
    return false;
  }
  pipeline = it->second.pipeline;
  return true;
}

bool wait_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline) {
  return wait_pipeline_impl(ref, pipeline, true, false);
}

bool wait_pipeline_for_persistent_pass(PipelineRef ref, wgpu::RenderPipeline& pipeline) {
  return wait_pipeline_impl(ref, pipeline, false, true);
}

} // namespace aurora::gfx

void aurora_set_skip_unready_pipelines(const bool enabled) { aurora::gfx::set_skip_unready_pipelines(enabled); }

bool aurora_get_skip_unready_pipelines() { return aurora::gfx::skip_unready_pipelines(); }

uint32_t aurora_get_queued_pipeline_count() { return aurora::gfx::queued_pipeline_count(); }
