# BLE Inspector 使用说明

`tools/ble-inspector` 是一个 macOS 原生 BLE（Bluetooth Low Energy）扫描与
GATT 检查工具，不需要安装 Python 包。它不会扫描经典蓝牙，也不会绕过设备的
配对、加密或授权机制。

## 快速开始

```bash
# 扫描 10 秒，只读取广播，不连接任何设备
./tools/ble-inspector

# 扫描更长时间
./tools/ble-inspector --seconds 20

# 连接自己拥有的目标设备，枚举服务和控制能力
./tools/ble-inspector --inspect blinkwh

# 读取目标设备所有声明为可读的特征
./tools/ble-inspector --inspect <扫描结果中的设备UUID> --read

# 检查所有声明为可连接的设备（可能触发配对提示，耗时也较长）
./tools/ble-inspector --inspect all
```

首次运行时，macOS 可能询问蓝牙权限。若扫描不到设备，请到：

`系统设置 → 隐私与安全性 → 蓝牙`

允许当前使用的 Terminal 或 Codex，然后重新运行。

## 如何理解结果

- `RSSI`：接收信号强度，通常越接近 0 信号越强；例如 `-45 dBm` 一般比
  `-85 dBm` 更近或信号更好，但它不是精确距离。
- `可连接`：可以尝试建立 BLE GATT 连接；`仅广播` 通常只能读取广播数据。
- `服务`：设备公开的一组功能。
- `特征`：具体数据或命令入口。
- `read`：可读取当前值。
- `write` / `write-no-response`：可能是控制入口，但具体字节协议必须查设备文档
  或固件，不能仅凭“可写”推断命令。
- `notify` / `indicate`：设备可以主动推送状态或传感器数据。
- 厂商数据和服务数据是原始十六进制；若厂商未公开格式，工具不会猜测其含义。

macOS 的 CoreBluetooth 会隐藏真实蓝牙 MAC 地址，扫描结果中的 UUID 是 macOS
为该设备分配的标识。因此，ESP32 配置中的
`0c:3d:5e:a6:63:c0` 不会直接出现在本工具的结果里。

## 控制已知的 blinkwh 灯

本项目固件已经定义了该灯的协议：

| 项目 | 值 |
|---|---|
| BLE 服务 | `FFE0` |
| LED 特征 | `FFE1` |
| 开灯 | 向 `FFE1` 写入 `01` |
| 关灯 | 向 `FFE1` 写入 `00` |

先扫描并确认名称/UUID，再执行：

```bash
./tools/ble-inspector --write <设备名或UUID> FFE1 01 --yes
./tools/ble-inspector --write <设备名或UUID> FFE1 00 --yes
```

`--yes` 是有意设计的写入确认。只对你拥有或明确获准测试的设备执行写入。

## 限制

- 工具只支持 BLE，不显示耳机、键盘等仅使用经典蓝牙的设备。
- 设备可能使用随机地址、轮换标识或不广播名称。
- 能看到“可写”并不等于知道控制协议；协议可能还需要认证、校验、分包或加密。
- `--read` 可能触发系统配对提示；读取受保护特征时也可能失败。
