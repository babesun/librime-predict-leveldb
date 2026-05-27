#include "predict_engine.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include <rime/schema.h>
#include <rime/deployer.h>
#include <rime/algo/utilities.h>
#include <rime/algo/dynamics.h>

#include <thread>

namespace rime {

static const ResourceType kPredictDbPredictDbResourceType = {"level_predict_db",
                                                             "", ""};
static const ResourceType kLegacyPredictDbResourceType = {"predict_db", "", ""};

PredictDbManager& PredictDbManager::instance() {
  static PredictDbManager instance;
  return instance;
}

an<PredictDb> PredictDbManager::GetPredictDb(const path& file_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = db_cache_.find(file_path.string());
  if (found != db_cache_.end()) {
    if (auto db = found->second.lock()) {
      DLOG(INFO) << "Using cached PredictDb for: " << file_path;
      return db;
    } else {
      DLOG(INFO) << "Cached PredictDb for " << file_path
                 << " has expired, creating a new one.";
      db_cache_.erase(found);
    }
  }
  DLOG(INFO) << "Creating new PredictDb for: " << file_path;
  an<PredictDb> new_db = std::make_shared<PredictDb>(file_path);
  db_cache_[file_path.string()] = new_db;
  return new_db;
}

void PredictDbManager::ClearCache() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Clear expired entries; any remaining PredictDb shared_ptrs are held
  // by active PredictEngine instances and will be cleaned up naturally.
  for (auto it = db_cache_.begin(); it != db_cache_.end();) {
    if (it->second.expired()) {
      it = db_cache_.erase(it);
    } else {
      ++it;
    }
  }
  DLOG(INFO) << "PredictDbManager cache cleared.";
}

PredictEngine::PredictEngine(an<PredictDb> level_db,
                             an<LegacyPredictDb> legacy_db,
                             bool legacy_mode,
                             int max_iterations,
                             int max_candidates)
    : legacy_mode_(legacy_mode),
      level_db_(level_db),
      legacy_db_(legacy_db),
      max_iterations_(max_iterations),
      max_candidates_(max_candidates) {}

PredictEngine::~PredictEngine() {
  if (open_thread_.joinable()) {
    open_thread_.join();
  }
}

an<PredictDb> PredictEngine::GetLevelDb() const {
  std::lock_guard<std::mutex> lock(level_db_mutex_);
  return level_db_;
}

void PredictEngine::EnqueuePendingUpdate(const string& key,
                                         const string& word,
                                         bool todelete) {
  std::lock_guard<std::mutex> lock(pending_updates_mutex_);
  if (pending_updates_.size() >= kMaxPendingUpdates) {
    pending_updates_.pop_front();
  }
  pending_updates_.push_back(PendingUpdate{key, word, todelete});
}

void PredictEngine::FlushPendingUpdates() {
  auto db = GetLevelDb();
  if (!db) {
    return;
  }
  std::deque<PendingUpdate> pending;
  {
    std::lock_guard<std::mutex> lock(pending_updates_mutex_);
    pending.swap(pending_updates_);
  }
  for (const auto& item : pending) {
    db->UpdatePredict(item.key, item.word, item.todelete);
  }
}

