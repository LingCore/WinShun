# ShunSync v1 跨平台剪贴板共享协议规范

ShunSync v1 是 **Mac顺（MacShun）** 与 **Win顺（WinShun）** 共用的跨设备剪贴板共享协议，面向 **1+ 台 Mac 与多台 Windows** 组成的个人/团队对等设备组，支持实时文字、图片与大文件/文件夹的端到端加密同步，不依赖账号或第三方云端服务器。

- 协议版本：`proto = 1`
- 默认 TCP 监听端口：`37219`（冲突时自动改用系统分配端口并通过发现广播）
- 默认 UDP 局域网发现端口：`37220`
- Bonjour / mDNS 服务类型：`_shunsync._tcp`
- 跨语言测试向量：[docs/shunsync-test-vectors.json](shunsync-test-vectors.json)（由 Mac顺 Swift 单元测试与 Win顺 C++ 单元测试共同校验）

---

## 1. 身份与群组模型

每台设备本地持久化一份身份文件（macOS 存于 `~/Library/Application Support/MacShun/Sync/identity.json`，权限 `0600`；Windows 建议存于 `%LOCALAPPDATA%\WinShun\Sync\identity.json`）：

- `deviceId`：小写 UUID 字符串（不含大括号，如 `"550e8400-e29b-41d4-a716-446655440000"`；Qt 侧须使用 `QUuid::toString(QUuid::WithoutBraces)`），每台电脑首次启用时随机生成。
- `groupKey`：32 字节随机根密钥（AES-256 级熵），同一同步组内的所有 Windows 与 Mac 共用。
- `groupId`：群组公开标识（32 字符小写十六进制，不泄露根密钥）：
  ```text
  groupId = hexLower(SHA256("shunsync:gid:" || groupKey)[0..15])
  ```
- `revokedDeviceIds`：已被移除的设备 ID 列表。

### 群组邀请码（跨子网 / Tailscale / 离线配对）

```text
shunsync://join?k=<base64url_nopad(groupKey)>[&p=<percent_encoded(host:port)>]
```

解码端同样应兼容裸 32 字节 Base64 / Base64URL 字符串（去除首尾空白）。

---

## 2. 局域网与跨网设备发现

三轨并行，任何一轨命中即可建连：

1. **mDNS / Bonjour（`_shunsync._tcp`）**
   - TXT 记录键：`v="1"`、`id=<deviceId>`、`gid=<groupId>`、`name=<deviceName>`、`os="macos"|"windows"`、`port=<tcpPort>`、`pair="0"|"1"`。
2. **UDP 局域网广播信标（端口 `37220`）**
   - 每 5 秒向活跃物理 IPv4 网卡的广播地址（跳过回环、WSL/Docker/utun/vEthernet 等虚拟网卡）发送 UTF-8 JSON：
     ```json
     {
       "proto": 1,
       "deviceId": "...",
       "deviceName": "办公室台式机",
       "platform": "windows",
       "groupId": "32-hex-chars",
       "tcpPort": 37219,
       "pairingOpen": false
     }
     ```
   - 接收端从 UDP 来源包头获取对端 IP，与 `tcpPort` 组成连接地址。
3. **历史 IP 缓存 + 手动设备地址 + 同组 Gossip**
   - 记住曾成功连过的同组设备 IP，支持用户手动输入 `host:port`（如 Tailscale IP `100.64.0.2:37219`）。
   - 认证成功后通过 `auth_ok.peers` 与 `peer_update.peers` 交换同组其他成员地址，让 3+ 台设备自动形成全互联 Mesh。

---

## 3. 线缆分帧与端到端加密

### 3.1 TCP 分帧格式

所有报文（包括握手明文阶段与握手后的密文阶段）统一采用 4 字节大端无符号长度前缀：

```text
+-------------------+-----------------------------+
| length (4B BE)    | payload (length bytes)      |
+-------------------+-----------------------------+
```

- 最大明文帧长度：`20 * 1024 * 1024` 字节（20 MB）；密文帧上限加 16 字节 GCM Tag。
- `length == 0` 或超限均视为协议错误并立即断开。

### 3.2 握手与密钥派生（X25519 + HKDF-SHA256 + HMAC-SHA256）

