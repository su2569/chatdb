# ChatDB v2.0 - C++ 智能群聊数据库

专为多群 QQ 机器人设计的聊天记录存储、语义检索与分层记忆系统。

## 核心特性

- **SQLite 主存储**：零配置、WAL 模式、FTS5 全文搜索，磁盘存储零额外内存
- **Redis 向量索引**：复用现有 Redis 实例，HNSW 近似最近邻搜索
- **多 Embedding 源**：Ollama(本地) / OpenAI / 阿里云，支持热切换
- **分层记忆总结**：3h→12h→24h→月→年→3年，自动归档重要/日常/撤回记忆
- **TCP JSON-RPC API**：前端控制、AstrBot 插件接入
- **WebSocket 客户端**：连接 go-cqhttp / AstrBot 接收 QQ 消息
- **进程守护**：探查 Python/Node 进程，防止大模型自毁
- **智能端口检测**：默认端口→进程扫描→交互询问，三档降级
- **跨平台**：Windows / Linux 双适配
- **低配置优化**：批量写入、异步 Embedding、滑动窗口、INT8 量化

## 系统要求

| 组件 | 最低 | 推荐 |
|------|------|------|
| CPU | x64 任意 | 4核+ |
| 内存 | 2 GB | 4 GB+ |
| 磁盘 | 100 MB | 1 GB+ |
| 系统 | Win10 / Linux 4.0+ | 最新稳定版 |

## 依赖安装

### Linux (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    libsqlite3-dev libhiredis-dev

# Ollama
curl -fsSL https://ollama.com/install.sh | sh
ollama pull nomic-embed-text

# Redis（复用云崽的即可，无需重复安装）
# 确保 Redis 已加载 RedisSearch 模块（Redis Stack 默认包含）
```

### Windows

1. **Visual Studio 2022**（带 C++ 桌面开发工作负载）
2. **CMake** 3.16+
3. **vcpkg**：
   ```powershell
   git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install sqlite3 hiredis --triplet x64-windows
   ```
4. **Redis for Windows**：https://github.com/tporadowski/redis/releases
5. **Ollama Windows**：https://ollama.com/download/windows
   ```powershell
   ollama pull nomic-embed-text
   ```

## 构建

### Linux

```bash
cd chatdb
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Windows (VS2022)

```powershell
cd chatdb
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --parallel
```

### Windows (MinGW/MSYS2)

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-sqlite3 mingw-w64-ucrt-x86_64-hiredis
cd chatdb
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
mingw32-make -j$(nproc)
```

## 运行

```bash
# 测试所有连接
./chatdb -t

# 运行演示（模拟消息 + 查询 + 记忆）
./chatdb -d

# 使用自定义配置
./chatdb -c mybot.conf

# 查看统计
./chatdb -s

# 清理 7 天前的数据
./chatdb --cleanup 7

# 手动触发总结
./chatdb --summarize 10001 24h

# 切换 Embedding Provider
./chatdb --switch-provider openai

# 备份向量索引
./chatdb --backup

# 压缩数据库
./chatdb -v
```

## 配置文件 chatdb.conf

```ini
# SQLite
sqlite_path=chatdb.sqlite

# Redis（复用云崽）
redis_host=127.0.0.1
redis_port=6379
redis_password=
redis_db=0

# Ollama（本地，默认 Provider）
ollama_host=127.0.0.1
ollama_port=11434
ollama_model=nomic-embed-text
ollama_timeout_ms=300000      # 300秒，本地模型可能较慢
ollama_embedding_dim=768

# OpenAI（可选，留空禁用）
openai_api_base=https://api.openai.com/v1
openai_api_key=sk-xxxxxxxx
openai_model=text-embedding-3-small

# 阿里云（可选）
aliyun_api_base=https://dashscope.aliyuncs.com/api/v1
aliyun_api_key=sk-xxxxxxxx
aliyun_model=text-embedding-v2

# 向量策略
vector_retention_days=7       # 滑动窗口，控制 Redis 内存
use_int8_quantization=false   # Redis 8+ 支持，省 75% 内存

# TCP Server（前端 / AstrBot）
tcp_port=17320
tcp_max_clients=32

# WebSocket Client（QQ）
ws_host=127.0.0.1
ws_port=3001
ws_path=/
ws_access_token=              # go-cqhttp / AstrBot 的 access token

# 进程守护
guard_processes=python,node   # 监视的进程名
guard_check_interval_ms=10000 # 检查间隔

# 性能
worker_threads=2
sqlite_batch_size=100
```

## 架构

```
                    ┌─────────────┐
                    │   前端 UI    │
                    │  / AstrBot   │
                    └──────┬──────┘
                           │ TCP JSON-RPC :17320
                           ▼