bool PredictEngine::EnsureDb() {
  if (legacy_mode_) {
    return false;
  }
  auto state = db_init_state_.load(std::memory_order_acquire);
  if (state == DbInitState::Ready) {
    return true;  // Already initialized
  }
  if (state == DbInitState::Opening) {
    return false;
  }
  std::lock_guard<std::mutex> lock(db_init_mutex_);
  state = db_init_state_.load(std::memory_order_acquire);
  if (state == DbInitState::Ready) {
    return true;
  }
  if (state == DbInitState::Opening) {
    return false;
  }
  if (db_name_.empty()) {
    LOG(WARNING) << "PredictEngine::EnsureDb: db_name_ is empty, cannot open.";
    db_init_state_.store(DbInitState::Failed, std::memory_order_release);
    return false;
  }
  if (open_thread_.joinable()) {
    open_thread_.join();
  }
  db_init_state_.store(DbInitState::Opening, std::memory_order_release);
  const auto db_name_snapshot = db_name_;
  const auto cleanup_snapshot = cleanup_config_;
  DLOG(INFO) << "PredictEngine::EnsureDb: opening predict db asynchronously: "
             << db_name_snapshot;
  open_thread_ = std::thread([this, db_name_snapshot, cleanup_snapshot]() {
    the<ResourceResolver> resolver(Service::instance().CreateResourceResolver(
        kPredictDbPredictDbResourceType));
    auto file_path = resolver->ResolvePath(db_name_snapshot);
    auto opened_db = PredictDbManager::instance().GetPredictDb(file_path);
    if (!opened_db || !opened_db->valid()) {
      LOG(ERROR) << "PredictEngine::EnsureDb: failed to open: "
                 << db_name_snapshot;
      db_init_state_.store(DbInitState::Failed, std::memory_order_release);
      return;
    }
    opened_db->SetCleanupConfig(cleanup_snapshot);
    {
      std::lock_guard<std::mutex> db_lock(level_db_mutex_);
      level_db_ = opened_db;
    }
    db_init_state_.store(DbInitState::Ready, std::memory_order_release);
    FlushPendingUpdates();
    DLOG(INFO) << "PredictEngine::EnsureDb: database opened successfully.";
  });
  return false;
}

void PredictEngine::UpdatePredict(const string& key,
                                  const string& word,
                                  bool todelete) {
  if (legacy_mode_) {
    return;
  }
  if (EnsureDb()) {
    auto db = GetLevelDb();
    if (db) {
      db->UpdatePredict(key, word, todelete);
      return;
    }
  }
  EnqueuePendingUpdate(key, word, todelete);
}

bool PredictEngine::Predict(Context* ctx, const string& context_query) {
  DLOG(INFO) << "PredictEngine::Predict ctx=" << ctx << ", context_query='"
             << context_query << "'";
  if (legacy_mode_) {
    if (!legacy_db_) {
      LOG(WARNING) << "PredictEngine::Predict legacy_db_ is null";
      return false;
    }
    if (const auto* found = legacy_db_->Lookup(context_query)) {
      query_ = context_query;
      legacy_candidates_ = found;
      DLOG(INFO) << "PredictEngine::Predict found " << legacy_candidates_->size
                 << " legacy candidates for '" << context_query << "'";
      return true;
    }
    DLOG(INFO) << "PredictEngine::Predict no legacy candidates for '"
               << context_query << "'";
    Clear();
    return false;
  }
  if (!EnsureDb()) {
    return false;
  }
  auto db = GetLevelDb();
  if (!db) {
    return false;
  }
  if (db->Lookup(context_query)) {
    query_ = context_query;
    DLOG(INFO) << "PredictEngine::Predict found candidates for '"
               << context_query << "'";
    return true;
  } else {
    DLOG(INFO) << "PredictEngine::Predict no candidates for '" << context_query
               << "'";
    Clear();
    return false;
  }
}

void PredictEngine::Clear() {
  DLOG(INFO) << "PredictEngine::Clear";
  query_.clear();
  if (legacy_mode_) {
    legacy_candidates_ = nullptr;
    return;
  }
  auto db = GetLevelDb();
  if (db) {
    db->Clear();
  }
  legacy_candidates_ = nullptr;
}

void PredictEngine::CreatePredictSegment(Context* ctx) const {
  int end = int(ctx->input().length());
  Segment segment(end, end);
  segment.tags.insert("prediction");
  segment.tags.insert("placeholder");
  ctx->composition().AddSegment(segment);
  ctx->composition().back().tags.erase("raw");
}

an<Translation> PredictEngine::Translate(const Segment& segment) const {
  DLOG(INFO) << "PredictEngine::Translate";
  auto translation = New<FifoTranslation>();
  size_t end = segment.end;
  auto db = GetLevelDb();
  if (!db)
    return translation;
  auto snapshot = db->GetCandidates();
  int i = 0;
  for (auto predict : snapshot) {
    translation->Append(New<SimpleCandidate>("prediction", end, end, predict));
    i++;
    if (max_candidates_ > 0 && i >= max_candidates_)
      break;
  }
  return translation;
}