无需 X.509 证书，每次 TCP 连接使用独立临时 X25519 密钥对，提供前向保密与双向认证。

1. **Client → Server（明文 JSON 帧）**：
   ```json
   {
     "type": "hello",
     "proto": 1,
     "mode": "sync",
     "groupId": "32-hex-chars-or-null-for-pair",
     "deviceId": "client-uuid",
     "deviceName": "公司台式机",
     "platform": "windows",
     "ephPub": "<base64(32B X25519 pubkey)>",
     "nonce": "<base64(16B random)>",
     "listenPort": 37219
   }
   ```
   `mode` 取值为 `"sync"`（常驻控制与剪贴板通道）、`"file"`（按需文件流通道）或 `"pair"`（6 位码配对通道）。

2. **共享秘密与 Transcript 计算**：
   ```text
   sharedSecret = X25519(myEphPriv, peerEphPub)   // 32 bytes
   transcript   = SHA256(
                    "shunsync:v1:" || mode ||
                    clientNonce (16B) ||
                    serverNonce (16B) ||
                    clientEphPub (32B) ||
                    serverEphPub (32B) ||
                    UTF8(clientDeviceId) || 0x00 || UTF8(serverDeviceId)
                  )                               // 32 bytes
   ```

3. **同步 / 文件模式（`mode = "sync" | "file"`）**：
   - 服务端检查 `hello.groupId == local.groupId` 且 `hello.deviceId` 未在吊销列表中。
   - 双方派生证明与双向会话密钥：
     ```text
     serverProof = HMAC-SHA256(key = groupKey, msg = "server:" || transcript)
     clientProof = HMAC-SHA256(key = groupKey, msg = "client:" || transcript)
     ikm         = sharedSecret || groupKey                               // 64 bytes
     c2sKey      = HKDF-SHA256(ikm, salt = transcript, info = "shunsync:v1:c2s", len = 32)
     s2cKey      = HKDF-SHA256(ikm, salt = transcript, info = "shunsync:v1:s2c", len = 32)
     ```
   - **Server → Client（明文 JSON 帧）**：
     ```json
     {
       "type": "hello_ack",
       "proto": 1,
       "accepted": true,
       "deviceId": "server-uuid",
       "deviceName": "MacBook Pro",
       "platform": "macos",
       "ephPub": "<base64(32B)>",
       "nonce": "<base64(16B)>",
       "proof": "<base64(32B serverProof)>",
       "listenPort": 37219
     }
     ```
   - 收到 `accepted == true` 后，双方立即启用 AES-256-GCM 通道（Client 发送用 `c2sKey`、接收用 `s2cKey`；Server 相反）。
   - Client 恒定时间校验 `proof == serverProof`，随后发送第 1 个加密 JSON 帧：
     ```json
     { "type": "auth_finish", "proof": "<base64(32B clientProof)>" }
     ```
   - Server 恒定时间校验 `clientProof`，通过后回复加密 JSON 帧：
     ```json
     { "type": "auth_ok", "peers": [ { "deviceId": "...", "deviceName": "...", "platform": "windows", "host": "192.168.1.20", "port": 37219 } ] }
     ```