┌─────────────────────────────────────────────────────────────┐
│                      ChatDB Core (C++)                       │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ TCP Server  │  │  WS Client  │  │  Process Guard      │  │
│  │ JSON-RPC    │  │  QQ 消息    │  │  Python/Node 探查   │  │
│  └──────┬──────┘  └──────┬──────┘  └─────────────────────┘  │
│         │                │                                   │
│         ▼                ▼                                   │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              MessageProcessor                        │    │
│  │  去重 → SQLite 批量写入 → 异步 Embedding → Redis    │    │
│  └─────────────────────────────────────────────────────┘    │
│         │                │                │                  │
│         ▼                ▼                ▼                  │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────────┐    │
│  │ SQLite(磁盘) │ │ Redis(内存)  │ │ MemorySummarizer │    │
│  │ • messages   │ │ • vec:*      │ │ • 3h/12h/24h     │    │
│  │ • msg_fts    │ │ • dup:*      │ │ • month/year/3y  │    │
│  │ • memories   │ │ • state:*    │ │ • 主动聊天       │    │
│  │ • index_back │ │ • HNSW索引   │ │ • 重要/日常/撤回 │    │
│  └──────────────┘ └──────────────┘ └──────────────────┘    │
│         │                │                │                  │
│         ▼                ▼                ▼                  │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              EmbeddingProviderManager                │    │
│  │  Ollama(本地) / OpenAI / 阿里云 ← 热切换            │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

## TCP JSON-RPC API（前端 / AstrBot 接入）

### 消息接口

```json
// 接收单条消息
{"id":"1","method":"msg.receive","params":{"group_id":123456,"qq_id":10001,"nickname":"Alice","content":"大家好","msg_type":1}}

// 批量接收
{"id":"2","method":"msg.batch","params":{"messages":[{"group_id":123,"qq_id":1,"content":"hi"}]}}

// 处理撤回
{"id":"3","method":"msg.recall","params":{"group_id":123,"msg_id":456,"important":false}}
```

### 搜索接口

```json
// 全文搜索
{"id":"4","method":"search.fulltext","params":{"query":"显卡","group_id":123,"limit":10}}

// 语义搜索
{"id":"5","method":"search.semantic","params":{"query":"电脑硬件","group_id":123,"limit":10}}

// 混合搜索（默认）
{"id":"6","method":"search.hybrid","params":{"query":"键盘推荐","group_id":123,"limit":10}}

// 按时间搜索
{"id":"7","method":"search.time","params":{"group_id":123,"start":1690000000,"end":1690100000}}

// 按标记搜索
{"id":"8","method":"search.marked","params":{"group_id":123,"tags":["important"]}}

// 引用搜索（上下文）
{"id":"9","method":"search.ref","params":{"msg_id":456,"radius":5}}
```

### 记忆接口

```json
// 获取记忆列表
{"id":"10","method":"mem.list","params":{"group_id":123,"level":"24h","limit":50}}

// 设置重要性
{"id":"11","method":"mem.set_importance","params":{"msg_id":456,"score":0.9,"reason":"重要通知"}}

// 手动触发总结
{"id":"12","method":"mem.summarize","params":{"group_id":123,"level":"24h"}}

// 合并记忆
{"id":"13","method":"mem.merge","params":{"mem_ids":[1,2,3],"new_summary":"合并后的总结"}}

// 删除记忆
{"id":"14","method":"mem.delete","params":{"mem_id":1}}
```

### Provider 接口

```json
// 列出所有 Provider
{"id":"15","method":"provider.list","params":{}}

// 切换 Provider（触发索引重建）
{"id":"16","method":"provider.switch","params":{"name":"openai"}}

// 查看当前 Provider 状态
{"id":"17","method":"provider.status","params":{}}
```

### 系统接口

```json
// 统计
{"id":"18","method":"sys.stats","params":{}}

// 健康检查
{"id":"19","method":"sys.health","params":{}}

// 备份索引
{"id":"20","method":"sys.backup","params":{}}

// 恢复索引
{"id":"21","method":"sys.restore","params":{"file":"backup.json"}}

// 清理旧数据
{"id":"22","method":"sys.cleanup","params":{"days":7}}
```

### 事件推送（Server → Client）

ChatDB 会主动向所有 TCP 连接推送事件：

```json
// 新消息
{"event":"evt.msg_new","data":{"group_id":123,"qq_id":1,"content":"hi"},"ts":1690000000}

// 记忆总结完成
{"event":"evt.mem_summary","data":{"group_id":123,"level":"24h","summary":"今日总结..."},"ts":1690000000}

// 主动聊天触发
{"event":"evt.mem_active","data":{"group_id":123,"topic":"今日热点","suggested":"大家怎么看？"},"ts":1690000000}

// Provider 切换
{"event":"evt.provider_changed","data":{"old":"ollama","new":"openai"},"ts":1690000000}

// 系统告警
{"event":"evt.sys_alert","data":{"level":"warn","message":"Redis memory > 80%"},"ts":1690000000}
```

