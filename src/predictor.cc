#include "predictor.h"

#include "predict_engine.h"
#include <string>
#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/translation.h>
#include <rime/schema.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/key_table.h>

namespace rime {

Predictor::Predictor(const Ticket& ticket, an<PredictEngine> predict_engine)
    : Processor(ticket), predict_engine_(predict_engine) {
  // update prediction on context change.
  auto* context = engine_->context();
  select_connection_ = context->select_notifier().connect(
      [this](Context* ctx) { OnSelect(ctx); });
  context_update_connection_ = context->update_notifier().connect(
      [this](Context* ctx) { OnContextUpdate(ctx); });
  delete_connection_ = context->delete_notifier().connect(
      [this](Context* ctx) { OnDelete(ctx); });
  option_update_connection_ = context->option_update_notifier().connect(
      [this](Context* ctx, const string& option) {
        // Lazy-open the predict DB when the user toggles prediction on.
        // This avoids blocking the main thread during IME lifecycle events
        // (e.g. finishComposingText on keyboard hide).
        if (option == "prediction" && ctx->get_option("prediction") &&
            predict_engine_) {
          predict_engine_->EnsureDb();
        }
      });

  // Open the predict DB immediately if prediction is already enabled
  // (e.g. schema default). The option_update_notifier fires before the
  // Predictor is constructed, so we missed the initial notification.
  if (predict_engine_ && context->get_option("prediction")) {
    predict_engine_->EnsureDb();
  }

  ConnectAbortNotifier(context);
}

void Predictor::OnAbort(Context* ctx) {
  if (!predict_engine_ || !ctx || !ctx->get_option("prediction")) {
    return;
  }
  // 任何 abort 路径（ESC/BackSpace）都要清掉 shift shadow，
  // 否则下一次预测窗弹出时残留的 shadow 会让 OnDelete 误删非 Shift 场景的候选。
  shift_active_ = false;
  shift_shadow_text_.clear();
  predict_engine_->Clear();
  iteration_counter_ = 0;
  has_last_timed_commit_ = false;  // 清除时间戳记录
  if (ctx->IsComposing()) {
    self_updating_ = true;
    ctx->Clear();
    ctx->update_notifier()(ctx);
    self_updating_ = false;
  }
}

Predictor::~Predictor() {
  select_connection_.disconnect();
  context_update_connection_.disconnect();
  delete_connection_.disconnect();
  abort_connection_.disconnect();
  option_update_connection_.disconnect();
}

ProcessResult Predictor::ProcessKeyEvent(const KeyEvent& key_event) {
  if (!engine_ || !predict_engine_)
    return kNoop;
  auto keycode = key_event.keycode();
  // Shift_L/R 释放：恢复 selected_index + 清 shadow
  // 必须在 Shift 按下分支之前判断，否则 release 事件也会被按下分支吞掉。
  if (key_event.release() &&
      (keycode == XK_Shift_L || keycode == XK_Shift_R)) {
    auto* ctx = engine_->context();
    if (!ctx->composition().empty() && shift_active_) {
      auto& back = ctx->composition().back();
      if (back.HasTag("prediction")) {
        back.selected_index = 0;
      }
    }
    shift_active_ = false;
    shift_shadow_text_.clear();
    return kNoop;
  }
  // Shift_L/R 按下：备份 selected_candidate text 并把 selected_index 改到越界
  // 让 commit_text_preview 不含预测词 → Squirrel setMarkedText("") → A 键按下不 commit 预测词。
  // menu 仍非空 → HasMenu() true → 候选窗保留。Shift+Delete 走 OnDelete 时 shadow 兜底。
  if (!key_event.release() &&
      (key_event.modifier() & kShiftMask) != 0 &&
      (keycode == XK_Shift_L || keycode == XK_Shift_R)) {
    auto* ctx = engine_->context();
    if (!ctx->composition().empty()) {
      auto& back = ctx->composition().back();
      // 仅修改"零长度 + placeholder + prediction"特征段（预测段），不影响用户输入段。
      if (back.start == back.end &&
          back.HasTag("placeholder") &&
          back.HasTag("prediction")) {
        if (auto cand = back.GetSelectedCandidate()) {
          shift_shadow_text_ = cand->text();
          shift_active_ = true;
        }
        // SIZE_MAX+1 溢出为 0，Menu::Prepare(0) 是 no-op，不会浪费 CPU 拉所有候选。
        back.selected_index = SIZE_MAX;
      }
    }
    return kNoop;
  }
  // BackSpace 独立处理：剥 prediction 段 + return kNoop 让 key 传到 application。
  // 这样 Squirrel 下一轮 rimeUpdate 看到 composition 空 → setMarkedText("")
  // → Word 退出 marked-text 状态 → application 收到的 BackSpace 才能真正删字。
  if (keycode == XK_BackSpace) {
    auto* ctx = engine_->context();
    last_action_ = kDelete;
    if (!ctx->composition().empty()) {
      auto& back = ctx->composition().back();
      // 与 Shift 拦截分支使用相同的零长度 placeholder+prediction 段匹配模式，
      // 避免漏掉 tag 缺失但特征匹配的边界情况。
      if (back.start == back.end && back.HasTag("placeholder") &&
          back.HasTag("prediction")) {
        ctx->composition().pop_back();
        predict_engine_->Clear();
        iteration_counter_ = 0;
        has_last_timed_commit_ = false;
        ctx->commit_history().clear();
        // 主动触发 update_notifier 让 Squirrel 立即看到 composition 变化，
        // 下一轮 rimeUpdate 走 setMarkedText("") 退出 marked-text 状态，
        // application 收到的 BackSpace 才能真正删字。
        // OnContextUpdate 因 last_action_=kDelete 会直接 return，不会重建预测段。
        self_updating_ = true;
        ctx->update_notifier()(ctx);
        self_updating_ = false;
      }
    }
    return kNoop;  // 关键：让 BackSpace 透传到 application
  }

  // 单独的 Cmd(Super) 键 / Escape：关闭预测窗口并清空 commit_history
  bool is_cmd = (keycode == XK_Super_L || keycode == XK_Super_R) &&
                (key_event.modifier() & kSuperMask) != 0;
  if (is_cmd || keycode == XK_Escape) {
    last_action_ = kDelete;
    auto* ctx = engine_->context();
    predict_engine_->Clear();
    iteration_counter_ = 0;
    has_last_timed_commit_ = false;  // 清除时间戳记录
    ctx->commit_history().clear();   // 清空已上屏历史
    if (!ctx->composition().empty() &&
        ctx->composition().back().HasTag("prediction")) {
      ctx->Clear();
      return kAccepted;
    }
  } else {
    last_action_ = kUnspecified;
  }
  return kNoop;
}

void Predictor::OnSelect(Context* ctx) {
  last_action_ = kSelect;
}

void Predictor::OnDelete(Context* ctx) {
  if (legacy_mode_) {
    return;
  }
  if (!predict_engine_ || !ctx || !ctx->get_option("prediction")) {
    return;
  }
  if (ctx->commit_history().empty()) {
    predict_engine_->Clear();
    iteration_counter_ = 0;
    return;
  }
  auto last_commit = ctx->commit_history().back();
  auto selected_candidate = ctx->GetSelectedCandidate();
  // Shift 期间 prediction 段 selected_index 被改成 SIZE_MAX，
  // Context::GetSelectedCandidate() 返回 null，正常路径拿不到候选。
  // 用 Shift 按下时备份的 shadow 兜底，让 Shift+Delete 删词功能不受影响。
  string target_text;
  if (selected_candidate) {
    target_text = selected_candidate->text();
  } else if (shift_active_ && !shift_shadow_text_.empty()) {
    target_text = shift_shadow_text_;
  } else {
    return;
  }
  predict_engine_->UpdatePredict(last_commit.text, target_text, true);
  // 用完即清，避免后续误用。
  if (shift_active_) {
    shift_active_ = false;
    shift_shadow_text_.clear();
  }
  ctx->Clear();
  ctx->update_notifier()(ctx);
}

void Predictor::OnContextUpdate(Context* ctx) {
  if (self_updating_ || !predict_engine_ || !ctx ||
      !ctx->composition().empty() || !ctx->get_option("prediction") ||
      last_action_ == kDelete) {
    return;
  }
  // 误上屏预测词清理：Shift+a 等场景下，前端把当前预测候选（type=prediction）
  // 当作提交直接上屏，而非用户主动选词（用户选词时 last_action_==kSelect）。
  // 此时不再重建预测窗，并清空预测状态，使预测词随本次提交上屏后即消失。
  // 该判断不影响 Shift+Delete（last_action_==kDelete 已提前返回）。
  if (last_action_ != kSelect && !ctx->commit_history().empty() &&
      ctx->commit_history().back().type == "prediction") {
    predict_engine_->Clear();
    iteration_counter_ = 0;
    has_last_timed_commit_ = false;  // 清除时间戳记录
    ctx->commit_history().clear();   // 清除误提交的预测记录
    return;
  }
  if (ctx->commit_history().empty()) {
    PredictAndUpdate(ctx, "$");
    return;
  }
  auto last_commit = ctx->commit_history().back();
  if (last_commit.type == "punct" || last_commit.type == "raw" ||
      last_commit.type == "thru") {
    predict_engine_->Clear();
    iteration_counter_ = 0;
    // 清除时间戳记录
    has_last_timed_commit_ = false;
    return;
  }

  if (legacy_mode_) {
    if (last_commit.type == "prediction") {
      int max_iterations = predict_engine_->max_iterations();
      iteration_counter_++;
      if (max_iterations > 0 && iteration_counter_ >= max_iterations) {
        predict_engine_->Clear();
        iteration_counter_ = 0;
        auto* active_ctx = engine_->context();
        if (active_ctx && !active_ctx->composition().empty() &&
            active_ctx->composition().back().HasTag("prediction")) {
          active_ctx->Clear();
        }
        return;
      }
    }
    PredictAndUpdate(ctx, last_commit.text);
    return;
  }

  // 获取当前时间戳
  auto current_time = std::chrono::steady_clock::now();

  // 检查时间间隔：如果有上一次提交记录，判断时间间隔
  bool should_update_relation = false;
  if (has_last_timed_commit_) {
    // max_commit_interval_seconds_ <= 0 表示不限制时间间隔
    if (max_commit_interval_seconds_ <= 0) {
      should_update_relation = true;
    } else {
      auto duration = std::chrono::duration_cast<std::chrono::seconds>(
          current_time - last_timed_commit_.timestamp);
      if (duration.count() <= max_commit_interval_seconds_) {
        // 时间间隔在阈值内，认为两次提交相关
        should_update_relation = true;
      } else {
        // 时间间隔过长，不建立关联，但更新最后一次提交记录
        LOG(INFO) << "[Predict] Commit interval too long (" << duration.count()
                  << "s), skipping relation update";
      }
    }
  } else {
    // 第一次提交，无法建立关联，但记录当前提交
    LOG(INFO) << "[Predict] First commit, recording for future relation";
  }

  // 只有时间间隔合理时才更新提交之间的关系
  if (should_update_relation) {
    // 防御：跳过"前一个词 == 当前词"的自预测关系。
    // 典型场景：用户删除刚上屏的字又重打一遍同样的词，会写出
    // "词\t词" 这种无意义的自预测记录，导致下次预测把刚输入的字
    // 词又弹回来。这里从源头拦截，避免污染用户词典。
    if (last_timed_commit_.text != last_commit.text) {
      predict_engine_->UpdatePredict(last_timed_commit_.text, last_commit.text,
                                     false);
    }
  }

  // 更新最后一次提交记录（无论是否建立关联）
  last_timed_commit_ = {last_commit.text, last_commit.type, current_time};
  has_last_timed_commit_ = true;

  if (last_commit.type == "prediction") {
    int max_iterations = predict_engine_->max_iterations();
    iteration_counter_++;
    if (max_iterations > 0 && iteration_counter_ >= max_iterations) {
      predict_engine_->Clear();
      iteration_counter_ = 0;
      auto* ctx = engine_->context();
      if (ctx && !ctx->composition().empty() &&
          ctx->composition().back().HasTag("prediction")) {
        ctx->Clear();
      }
      return;
    }
  }
  PredictAndUpdate(ctx, last_commit.text);
}

void Predictor::PredictAndUpdate(Context* ctx, const string& context_query) {
  if (!ctx || !predict_engine_)
    return;
  if (predict_engine_->Predict(ctx, context_query)) {
    predict_engine_->CreatePredictSegment(ctx);
    self_updating_ = true;
    ctx->update_notifier()(ctx);
    self_updating_ = false;
  }
}

PredictorComponent::PredictorComponent(
    an<PredictEngineComponent> engine_factory)
    : engine_factory_(engine_factory) {}

PredictorComponent::~PredictorComponent() {}

Predictor* PredictorComponent::Create(const Ticket& ticket) {
  int max_commit_interval_seconds = 30;  // 默认 30 秒
  bool legacy_mode = false;
  if (auto* schema = ticket.schema) {
    auto* config = schema->config();
    if (!config->GetInt("predictor/max_commit_interval_seconds",
                        &max_commit_interval_seconds)) {
      DLOG(INFO) << "predictor/max_commit_interval_seconds not set, using "
                    "default (30s)";
    } else {
      DLOG(INFO) << "predictor/max_commit_interval_seconds: "
                 << max_commit_interval_seconds << "s";
    }
    if (!config->GetBool("predictor/legacy_mode", &legacy_mode)) {
      DLOG(INFO) << "predictor/legacy_mode not set, using default (false)";
    } else {
      DLOG(INFO) << "predictor/legacy_mode: "
                 << (legacy_mode ? "true" : "false");
    }
  }
  Predictor* predictor =
      new Predictor(ticket, engine_factory_->GetInstance(ticket));
  predictor->SetMaxCommitIntervalSeconds(max_commit_interval_seconds);
  predictor->SetLegacyMode(legacy_mode);
  return predictor;
}

}  // namespace rime
