# LSQUIC Speed Test Docker

基于 LSQUIC 的 QUIC 协议速度测试工具，使用 UDP 端口 9331。

## 构建镜像

```bash
cd deps/lsquic
docker build -t lsquic-speed-test -f Dockerfile.speedtest .
```

## 使用方法（使用 host 网络获得最佳性能）

### 服务器端

```bash
# 启动服务器，监听 9331 端口
docker run -d --name speed-server \
    --network host \
    lsquic-speed-test server

# 查看日志
docker logs -f speed-server
```

### 客户端

```bash
# 发送 1GB 数据测试
docker run --rm --network host \
    lsquic-speed-test client \
    -s <服务器IP>:9331 -b 1G

# 发送 500MB 数据测试
docker run --rm --network host \
    lsquic-speed-test client \
    -s 192.168.1.100:9331 -b 500M

# 发送 2GB 数据测试
docker run --rm --network host \
    lsquic-speed-test client \
    -s 10.0.0.1:9331 -b 2G
```

## 跨服务器测试示例

### 服务器 A (192.168.1.100)

```bash
docker run -d --name speed-server \
    --network host \
    lsquic-speed-test server
```

### 服务器 B (192.168.1.101)

```bash
# 测试到服务器 A 的速度
docker run --rm --network host \
    lsquic-speed-test client \
    -s 192.168.1.100:9331 -b 1G
```

## 输出示例

```
Starting LSQUIC Speed Test Client...
[NOTICE] Will send 1024.00 MB of random data
[NOTICE] New connection established
[NOTICE] Starting speed test: sending 1024.00 MB
[NOTICE] Progress: 100 / 1024 MB (9.8%)
[NOTICE] Progress: 200 / 1024 MB (19.5%)
...
[NOTICE] CLIENT: Sent 1024.00 MB in 2.456 seconds = 3500.00 Mbps
[NOTICE] SERVER: RESULT: bytes=1073741824 time=2.456s speed=3498.50Mbps
```

## 参数说明

### 客户端参数

| 参数 | 说明 | 示例 |
|------|------|------|
| `-s HOST:PORT` | 服务器地址 | `-s 192.168.1.100:9331` |
| `-b BYTES` | 发送字节数 | `-b 1G`, `-b 500M`, `-b 100K` |

### 大小后缀

- `K` - KB (1024 字节)
- `M` - MB (1024 KB)
- `G` - GB (1024 MB)

## 注意事项

1. 确保防火墙允许 UDP 9331 端口
2. 使用 `--network host` 可获得更好的性能
3. 测试大数据量时请确保网络稳定
