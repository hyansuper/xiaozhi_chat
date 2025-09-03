使用方法和 [coze_ws_app](https://github.com/espressif/esp-adf/tree/master/examples/ai_agent/coze_ws_app) 十分相似.

xiaozhi_chat 组件和 esp_coze 类似，只是完场数据的接受和发送。语音唤醒，录音和播放，还有opus编解码是通过 esp-gmf 组件完成的。

你需要根据自己的硬件修改 board.c 中的引脚和驱动，默认的是 ESP32S3R8 / 16M flash / es8311 / 单麦

## Build

```sh
git clone https://github.com/hyansuper/xiaozhi_chat.git
cd xiaozhi_chat
git submodule update --init -recursive

cd example
idf.py set-target esp32s3
idf.py menuconfig
       -> set wifi ssid/password
idf.py build
idf.py flash monitor
```