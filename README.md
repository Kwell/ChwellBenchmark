# ChwellBenchmark

基于 `ChwellCore` 的性能基准测试合集。

目前包含：

- **echo_qps_bench**：对 `ChwellCore` 的 `example_echo_server` 做大量长连接 Echo QPS 压测。

## 构建

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## 使用 echo_qps_bench

1. 在另一个终端中启动 `ChwellCore` 仓库里的 `example_echo_server`，例如：

```bash
cd /root/work/chwell/ChwellCore/build
./example_echo_server
```

2. 在本仓库的 `build` 目录运行压测（注意提前调大 `ulimit -n` 和内核网络参数，确保能建立 10w 连接）：

```bash
./echo_qps_bench \
  --host 127.0.0.1 \
  --port 9000 \
  --connections 100000 \
  --duration 30
```

参数说明：

- `--host`：Echo 服务器 IP，默认 `127.0.0.1`
- `--port`：Echo 服务器端口，默认 `9000`
- `--connections`：要建立的 TCP 长连接数，默认 `100000`
- `--duration`：压测时长（秒），默认 `30`

