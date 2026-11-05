# AviUtlPathDraw（AviUtlPass / Path Draw）

[試用版をダウンロード](https://github.com/GGAkyo/AviUtlPathDraw/releases/latest)

AviUtl 1.x用のパス描画プラグインです。キャンバスで点を編集して線・曲線を描き、線幅・RGB・丸い線端・塗りつぶしを調整します。開発時のフォルダー名はAviUtlPassです。AviUtl2には対応していません。

## 導入と操作

1. AviUtlを終了し、配布ZIPの `path_draw.auf` を `plugins` へコピーします。
2. 「設定」からPath Drawを開きます。
3. `New / Clear` で描画用IDを作成し、`Show Canvas` でキャンバスを表示します。
4. クリックで点を追加し、点をドラッグして形を調整します。Size・RGB・Round Cap・Fill Insideで見た目を変更します。

**開発版です。** パスは作業フォルダーの `path_draw_save.dat` に保存され、AviUtlプロジェクトだけでは完結しません。プロジェクトを渡す際はこのファイルも保管してください。現時点では通常フィルタとして提供し、文字や図形へ適用する完成した拡張編集フィルタではありません。必ず別の検証用プロジェクトで試してください。

## ビルド

Visual StudioのC++ x86環境とCMakeを使います。依存元はBlend2DとAsmJitです。

```powershell
git clone --recurse-submodules https://github.com/GGAkyo/AviUtlPathDraw.git
cd AviUtlPathDraw
cmake -S . -B build -A Win32
cmake --build build --config Release
```

`build/Release/path_draw.auf` が生成されます。旧Lua版の `path_draw.anm` は現在のDLLと方式が異なるため配布しません。

## ライセンス

自作コードはMIT。Blend2D・AsmJitおよびAviUtl SDKには各配布元の著作権表示・ライセンスが適用されます。`THIRD_PARTY_NOTICES.md` を参照してください。

[作者のツール一覧](https://sonikiwa.netlify.app/tools.html)