4. **6 位确认码配对模式（`mode = "pair"`）**：
   - 开启共享后，Server 默认允许响应局域网或直连设备的 6 位码配对请求（内置防骚扰频控：同一来源 8 秒内最多 1 次，同时只处理 1 个会话；若用户在高级设置中关闭了配对响应，则仅在手动开启 120 秒临时窗口期间接受，否则返回 `hello_ack { accepted: false, error: "pairing_closed" }`）。
   - 客户端主动直连某 IP（`mode="sync"`）收到 `wrong_group` 时，推荐无缝自动转入 `mode="pair"` 发起 6 位码配对，避免用户来回操作。
   - 拒绝时 `hello_ack` 为 `{ "accepted": false, "error": "<code>" }`，两端使用的 `error` 取值：`wrong_group`（不在同一组，`mode="sync" | "file"`）、`revoked`（已被移除）、`pairing_closed`（不接受配对）、`pairing_busy`（已有一个配对在等确认）、`pairing_rate_limited`（同一来源 8 秒内又来）、`bad_hello`（hello 格式不对；Win顺 直接断开）。Win顺 另有 `paused`（本机暂停了同步）和 `self`（连到了自己）。
   - 双方计算一致的 6 位短认证码（SAS）与配对密钥：
     ```text
     raw32   = bigEndianUInt32(transcript[0..3])
     sasCode = printf("%06u", raw32 % 1000000)
     c2sKey  = HKDF-SHA256(sharedSecret, salt = transcript, info = "shunsync:v1:pair-c2s", len = 32)
     s2cKey  = HKDF-SHA256(sharedSecret, salt = transcript, info = "shunsync:v1:pair-s2c", len = 32)
     ```
   - 收到 `hello_ack(accepted: true)` 后双方切换为配对密钥加密通道并各自展示 `sasCode`。
   - 用户在 Server 核对两台电脑 6 位码一致并点击同意后，Server 发送加密帧：
     ```json
     { "type": "pair_grant", "groupKey": "<base64(32B)>", "peers": [...] }
     ```
     或拒绝时发送 `{ "type": "pair_reject", "reason": "declined" }`；Client 中途取消发送 `{ "type": "pair_cancel" }`。
   - Client 收到 `pair_grant` 后保存 `groupKey` 并断开配对连接，立刻以 `mode = "sync"` 接入群组 Mesh。

### 3.3 对称帧加密（AES-256-GCM）

每个发送方向各自维护从 `0` 开始严格递增的 64 位序号 `seq`（每发 1 帧加 1，防重放与乱序）：

- **Nonce（12 字节）**：`0x00 0x00 0x00 0x00 || bigEndianUInt64(seq)`
- **AAD（8 字节）**：`bigEndianUInt64(seq)`
- **线缆负载**：`ciphertext || tag(16 bytes)`（无需在包内重复传输 Nonce）

### 3.4 加密帧明文格式

明文首字节为 `frameKind`：

| `frameKind` | 类型 | 负载结构 |
|---|---|---|
| `0x01` | JSON 控制消息 | UTF-8 JSON 对象（含 `"type"` 字段） |
| `0x02` | 二进制文件分片 | `[16B transferId UUID 原始字节][4B fileIndex BE][8B offset BE][raw bytes]`（仅在 `mode="file"` 连接传输，建议每片 256 KB） |
| `0x03` | 二进制图片条目 | `[4B metaLen BE][UTF-8 JSON ClipImageMeta][raw PNG bytes]`（免 Base64 膨胀） |

---

## 4. 控制消息与剪贴板同步语义

### 4.1 多机 Mesh 建连去重与心跳

- 当两台设备几乎同时向对方发起 `mode="sync"` 连接（5 秒内并发碰撞）时，**保留 `deviceId` 字典序较小一侧发起的连接**，另一条静默关闭。
- 每 20 秒发送 `{ "type": "ping", "ts": 1700000000000 }`，对方回 `{ "type": "pong", "ts": ... }`；60 秒无任何收包视为断线重连。
- 设备改名或成员变动时发送 `peer_update` 广播最新名称与已知成员地址。

### 4.2 防回环与并发仲裁（3+ 台设备）

1. **单跳广播**：只广播本机新产生的复制，永不二次转发远端收到的消息。
2. **本机写入抑制**：收到远端条目写入本机剪贴板时，记录本机写入标记（macOS：`changeCount`；Windows：`GetClipboardSequenceNumber()` + 私有格式标记 `ShunSync.Origin`）与 5 秒指纹抑制窗口。
3. **LWW 并发决胜**：按 `(timestampMs, originDeviceId)` 字典序决胜是否覆盖系统当前剪贴板。
4. **隐私过滤**：跳过密码管理器隐藏条目（macOS `org.nspasteboard.ConcealedType` / `TransientType` 等；Windows `ExcludeClipboardContentFromMonitorProcessing` / `CanIncludeInClipboardHistory = 0`）。

### 4.3 剪贴板消息格式

- **文字（`0x01`）**：
  ```json
  {
    "type": "clip_text",
    "eventId": "uuid",
    "timestampMs": 1732060000000,
    "originDeviceId": "uuid",
    "originDeviceName": "公司台式机",
    "originPlatform": "windows",
    "fingerprint": "sha256-hex",
    "text": "hello",
    "targetDeviceId": null
  }
  ```
  `targetDeviceId` 非空时仅目标设备处理（用于定向发送）。

