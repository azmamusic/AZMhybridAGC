# AZMhybridAGC

ステレオ入力のラウドネスと RMS を再生中ずっと蓄積し、両者を組み合わせたレベルが目標に近づくように、左右連動でゲインを緩やかに調整する JSFX です。入力側で測定するため、補正後の音量が学習値にフィードバックされることはありません。

**リミッターやピーク保護はありません。** ブーストによりピークが 0 dBFS を超えることがあるため、必要に応じて後段にリミッターを置いてください。

## 導入

1. REAPER の「Options → Show REAPER resource path in explorer/finder」でリソースフォルダーを開きます。
2. `jsfx/AZMhybridAGC.jsfx` を、その中の `Effects/AZMhybridAGC/` にコピーします。
3. FX ブラウザーを更新し、`AZMhybridAGC` をステレオトラックまたはモニター経路に挿入します。

音声がプラグインに届いていれば、トランスポート停止中も動作します。ループバックなどを使用する場合は、ホスト側で入力モニタリングとルーティングを設定してください。ホストが停止中のエフェクト処理を中断する場合、その間は測定・補正できません。

## 操作

画面上部の大きな数値は実際に出力に適用されるゲインです。下の数値は入力の **LOUDNESS**、較正後の **COMBINED RMS**、両者を混合した **HYBRID LEVEL** です。測定可能なブロックがまだないときは `--` と表示されます。`SETTINGS` を開くと以下のノブを操作できます。ノブは上下ドラッグで調整し、ダブルクリックで初期値、右クリックで数値入力、ホイールで微調整、Shift 併用でさらに細かく調整できます。ノブや欄にポインターを置くと、下部のバーに名前・値・説明を表示します。

GUI のフォント、余白、配色、アンチエイリアス（2 倍解像度で描画して縮小）は [AZMbeziExp](https://github.com/azmamusic/AZMbeziExp) に合わせています。メーター（`APPLIED GAIN` のパネル）の大きさは、`SETTINGS` の開閉に関係なく同じです。初期ウィンドウは 570 × 353 で、`SETTINGS` を開いた状態が収まる高さです。ホストがウィンドウを自動でリサイズしなくても、メーターは変わらず、`SETTINGS` を閉じると下に空きができます。

| 設定 | 範囲 | 初期値 | 動作 |
|---|---:|---:|---|
| Hybrid Target | −36〜−6 dB | −16 dB | 混合レベルの目標値 |
| Push Down | 0〜6 dB | 6 dB | 目標ゲインの減衰側の上限 |
| Pull Up | 0〜6 dB | 6 dB | 目標ゲインの増幅側の上限 |
| Smoothing | 0.1〜30 秒 | 3 秒 | 目標ゲインの移動平均窓。増減共通 |
| Calibration | −12〜+12 dB | 0 dB | RMS へのオフセット |
| Ratio | 0〜100% | 50% | RMS の混合割合。0% は Loudness のみ、100% は RMS のみ |

`SILENCE` は無音による自動リセット（初期値 On）、`PLAYBACK` は停止・一時停止から再生への遷移によるリセット（初期値 Off）、`RESET` は手動リセットです。再生リセットを Off にすると、停止中のモニタリングで蓄積した値を再生開始後も維持します。ホストが停止中に処理を中断し、停止状態をプラグインに通知しない場合は、再生開始を検出できないことがあります。

無音リセットは、ステレオ平均パワーが **−60 dBFS 未満**の 100 ms 区間が **5 秒**続くと実行されます。閾値と時間は現バージョンでは固定です。無音中はゲインが増加しないようにしています。

`BYPASS` は約 5 ms でドライ信号へ切り替えます。プラグイン内の Bypass 中も測定は続きます。ホスト側の FX バイパスは別であり、その場合の処理継続はホストに依存します。

## 測定と補正

- 最初の 400 ms の測定ブロックができるまではゲイン調整を開始しません。以後は 100 ms ごとに、400 ms の重複ブロックを蓄積します。再生時間による固定窓ではなく、リセットまで学習し続けます。
- Loudness はステレオの K-weighting によるパワーから求めます。−70 LUFS の絶対ゲートを通過した全ブロックの平均から相対ゲート（−10 LU）を算出し、条件に合うブロックを選択し直します。RMS も同じ選択ブロックについて、フィルター前の左右平均パワーから求めます。
- 長時間でもメモリ使用量を一定にするため、ブロックは 0.1 LU 刻みのヒストグラムに集約します。相対ゲート境界ではビンの中央値で判定するため、厳密な EBU R128 Integrated Loudness と完全には一致しません。
- `Hybrid Level = (1 − Ratio) × Loudness + Ratio × (RMS + Calibration)`（Ratio は 0〜1）。それぞれ dB 値を混合します。目標ゲインは `Hybrid Target − Hybrid Level` を Push Down / Pull Up の範囲に制限した値です。ゲインはその目標値の移動平均に向かって 100 ms ごとに補間され、急に切り替わりません。
- 手動・無音・再生リセットとサンプルレート変更時には測定履歴を消去し、再び 400 ms のブロックから学習します。極端な入力で測定範囲を超えた場合は `MEASUREMENT ERROR - RESET` を表示してゲインを固定します。入力レベルを確認してからリセットしてください。

## 開発・検証

統合テストは外部の [ysfx](https://github.com/jpcima/ysfx) ランタイムを使用します。Linux での一例（CMake、Ninja、C++ コンパイラー、`pkg-config`、Fontconfig / FreeType の開発ファイルが必要）:

```sh
# このリポジトリの親ディレクトリで実行
git clone https://github.com/jpcima/ysfx.git
git -C ysfx checkout 8077347ccf4115567aed81400281dca57acbb0cc
git -C ysfx submodule update --init --recursive
cmake -S ysfx -B ysfx/build -G Ninja -DYSFX_PLUGIN=OFF -DYSFX_GFX=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ysfx/build
cd AZMhybridAGC
g++ -std=c++17 -O2 -Wall -Wextra -I../ysfx/include tests/test_ysfx.cpp \
  ../ysfx/build/libysfx.a $(pkg-config --libs freetype2 fontconfig) -lpthread -ldl \
  -o /tmp/azmhybridagc-test
/tmp/azmhybridagc-test
```

実際のホストごとのルーティングや UI サイズ変更は、ホスト上でも確認してください。
