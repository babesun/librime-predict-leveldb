# librime-predict-leveldb
librime plugin. predict next word by commit history.

a mod of `rime/librime-predict`

## Usage
* In `*.schema.yaml`, add `predictor` to the list of `engine/processors` **before `express_editor`**
(must be positioned before the editor so that Backspace events reach the predictor),
add `predict_translator` to the list of `engine/translators`;
or patch the schema with:
```yaml
patch:
  # 放在 express_editor 之前（predictor 才能收到 Backspace 事件）
  'engine/processors/@before last': predictor
  'engine/translators/@before 0': predict_translator
```

> **注意**：`engine/processors/+`（末尾追加）会把 predictor 放到 express_editor 之后，
> 导致 Backspace 事件被 express_editor 的 keymap (`RevertLastEdit`) 拦截，
> 预测窗不会关闭，应用也收不到 BackSpace。必须使用 `@before last` 把 predictor 放在
> express_editor 之前。

* Add the `prediction` switch:
```yaml
switches:
  - name: prediction
    states: [ 關閉預測, 開啓預測 ]
    reset: 1
```
* Config items for your predictor:
```yaml
predictor:
  # predict db folder in user directory
  # default to 'predict.userdb'
  # be careful when there is a schema named `predict`, in which case you should reset value of this `predictdb` key to another name for better compcompliance.
  predictdb: predict.userdb
  # max prediction candidates every time
  # default to 0, which means showing all candidates
  # you may set it the same with page_size so that period doesn't trigger next page
  max_candidates: 5
  # max continuous prediction times
  # default to 0, which means no limitation
  max_iterations: 1
  # 提交时间间隔阈值（秒）
  # 超过此时间间隔的两次提交，将不会被认为有关联
  # 用于避免将长时间间隔后的输入错误地关联在一起
  # 默认值：30 秒
  # 设为 0 或负值可禁用时间限制（恢复旧版行为，始终关联所有提交）
  # 建议范围：10-120 秒，根据实际使用习惯调整
  max_commit_interval_seconds: 30
  # 兼容模式：启用后按 librime-predict 的方式预测候选
  # - 不做 leveldb 扩展的提交关系学习与删除更新
  # - 仅按最近提交词进行预测
  # - 数据源切换为用户目录下的 predictor/db（默认 predict.db）
  # 默认值：false（使用 librime-predict-leveldb 现有逻辑）
  legacy_mode: false
  # legacy_mode=true 时生效，默认 predict.db
  db: predict.db
```
* Deploy and enjoy.

## Bug Fixes

### 1. Backspace 不关闭预测窗，且应用收不到 BackSpace（macOS 适配问题）

**现象**：在 Word 等 macOS 应用中输入触发预测窗后，按 BackSpace 键：
- 应用中的文字不删除
- 预测窗不关闭

**根因**：
- `express_editor` 通过 keymap 把 `XK_BackSpace` 绑到 `RevertLastEdit`，返回 `kAccepted`
- 当 predictor 排在 `express_editor` 之后时，永远收不到 BackSpace 事件
- macOS 的 `NSTextInputClient` 协议要求 `setMarkedText("")` 退出 marked-text 状态后，应用才会响应 BackSpace
- 原代码用 `ctx->Clear()` 拦截 BackSpace，阻止了 setMarkedText 的更新链路

**修复**：
- 把 predictor 放在 `engine/processors` 中 `express_editor` 之前（用 `@before last`）
- predictor 的 BackSpace 处理改为：剥 prediction 段（`pop_back()`）+ `return kNoop` 让按键透传
- 剥段后主动触发 `ctx->update_notifier()(ctx)`，让 Squirrel 刷新 UI 退出 marked-text 状态

### 2. Shift+字母导致预测词误上屏

**现象**：在 Squirrel 预测窗显示时按住 Shift 输入字母（如 Shift+A），预测词会跟着字母一起上屏。

**根因**：
- Shift+字母在 macOS 下 Squirrel 不把字母键发给 RIME 的 ProcessKeyEvent，而是直接提交
- Squirrel 通过 `RimeGetContext` 读取 `commit_text_preview`，含当前预测词
- `setMarkedText` 走 `commit_text_preview`，导致预测词误上屏

