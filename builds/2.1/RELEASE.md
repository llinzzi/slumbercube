# v2.1 — 按钮交互改进 + 字体升级

**发布日期**: 2026-05-20

## 新增

- **短按/长按分离**: 短按直接睡眠，长按触发 WiFi + 音频播放
- **Canvas 音频指示点**: 深夜模式下长按播放时，左上角显示 4×4 指示点
- **完整中文字体**: font_station 包含 3977 个字形（ark-pixel-10px，bpp=2）

## 变更

- 深夜模式恢复为 22:00–6:00（8×8 网格抖动）
- 闹钟唤醒默认 7:45
- 分区表切换为 3MB factory（`partitions_3m.csv`）
- 移除硬编码备用电台 URL（音频播放器不再自动 fallback）

## 修复

- 蓝牙音频播放 I2S 看门狗崩溃
- 深夜模式 WiFi+音频启动：异步处理，避免阻塞按钮任务
- 音频指示点线程安全：canvas 绘制替代 LVGL widget

## 烧录

```bash
esptool.py -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 builds/2.1/bootloader.bin \
  0x8000 builds/2.1/partition-table.bin \
  0x10000 builds/2.1/project-name.bin
```

> 固件版本: v2.0-20-g1d1436c | 目标: ESP32-C3 | 分区: 3MB factory (50% 空闲)
