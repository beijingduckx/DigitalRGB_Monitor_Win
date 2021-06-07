# DigitalRGB_Monitor for Windows
[Minatsuさんの Raspberry Piを Digital RGB Monitorにするプログラム](https://github.com/MinatsuT/RPi_DigitalRGB_Monitor)をWindows用(SDL利用)に変更したものです  
Old PCの画面をウィンドウ内に表示できます

追加機能として、RGB端子にドットクロックが出ていない機種にも対応しています (付加回路が必要)

ドットクロック生成を付加した回路を用い、手元のX1turbo(標準解像度), Pasopia7(ドットクロック出力不使用) が映ることを確認しています


## 必要ライブラリ
以下のライブラリを利用しています
- [Cypress EZ-USB FX3 SDK](https://www.cypress.com/documentation/software-and-drivers/ez-usb-fx3-software-development-kit?source=search&cat=software_tools)
- [SDL 2.0](https://www.libsdl.org/)

## コンパイル
Windows用 PowerShell上で、以下の手順で `slave.inc`を生成します
```
PS> sdcc -mmcs51 -I. .\slave.c
PS> .\gen_inc.ps1 > slave.inc
```

生成した`slave.inc`をWindows用のソースコードのフォルダにコピーし、Windows用のプログラムをコンパイルします

VisualStudio2019での動作を確認しています

リンクには、少なくとも CyAPI.lib, SDL2.lib, SDL2main.lib が必要です

## 動作
- カーソルキー: 表示位置を調整します
- `a` `s`: 水平方向の総ドット数を調整します (CS2300-CP接続時のみ)
- `x`: USB通信を再起動します 画面モードが切り替わって画面が乱れた時の回復に

## 制限
- プログラム起動前に、EZ-USBにDigital RGB信号を入力してください
- 起動後に映像信号(同期信号)が失われた時の処理は不十分で、ハングアップする可能性が高いです
- 最大解像度は 640x200 と思われます

## libusbからEZ-USB FX3 SDKへの変更について
オリジナルはlibusbを利用していましたが、Windows版の作成にあたりEZ-USB FX3 SDKを利用しています。
これは、Windows版のlibusb(WinUSBバックエンド)では、安定した表示に必要な転送パフォーマンスが得られなかったためです。

## 回路/動作について
- ドットクロックは、水平同期周波数を逓倍して生成しています (Cirrus Logic製 CS2300-CPを利用)
- CS2300から出力されるクロックはフリーランで、水平同期信号と位相が合いません ずれも一定ではありません  
このため、本来のドットクロックの倍のクロックを生成させ、水平同期信号の開始を検知したら、そのクロックを画素サンプルのタイミング基準とし
1クロックおきに画素をサンプルしています  
手元のPCではこれで水平方向の表示は安定していますが、機種によっては安定しないかもしれません  
その場合は、水平同期から設定する基準クロックを、1クロック後にすると良いかもしれません
- 回路図ではRGBHV全てを真面目にレベル変換していますが、CS2300に入力する水平同期信号のみのレベル変換でも動作するはずです
(設計当初、CS2300からのクロックと水平同期との位相が合うものと勘違いしました 
映像信号の遅延を合わせるために、全ての信号のレベル変換を行うように設計したのです)