- **图片（`0x03`）**：
  `ClipImageMeta` JSON：
  ```json
  {
    "eventId": "uuid",
    "timestampMs": 1732060000000,
    "originDeviceId": "uuid",
    "originDeviceName": "MacBook Pro",
    "originPlatform": "macos",
    "fingerprint": "sha256-hex",
    "width": 1200,
    "height": 800,
    "byteSize": 52410,
    "targetDeviceId": null
  }
  ```
  紧跟原始 PNG 字节（≤ 15 MB）。

- **文件 / 文件夹清单（`0x01`）**：
  ```json
  {
    "type": "clip_files",
    "eventId": "uuid",
    "timestampMs": 1732060000000,
    "originDeviceId": "uuid",
    "originDeviceName": "公司台式机",
    "originPlatform": "windows",
    "fingerprint": "sha256-hex",
    "transferId": "uuid",
    "totalBytes": 10485760,
    "entries": [
      { "index": 0, "relativePath": "项目资料", "isDirectory": true, "size": 0, "modifiedMs": 1732059000000 },
      { "index": 1, "relativePath": "项目资料/方案.pdf", "isDirectory": false, "size": 10485760, "modifiedMs": 1732059000000 }
    ],
    "targetDeviceId": null
  }
  ```
  - `relativePath` 始终使用 POSIX `/` 分隔符，绝不含前导 `/`、`\`、盘符或 `..`。
  - 接收端默认仅记录清单（`autoDownloadFileLimitMB = 0`，手动按需拉取），避免日常本地整理文件产生无谓流量；若用户调大自动预取上限，则对不超过上限的文件后台通过 `mode="file"` 预取并写入剪贴板。

### 4.4 文件流拉取（`mode = "file"`，以及 NAT/二级路由下的 `mode = "sync"` 复用兜底）

优先使用独立 `mode="file"` 连接连续拉取一个批次内的多个文件，拉完发送 `bye` 关闭。

**单向路由 / NAT 兜底（重要，适配如 Wi-Fi `192.168.x` 连有线 `172.16.x` 的二级路由）**：
- 若接收端无法向发送端新开 `mode="file"` 连接（超时或被路由器/防火墙拒绝），但双方已存在一条活跃的 `mode="sync"` 连接，接收端可**直接在该 `mode="sync"` 连接上发送 `file_pull_req`**，发送端按完全相同的格式在同一连接上回传 `0x02` 分片与 `file_pull_done` / `file_pull_err`（按 `transferId` 区分，无需发 `bye` 断开控制通道）。

1. 接收端发送请求（支持断点续传 `offset`）：
   ```json
   { "type": "file_pull_req", "transferId": "uuid", "fileIndex": 1, "offset": 0 }
   ```
2. 发送端从 `offset` 读取文件，按 256 KB 发送若干 `0x02` 分片帧，同时计算本次发送字节片段 `[offset..EOF)` 的 SHA-256。
3. 发送完成后发控制帧：
   ```json
   { "type": "file_pull_done", "transferId": "uuid", "fileIndex": 1, "totalSent": 10485760, "sha256": "64-hex-chars" }
   ```
   若出错则发 `{ "type": "file_pull_err", "transferId": "...", "fileIndex": 1, "code": "not_found|invalid_offset|read_error", "message": "..." }`。
4. 走 `mode="sync"` 兜底时，分片和其他控制消息在同一条连接上按序交错，发送端应限制排队量（Win顺 每条连接排队不超过 4 MB），以免剪贴板消息被大文件堵住。
5. `transferId` 按清单里的原文回传；接收端按 `transferId` 匹配分片时不区分大小写（分片头里是 16 字节，还原成文本后是小写，而 Swift 的 `UUID().uuidString` 是大写）。

### 4.5 设备吊销与密钥轮换

移除某台设备时，操作端向其他仍在线的可信成员发送：
```json
{
  "type": "group_rotate",
  "newGroupKey": "<base64(32B)>",
  "revokedDeviceIds": ["removed-device-uuid"]
}
```
收到该消息且自身不在 `revokedDeviceIds` 中的设备立即持久化新密钥并重连新群组。