PredictEngineComponent::PredictEngineComponent() {}

PredictEngineComponent::~PredictEngineComponent() {}

PredictEngine* PredictEngineComponent::Create(const Ticket& ticket) {
  string level_db_name = "predict.userdb";
  string legacy_db_name = "predict.db";
  bool legacy_mode = false;
  int max_candidates = 0;
  int max_iterations = 0;
  if (auto* schema = ticket.schema) {
    auto* config = schema->config();
    if (config->GetBool("predictor/legacy_mode", &legacy_mode)) {
      DLOG(INFO) << "predictor/legacy_mode: "
                 << (legacy_mode ? "true" : "false");
    }
    if (config->GetString("predictor/predictdb", &level_db_name)) {
      DLOG(INFO) << "custom predictor/predictdb" << level_db_name;
    }
    if (config->GetString("predictor/db", &legacy_db_name)) {
      DLOG(INFO) << "custom predictor/db " << legacy_db_name;
    }
    if (!config->GetInt("predictor/max_candidates", &max_candidates)) {
      DLOG(INFO) << "predictor/max_candidates is not set in schema";
    }
    if (!config->GetInt("predictor/max_iterations", &max_iterations)) {
      DLOG(INFO) << "predictor/max_iterations is not set in schema";
    }
  }

  if (legacy_mode) {
    the<ResourceResolver> resolver(Service::instance().CreateResourceResolver(
        kLegacyPredictDbResourceType));
    auto legacy_file_path = resolver->ResolvePath(legacy_db_name);
    an<LegacyPredictDb> legacy_db =
        std::make_shared<LegacyPredictDb>(legacy_file_path);
    if (!legacy_db || !legacy_db->Load()) {
      LOG(ERROR) << "failed to load legacy predict db: " << legacy_db_name;
      return nullptr;
    }
    return new PredictEngine(nullptr, legacy_db, true, max_iterations,
                             max_candidates);
  }

  // Always defer database open to avoid blocking the main Fcitx thread
  // during engine/session initialization. LevelDB Open() performs file I/O
  // and WAL replay which can take seconds on a populated database.
  // The database will be opened lazily via EnsureDb() when prediction is
  // first used (Predict/UpdatePredict is called).
  an<PredictDb> level_db;
  DLOG(INFO) << "Deferring predict database open for: " << level_db_name;

  auto* engine = new PredictEngine(level_db, nullptr, false, max_iterations,
                                   max_candidates);

  // Store the database name for lazy initialization (when prediction
  // is toggled from off to on at runtime after being disabled by default)
  engine->SetDbName(level_db_name);

  // 读取并设置清理配置
  CleanupConfig cleanup_config;
  if (auto* schema = ticket.schema) {
    auto* config = schema->config();
    config->GetBool("predictor/cleanup/enabled", &cleanup_config.enabled);
    config->GetInt("predictor/cleanup/expire_days",
                   &cleanup_config.expire_days);
    config->GetInt("predictor/cleanup/min_usage", &cleanup_config.min_usage);

    DLOG(INFO) << "cleanup config: enabled=" << cleanup_config.enabled
               << ", expire_days=" << cleanup_config.expire_days
               << ", min_usage=" << cleanup_config.min_usage;
  }
  engine->SetCleanupConfig(cleanup_config);

  return engine;
}

an<PredictEngine> PredictEngineComponent::GetInstance(const Ticket& ticket) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  if (Schema* schema = ticket.schema) {
    auto found = predict_engine_by_schema_id.find(schema->schema_id());
    if (found != predict_engine_by_schema_id.end()) {
      if (auto instance = found->second.lock()) {
        return instance;
      }
    }
    an<PredictEngine> new_instance{Create(ticket)};
    if (new_instance) {
      predict_engine_by_schema_id[schema->schema_id()] = new_instance;
      return new_instance;
    }
  }
  return nullptr;
}

void PredictEngineComponent::ClearCache() {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  predict_engine_by_schema_id.clear();
  DLOG(INFO) << "PredictEngineComponent cache cleared.";
}

