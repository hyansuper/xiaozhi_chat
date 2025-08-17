# 小智 AI as an esp-idf component

用 C 语言重新实现了小智的通信协议. MCP 部分还没实现

音频编解码和录音、播放以及 UI 界面需要在回调函数中自行处理，配合 esp-gmf, 可参考 [coze_ws_app](https://github.com/espressif/esp-adf/tree/master/examples/ai_agent/coze_ws_app)

## dependencies:

	see my_esp_util
