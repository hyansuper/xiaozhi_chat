使用方法和 [coze_ws_app](https://github.com/espressif/esp-adf/tree/master/examples/ai_agent/coze_ws_app) 十分相似.

xiaozhi_chat 组件只负责数据的接受和发送。语音唤醒，录音和播放，还有opus编解码是通过 esp-gmf 组件完成的。

注意：
1. 这个例子没有 UI, 设备注册的提示是通过串口打印
1. 与原生小智的略微区别是，说完"你好小智"唤醒词之后，他不会回答“你好呀，最近有什么好玩的事”之类，而是ding的一声便进入倾听模式。

因为使用 gmf_loader，许多默认配置是通过 menuconfig，你还需要根据自己的硬件修改 board.c 中的引脚和驱动。例子中用的是 ESP32S3R8 / 16M flash / es8311 / 单麦 / 无 AEC

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