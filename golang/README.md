# ukcp golang

Go 版 `ukcp` 模块。当前接口和 `cpp/ukcp_server` 主逻辑保持基本一致，统一使用一套 12 字节包头：

- `KCP` 包
- `UDP` 包
- `sess_id`
- `packet_seq`
- `connect` 标志

## 目录

- `client.go`: Go client
- `server.go`: Go server
- `session.go`: session 发送和生命周期
- `protocol/`: 包头编解码
- `examples/echo/`: 最小 echo 示例

## 服务端公开接口

- `Listen(addr, handler, config)`
- `Serve(conn, handler, config)`
- `(*Server).FindSession(sessID)`
- `(*Server).CloseSession(sessID, reason)`
- `(*Server).SetMtu(mtu)`
- `(*Server).SendKcpToSess / SendKcpToMultiSess / SendKcpToAll`
- `(*Server).SendUdpToSess / SendUdpToMultiSess / SendUdpToAll`

## Session 公开接口

- `(*Session).ID()`
- `(*Session).RemoteAddr()`
- `(*Session).SendKcp(payload)`
- `(*Session).SendUdp(packetSeq, payload)`
- `(*Session).Close()`
- `(*Session).CloseWithReason(reason)`

## Client 公开接口

- `Dial(addr, sessID, config)`
- `(*Client).SendAuth(payload)`
- `(*Client).SendKcp(payload)`
- `(*Client).SendUdp(packetSeq, payload)`
- `(*Client).Recv()`
- `(*Client).Reconnect()`
- `(*Client).Close()`

## 配置约定

- 默认 transport MTU 是 `1024`
- 设置到 KCP 的 MTU 是 `transport MTU - ukcp header(12)`
- KCP 单消息长度上限按配置 MTU 推导，不依赖运行时额外 hook
- `SetMtu` 只影响未来新建 session，不回改已有 session

## 测试

```powershell
cd D:\kup\golang
go test ./...
```