**修复**：
- Shift 按下时：备份当前 `selected_candidate.text` 到 `shift_shadow_text_`，并把 `back.selected_index` 设为 `SIZE_MAX`
- `SIZE_MAX` 让 `Composition::GetCommitText()` 拿不到预测词 → `commit_text_preview` 为空 → Squirrel `setMarkedText("")` 不带预测词
- menu 仍非空 → `Context::HasMenu()` 仍为 true → 候选窗保留
- Shift 释放时：恢复 `selected_index = 0`，清 shadow

### 3. Shift+Delete 删词功能保留

**修复**：在 `OnDelete` 中增加 shadow 兜底——当 Shift 期间 `Context::GetSelectedCandidate()` 返回 null 时，用 `shift_shadow_text_` 替代，删词功能不受影响。

### 4. 误上屏预测词清理

**场景**：Shift+字母场景下，前端把当前预测候选（`type=prediction`）当作提交直接上屏，而非用户主动选词。

**修复**：`OnContextUpdate` 中检测 `commit_history.back().type == "prediction"` 且 `last_action_ != kSelect` 时，清空预测状态和 commit_history，不重建预测窗。

### 5. 自预测防御

**场景**：用户删除刚上屏的字又重打一遍同样的词时，会写入无意义的"词\t词"自预测记录，导致下次预测把刚输入的词又弹回来。

**修复**：在 `predictor.cc`（`OnContextUpdate`）和 `predict_engine.cc`（`UpdatePredict`）两层都加上 `key == word` 的跳过判断，源头拦截自预测记录写入。

### 6. 防御 prefix/word 含空格的死数据

**现象**：用户词典 `predict.userdb` 中积累大量 `prefix<空格>` 形式的死数据（实测 8751 条，占总量 48%）。这些记录占空间、拖慢 Lookup，但永远不会被命中。

**根因**：
- RIME 在用户按空格键（`XK_space`，键码 0x20）时，会通过 `commit_history` 写入一条单空格 record（见 [src/rime/commit_history.cc:25-28](https://github.com/rime/librime/blob/master/src/rime/commit_history.cc)）
- 这导致 `last_timed_commit_.text` 末尾可能带空格
- `predict_engine` 写入 `prefix<TAB>word` 形式的 key
- `Lookup` 时用 `query + "\t"` 作前缀查询，`query` 不会带尾部空格 → 死数据无法被命中

**修复**：
- `OnContextUpdate` 中 `UpdatePredict` 调用前 `trim` 掉 `prefix`/`word` 前后空白字符
- trim 后任一字段为空，或两者相等（trim 自预测），则跳过
- 已存在的死数据需要用 `predict_data_tool` 一次性清理（见下方"清理历史死数据"）

### 清理历史死数据

如果之前使用过本插件，`predict.userdb` 中可能积累大量带空格的死数据。清理步骤：

```bash
# 1. 退出 Squirrel（避免 leveldb lock）
osascript -e 'quit app "Squirrel"'

# 2. 备份并导出为 txt
cp -pR ~/Library/Rime/predict.userdb ~/Library/Rime/predict.userdb.bak
build/plugins/librime-predict-leveldb/predict_data_tool \
  --from leveldb --to txt \
  --input ~/Library/Rime/predict.userdb \
  --output /tmp/predict.txt

# 3. 用 Python 清理（trim + 合并 weight + 删除自预测）
python3 scripts/clean_predict_userdb.py /tmp/predict.txt /tmp/predict.cleaned.txt

# 4. 写回 leveldb
build/plugins/librime-predict-leveldb/predict_data_tool \
  --from txt --to leveldb \
  --input /tmp/predict.cleaned.txt \
  --output ~/Library/Rime/predict.userdb

# 5. 重启 Squirrel
open -a Squirrel
```

## Data conversion tool

Build target `predict_data_tool` currently supports conversion between leveldb
(`predict.userdb`) and txt.

Python version is also provided at `scripts/predict_data_tool.py` (requires
`plyvel`).

```bash
# python tool (recommended for quick use)
pip install plyvel
python3 scripts/predict_data_tool.py \
  --from leveldb --to txt --input ./predict.userdb --output ./predict.txt

# leveldb -> txt
./build/plugins/librime-predict-leveldb/predict_data_tool \
  --from leveldb --to txt --input ./predict.userdb --output ./predict.txt

# txt -> leveldb
./build/plugins/librime-predict-leveldb/predict_data_tool \
  --from txt --to leveldb --input ./predict.txt --output ./predict.userdb
```

txt format (tab-separated):

```text
prefix<TAB>word<TAB>weight
# or
prefix<TAB>word<TAB>weight<TAB>commits<TAB>dee<TAB>tick
```