## 分层记忆机制

| 层级 | 周期 | 内容 | 保留策略 |
|------|------|------|---------|
| **3小时** | 每3h | 模糊零碎记忆，仅保留统计和关键词 | 自动，低重要性 |
| **12小时** | 每12h | 基于3h总结，进一步模糊不重要内容 | 自动，中等重要性 |
| **24小时** | 每天 | **特别详细**的一天总结，保留完整上下文 | 自动，高重要性 |
| **1月** | 每月 | 汇总每日详细记录，提取月度关键事件 | 自动，重要 |
| **1年** | 每年 | 汇总月度记录，年度大事件回顾 | 自动，非常重要 |
| **3年** | 每3年 | 汇总年度记录，长期趋势和关系变化 | 永久保留，最重要 |

### 记忆分类

- **important**：被 AI 或管理员标记为重要，或包含关键词（通知、公告、约定等）
- **daily**：普通日常聊天
- **recalled**：被撤回的消息（如为重要撤回则升级为 important）

### 主动聊天

AI 在整理记忆时，如果检测到：
- 群内有新热点话题
- 某个话题讨论度突然上升
- 有人提出开放性问题但无人回答

可能触发 `evt.mem_active` 事件，建议 AstrBot 发送一条参与性消息。

## WebSocket 连接 QQ

ChatDB 作为 WS **客户端**连接 go-cqhttp / AstrBot 的 WS 服务：

```ini
# go-cqhttp config.yml
servers:
  - ws:
      host: 0.0.0.0
      port: 3001
      access_token: your_token
```

ChatDB 会自动处理：
- 消息接收（文本、图片、表情、@、回复）
- 撤回检测（区分普通撤回和重要撤回）
- 断线重连（指数退避）
- 心跳保活

## 进程守护

ChatDB 会定期扫描系统进程，监视 Python 和 Node 进程：

```
[ChatDB] Process python (PID 1234) is UP
[ChatDB] Process node (PID 5678) is UP
[ChatDB] Process python (PID 1234) is DOWN!  <-- 告警
```

检测到进程消失时：
1. 记录日志告警
2. 推送 `evt.sys_alert` 到所有 TCP 客户端
3. 等待进程恢复（不自动重启，避免误杀）

## 端口检测策略

1. **默认端口**：Redis 6379、Ollama 11434、WS 3001
2. **进程扫描**：`ss -tlnp` / `netstat -ano` 查找进程
3. **交互式询问**：扫描失败时提示用户输入

## 内存优化建议

低配置机器（4GB）：

```ini
vector_retention_days=3       # 缩短向量窗口
worker_threads=1              # 减少线程
sqlite_batch_size=50          # 减小批量
use_int8_quantization=true    # Redis 8+ 启用
```

Redis 额外优化：
- 关闭 AOF：`appendonly no`（向量可重建）
- 定期执行 `./chatdb --cleanup 3`

## 环境变量

| 变量 | 说明 |
|------|------|
| `CHATDB_SQLITE_PATH` | 数据库路径 |
| `CHATDB_REDIS_HOST/PORT` | Redis 地址 |
| `CHATDB_OLLAMA_HOST/PORT/MODEL` | Ollama 配置 |
| `CHATDB_OPENAI_KEY` | OpenAI API Key |
| `CHATDB_ALIYUN_KEY` | 阿里云 API Key |
| `CHATDB_TCP_PORT` | TCP 服务端口 |
| `CHATDB_WS_HOST/PORT` | WS 连接地址 |
| `CHATDB_VECTOR_DAYS` | 向量保留天数 |
| `CHATDB_WORKER_THREADS` | 工作线程数 |

## 常见问题

**Q: RedisSearch 模块未加载？**
A: 向量搜索需要 RedisSearch。检查：`redis-cli INFO modules`。如缺失，换用 Redis Stack 或手动加载 `redisearch.so`。

**Q: Ollama 连接超时？**
A: 首次加载模型较慢，默认超时 300 秒。如仍超时，检查 `ollama serve` 是否运行，`ollama pull nomic-embed-text` 是否完成。

**Q: WS 连接不上 QQ？**
A: 检查 go-cqhttp / AstrBot 的 WS 服务是否开启，access_token 是否匹配。

**Q: 如何接入 AstrBot？**
A: AstrBot 通过 TCP 连接到 ChatDB 的 `tcp_port`（默认 17320），使用 JSON-RPC 协议调用 API。也可让 ChatDB 的 WS Client 连接到 AstrBot 的 WS 服务。

**Q: Provider 切换后搜索失效？**
A: 切换 Provider 会触发索引重建广播。需要等待重建完成（取决于消息量和 Provider 速度），期间搜索会降级为 FTS5 全文搜索。

**Q: Windows 编译报错找不到 hiredis？**
A: 确保 vcpkg 已集成：`cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`

## License

MIT License
