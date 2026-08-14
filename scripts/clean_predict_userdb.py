#!/usr/bin/env python3
"""清理 predict.userdb txt 格式：
   1. trim prefix 尾部空格 + word 首尾空格
   2. 删除 trim 后自预测（前缀 == 词）
   3. 合并同 (prefix, word) 的 weight（取 max，commits 累加，dee/tick 取 max）
   4. 修复 int32 溢出负数（commits/dee/tick 取 abs）

用法：
    python3 clean_predict_userdb.py <input.txt> <output.txt>

示例：
    python3 clean_predict_userdb.py /tmp/predict.txt /tmp/predict.cleaned.txt
"""
import sys
from collections import defaultdict


def main():
    if len(sys.argv) != 3:
        print("usage: clean_predict_userdb.py <input> <output>", file=sys.stderr)
        sys.exit(1)

    input_path, output_path = sys.argv[1], sys.argv[2]
    removed_self = 0
    trimmed = 0
    fixed_neg = 0
    total_lines = 0
    agg = {}

    with open(input_path, 'r', encoding='utf-8') as fin:
        for line in fin:
            raw = line.rstrip('\n')
            if not raw or raw.startswith('#'):
                continue
            total_lines += 1
            cols = raw.split('\t')
            if len(cols) == 3:
                prefix, word, weight_str = cols[0], cols[1], cols[2]
                commits, dee, tick = 0, 0.0, 0
            elif len(cols) == 6:
                prefix, word, weight_str, commits_str, dee_str, tick_str = cols
                try:
                    commits = int(commits_str)
                except ValueError:
                    commits = 0
                try:
                    dee = float(dee_str)
                except ValueError:
                    dee = 0.0
                try:
                    tick = int(tick_str)
                except ValueError:
                    tick = 0
            else:
                continue
            try:
                weight = float(weight_str)
            except ValueError:
                continue
            # 修复负数（int32 溢出值转正）
            if commits < 0:
                commits = abs(commits) if abs(commits) < 2**31 else 0
                fixed_neg += 1
            if tick < 0:
                tick = abs(tick) if abs(tick) < 2**31 else 0
                fixed_neg += 1
            if dee < 0:
                dee = abs(dee)
                fixed_neg += 1
            # trim
            new_prefix = prefix.rstrip()
            new_word = word.strip()
            if new_prefix != prefix:
                trimmed += 1
            # 跳过 trim 自预测
            if new_prefix and new_prefix == new_word:
                removed_self += 1
                continue
            key = (new_prefix, new_word)
            if key in agg:
                cur = agg[key]
                cur['weight'] = max(cur['weight'], weight)
                cur['commits'] += commits
                cur['dee'] = max(cur['dee'], dee)
                cur['tick'] = max(cur['tick'], tick)
            else:
                agg[key] = {
                    'weight': weight,
                    'commits': commits,
                    'dee': dee,
                    'tick': tick,
                }

    with open(output_path, 'w', encoding='utf-8') as fout:
        fout.write('# prefix<TAB>word<TAB>weight<TAB>commits<TAB>dee<TAB>tick\n')
        for (p, w), v in agg.items():
            fout.write(
                f"{p}\t{w}\t{v['weight']:.6f}\t{v['commits']}\t"
                f"{v['dee']:.6f}\t{v['tick']}\n"
            )

    print(
        f"total_lines={total_lines} removed_self={removed_self} "
        f"trimmed={trimmed} fixed_neg={fixed_neg} "
        f"unique_merged={len(agg)}",
        file=sys.stderr,
    )


if __name__ == '__main__':
    main()
