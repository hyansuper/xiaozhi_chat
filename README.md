# 小智 AI as an esp-idf component

用 C 语言重新实现了小智的通信协议. [MCP](https://github.com/hyansuper/xiaozhi_chat/src/xz_chat.c#L433) 部分还没实现，服务器端 AEC 不支持

音频编解码和录音、播放以及 UI 界面需要在回调函数中自行处理，见 example 

## dependencies:

	see my_esp_util
