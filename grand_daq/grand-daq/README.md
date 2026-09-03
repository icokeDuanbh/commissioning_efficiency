# `new-cs-daq`

`App -> Frontend -> ZmqComm -> DataHandler -> T3Trigger / Builder / DataStore`

## 当前唯一主路径

- 配置主路径只有 `cfgs/sysconfig.yaml`。
- `App::start()` 是启动入口，按 `STOP -> TERM -> INIT -> CONF -> STAR` 下发控制。
- `CONF` payload 由 `LegacyConfigCompiler` 基于 `cfgs/DU-address-map.yaml` 和 `cfgs/DU-readable-conf.yaml` 生成。
- `MT_T2` 进入 `T3Trigger`，命中后由 `App` 回发 `DOTRIGGER` 并通知 `Builder` 开始事件。
- `MT_DAQEVENT` 进入 `Builder`，完成关联后写入 CD `.dat`。
- `MT_RAWEVENT` 直接写入 L1 `.dat`。

## 运行入口

- 进程入口：`src/daq_main.cpp`
- HTTP 控制面：`src/http_api.cpp`
- 脚本入口：`scripts/cmd_start.sh`、`scripts/cmd_stop.sh`

常用生命周期命令：

- `POST /api/cmd/initialize`
- `POST /api/cmd/start`
- `POST /api/cmd/stop`
- `POST /api/cmd/terminate`

## 本地构建

推荐使用项目脚本：

```bash
cd /data/home/grand/gumh/grand-cs-daq/new-cs-daq-v1.0/grand-daq
./build.sh
```

等价手动构建：

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

## 常用运行命令

进入服务目录：

```bash
cd /data/home/grand/gumh/grand-cs-daq/new-cs-daq-v1.0/grand-daq
```

用 `tmux` 后台启动 DAQ 进程：

```bash
tmux new-session -d -s daq -c /data/home/grand/gumh/grand-cs-daq/new-cs-daq-v1.0/grand-daq './scripts/run_with_diag.sh cfgs/sysconfig.yaml'
```

启动采集流程：

```bash
curl -X POST http://localhost:8080/api/cmd/initialize -d '{}'
curl -X POST http://localhost:8080/api/cmd/start -d '{}'
```

查看 `tmux` 会话：

```bash
tmux attach -t daq
```

停止 DAQ：

```bash
./scripts/cmd_stop.sh
tmux kill-session -t daq
```

运行日志输出到：

```bash
logs/
```

如果需要把落盘输出指向临时目录，可设置（L1/MD 与 CD 各自一个目录）：

```bash
export CSDAQ_L1_STORE_PATH=/tmp/csdaq/l1
export CSDAQ_L3_STORE_PATH=/tmp/csdaq/l3
```
