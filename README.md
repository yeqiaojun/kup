# kup

多语言 `ukcp` 实现和互操作测试仓库。

## 功能

- 统一 `ukcp` 协议头
  所有实现统一使用一套 header，包含 `msg_type`、`flags`、`body_len`、`sess_id`、`packet_seq`。

- KCP / UDP 双通道
  支持可靠 KCP 消息和原始 UDP 消息两类收发接口。

- 会话管理
  服务端按 `sess_id` 管理 session，支持认证、查找 session、主动关闭 session、向单个/多个/全部 session 发包。

- 快速重连
  支持同一 `sess_id` 的重连和 fast reconnect 窗口处理。

- MTU 控制
  支持设置 transport MTU，并同步到 KCP MTU。默认 transport MTU 是 `1024`。

- 长度限制
  KCP 单消息长度上限按配置 MTU 推导，不依赖额外运行时 hook。

- 多语言互操作
  当前包含 C++ 服务器、C# 客户端、Go 实现和跨语言互操作测试。

## 目录

- `cpp/ukcp_server`
  C++ 服务器实现和测试，当前主对齐版本。

- `csharp/UkcpSharp`
  C# client 库。提供连接、认证、KCP 发送、UDP 发送、接收、关闭等能力。

- `csharp/UkcpSharp.Console`
  C# 控制台客户端，用于互操作和手工联调。

- `csharp/UkcpSharp.Tests`
  C# 测试。

- `golang/`
  Go 版 `ukcp` 模块、测试和示例。当前 server/client 主逻辑已基本对齐 C++ 版本。

- `docs/`
  设计和阶段性计划。

## 当前约定

- 前后端统一使用一套 `ukcp` header
- 默认 transport MTU 是 `1024`
- 设置到 KCP 的 MTU 是 `transport MTU - ukcp header`
- KCP 单消息长度上限按配置 MTU 推导

## 常用命令

Go:

```powershell
cd D:\kup\golang
go test ./...
```

C#:

```powershell
cd D:\kup
dotnet test csharp/UkcpSharp.Tests/UkcpSharp.Tests.csproj
```

C++:

```powershell
cd D:\kup\cpp\ukcp_server
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build-codex --config Release --target ukcp_server_tests
D:\kup\cpp\ukcp_server\build-codex\Release\ukcp_server_tests.exe
```