// ============================================================================
// PredictDb 实现
// ============================================================================

PredictDb::PredictDb(const path& file_path)
    : UserDbWrapper<LevelDb>(file_path, "predict.userdb") {
  // Synchronous open: only used when prediction is enabled by default.
  // When disabled, the database is not opened until prediction is toggled on.
  if (!Open()) {
    LOG(ERROR) << "Failed to open predict db: " << file_path_;
    return;
  }
  string db_type;
  if (!MetaFetch("/db_type", &db_type)) {
    CreateMetadata();
    DLOG(INFO) << "New predict database created (standard userdb format).";
  } else {
    DLOG(INFO) << "Predict database in standard userdb format.";
  }
}

PredictDb::~PredictDb() {
  // Cleanup is skipped in the destructor to avoid blocking the main thread.
  // Use explicit CleanupStaleEntries() call if needed.
}

bool PredictDb::CreateMetadata() {
  if (!UserDbWrapper<LevelDb>::CreateMetadata()) {
    return false;
  }
  return MetaUpdate("/db_type", "userdb");
}

// ============================================================================
// Lookup: 前缀查询（使用 Jump 优化）
// ============================================================================

bool PredictDb::Lookup(const string& query, int max_candidates) {
  DLOG(INFO) << "PredictDb::Lookup query='" << query << "'";

  if (!loaded()) {
    DLOG(INFO) << "Lookup: db not loaded yet";
    return false;
  }

  // 获取当前 tick（用于遗忘曲线计算）
  TickCount current_tick = 0;
  string tick_str;
  if (MetaFetch("/tick", &tick_str)) {
    try {
      current_tick = std::stoul(tick_str);
    } catch (...) {
      current_tick = 0;
    }
  }

  // 新格式：前缀查询
  string prefix = query + "\t";
  DLOG(INFO) << "Lookup: prefix='" << prefix
             << "', current_tick=" << current_tick;

  auto accessor = QueryAll();
  if (!accessor) {
    DLOG(INFO) << "Lookup: accessor is null";
    return false;
  }

  // ✅ 优化：直接 Jump 到前缀位置
  if (!accessor->Jump(prefix)) {
    DLOG(INFO) << "Lookup: Jump failed, no entries for prefix";
    return false;
  }

  std::vector<PredictEntry> entries;
  string key, value;
  int scanned = 0, matched = 0;

  // 从 Jump 位置开始读取，直到前缀不匹配
  while (accessor->GetNextRecord(&key, &value)) {
    scanned++;

    // ✅ 优化：前缀不匹配时立即退出
    if (key.compare(0, prefix.size(), prefix) != 0) {
      DLOG(INFO) << "Lookup: prefix mismatch, stopping scan";
      break;
    }

    // ✅ 优化：限制返回数量（可选，默认不限制以避免遗漏低频词）
    // 注意：设置过小会导致低频词永远无法被看到和选择
    if (max_candidates > 0 &&
        static_cast<int>(entries.size()) >= max_candidates) {
      DLOG(INFO) << "Lookup: reached max_candidates=" << max_candidates;
      break;
    }

    matched++;

    // 从 key 中提取 word
    string word = key.substr(prefix.size());
    if (word.empty()) {
      LOG(WARNING) << "Lookup: empty word in key='" << key << "'";
      continue;
    }

    PredictEntry entry;
    if (entry.Unpack(value)) {
      // 设置 word（从 key 中提取，不是从 value 解析）
      entry.w = word;
      // 应用遗忘曲线衰减
      entry.ApplyDecay(current_tick);
      DLOG(INFO) << "Lookup: matched entry w='" << entry.w
                 << "', dee=" << entry.dee << ", commits=" << entry.commits
                 << " (after decay)";
      entries.push_back(entry);
    }
  }

  DLOG(INFO) << "Lookup: scanned=" << scanned << ", matched=" << matched
             << ", valid entries=" << entries.size();

  // 按词频排序
  std::sort(entries.begin(), entries.end(),
            [](const PredictEntry& a, const PredictEntry& b) {
              return a.commits > b.commits;
            });

  {
    std::lock_guard<std::shared_mutex> lock(candidates_mutex_);
    candidates_.clear();
    for (const auto& e : entries) {
      candidates_.push_back(e.w);
    }
  }

  DLOG(INFO) << "Lookup: returning " << candidates_.size() << " candidates";
  return !candidates_.empty();
}

// ============================================================================
// UpdatePredict: 写入单个预测词（支持遗忘曲线）
// ============================================================================

void PredictDb::UpdatePredict(const string& key,
                              const string& word,
                              bool todelete) {
  // 序列化写入以避免同进程并发问题
  std::lock_guard<std::mutex> write_lock(write_mutex_);

  if (!loaded()) {
    return;
  }

  // 新格式：key="prefix\tpredict_word"
  string new_key = key + "\t" + word;

  // ✅ 优化 1: 删除操作直接执行，无需先读
  if (todelete) {
    Erase(new_key);
    return;
  }

  // 获取当前 tick（用于遗忘曲线计算）
  TickCount current_tick = 0;
  string tick_str;
  if (MetaFetch("/tick", &tick_str)) {
    try {
      current_tick = std::stoul(tick_str);
    } catch (...) {
      current_tick = 0;
    }
  }
  current_tick++;  // 递增 tick

  // ✅ 优化 2: 只在需要时读取现有数据
  PredictEntry entry;
  string existing_value;

  if (Fetch(new_key, &existing_value)) {
    // 现有条目：更新
    entry.Unpack(existing_value);
    entry.Boost(current_tick);
    DLOG(INFO) << "UpdatePredict: boost '" << word << "', dee=" << entry.dee
               << ", commits=" << entry.commits;
  } else {
    // 新词：初始化（避免读取空值）
    entry.w = word;
    entry.Boost(current_tick);  // 首次使用给予基础权重
    DLOG(INFO) << "UpdatePredict: new '" << word << "', dee=" << entry.dee
               << ", commits=" << entry.commits;
  }

  // ✅ 优化 3: 异步写入（提升性能，断电不敏感场景）
  // 注意：LevelDB 默认 WriteOptions 已经是 async，这里只是明确指定
  if (!Update(new_key, entry.Pack())) {
    LOG(ERROR) << "Failed to update predict entry: " << new_key;
  }

  // 更新全局 tick
  MetaUpdate("/tick", std::to_string(current_tick));

  // 记录输入样本用于 EMA 活跃度估算（仅非删除操作）
  if (!todelete) {
    time_t current_time = std::time(nullptr);

    // 有历史记录时才更新 EMA
    if (last_recorded_tick_ > 0 && last_recorded_time_ > 0) {
      TickCount tick_diff = current_tick - last_recorded_tick_;
      double hours =
          static_cast<double>(current_time - last_recorded_time_) / 3600.0;
      activity_estimator_.Update(tick_diff, hours);
    }

    // 更新历史记录
    last_recorded_tick_ = current_tick;
    last_recorded_time_ = current_time;
  }
}

// ============================================================================
// ActivityEstimator 实现
// ============================================================================

ActivityEstimator::ActivityEstimator(double alpha)
    : ema_(500.0), alpha_(alpha) {
  // alpha = 0.2 表示：
  // - 最新观测值权重 20%
  // - 历史 EMA 权重 80%
  // - 约等效于最近 10 次观测的加权平均
}

void ActivityEstimator::Update(TickCount tick_diff, double hours) {
  // 过滤异常数据（NaN/inf, 1 分钟 ~ 24 小时之外）
  if (std::isnan(hours) || std::isinf(hours) || hours < 0.017 || hours > 24.0) {
    DLOG(INFO) << "ActivityEstimator: skipped (hours=" << hours << ")";
    return;
  }

  if (tick_diff <= 0) {
    return;
  }

  // 计算当前观测值（次/天）
  // 示例：tick_diff=100, hours=2 → current = 100 / (2/24) = 1200 次/天
  double current_inputs_per_day =
      static_cast<double>(tick_diff) / (hours / 24.0);

  // 限制合理范围（避免极端值影响）
  current_inputs_per_day = std::clamp(current_inputs_per_day, 50.0, 5000.0);

  // EMA 更新公式：ema = α × current + (1 - α) × ema
  ema_ = alpha_ * current_inputs_per_day + (1.0 - alpha_) * ema_;

  DLOG(INFO) << "ActivityEstimator::Update: "
             << "tick_diff=" << tick_diff << ", hours=" << hours
             << ", current=" << static_cast<int>(current_inputs_per_day)
             << ", ema=" << static_cast<int>(ema_);
}

// ============================================================================
// 清理配置和清理逻辑
// ============================================================================

void PredictDb::SetCleanupConfig(const CleanupConfig& config) {
  cleanup_config_ = config;
  DLOG(INFO) << "CleanupConfig set: enabled=" << config.enabled
             << ", expire_days=" << config.expire_days
             << ", min_usage=" << config.min_usage;
}

int PredictDb::CleanupStaleEntries() {
  if (!cleanup_config_.enabled || !loaded()) {
    return 0;
  }

  // 获取当前 tick
  TickCount current_tick = 0;
  string tick_str;
  if (MetaFetch("/tick", &tick_str)) {
    try {
      current_tick = std::stoul(tick_str);
    } catch (...) {
      current_tick = 0;
    }
  }

  // 获取 EMA 估算的活跃度
  int inputs_per_day = activity_estimator_.GetInputsPerDay();

  // 计算清理阈值
  // expire_tick = expire_days × inputs_per_day
  // 示例：7 天 × 500 次/天 = 3500 tick
  int expire_tick = cleanup_config_.expire_days * inputs_per_day;

  // min_commits = min_usage × 100
  // 示例：5 次 × 100 = 500 commits
  int min_commits = cleanup_config_.min_usage * 100;

  DLOG(INFO) << "CleanupStaleEntries: "
             << "current_tick=" << current_tick
             << ", inputs_per_day=" << inputs_per_day
             << ", expire_tick=" << expire_tick
             << ", min_commits=" << min_commits;

  int cleaned_count = 0;
  int scanned_count = 0;

  auto accessor = QueryAll();
  if (!accessor) {
    LOG(ERROR) << "CleanupStaleEntries: QueryAll failed";
    return 0;
  }

  // 使用 LevelDB 事务批量删除
  BeginTransaction();

  // 批量删除（每 100 条提交一次，避免阻塞）
  constexpr int kBatchSize = 100;
  std::vector<std::string> keys_to_delete;

  string key, value;
  while (accessor->GetNextRecord(&key, &value)) {
    scanned_count++;

    // 跳过元数据
    if (!key.empty() && key[0] == '\x01') {
      continue;
    }

    // 解析条目
    PredictEntry entry;
    if (!entry.Unpack(value)) {
      DLOG(INFO) << "CleanupStaleEntries: failed to unpack key='" << key << "'";
      continue;
    }

    // 计算时间差（tick 差）
    TickCount delta = current_tick - entry.tick;

    // 判断是否过期
    // 条件 1: 超过 expire_tick 未使用
    // 条件 2: 使用次数 < min_usage
    if (delta > expire_tick && entry.commits < min_commits) {
      keys_to_delete.push_back(key);
      cleaned_count++;

      DLOG(INFO) << "CleanupStaleEntries: marking for deletion: "
                 << "key='" << key << "', "
                 << "delta=" << delta << " (≈" << delta / inputs_per_day
                 << "天), "
                 << "commits=" << entry.commits << " (≈" << entry.commits / 100
                 << "次)";
    }

    // 批量提交删除
    if (static_cast<int>(keys_to_delete.size()) >= kBatchSize) {
      for (const auto& k : keys_to_delete) {
        Erase(k);
      }
      keys_to_delete.clear();

      // yield，避免阻塞
      std::this_thread::yield();
    }
  }

  // 删除剩余条目
  for (const auto& k : keys_to_delete) {
    Erase(k);
  }

  CommitTransaction();

  DLOG(INFO) << "CleanupStaleEntries: completed, "
             << "scanned=" << scanned_count << ", cleaned=" << cleaned_count;

  return cleaned_count;
}

}  // namespace rime
