# v2.0 — 网络电台音频播放

**发布日期**: 2026-05-18

## 新增

- **HTTP 网络电台播放**: 唤醒后自动播放网络音乐流，NS4168 I2S 功放输出
- **主备双 URL 自动切换**: 主 URL 失败时自动切换到备用 1.FM 80s/90s 电台
- **ICY 元数据解析**: 显示电台名和歌曲标题 (in-stream metadata)
- **启动状态栏**: 显示 "Connecting WiFi..." → "Fetching weather..." 等状态
- **定时器闹钟唤醒**: 支持配置自动唤醒时间（默认 7:50）
- **备用电台回退**: 硬编码 `https://strm112.1.fm/80s_90s_mobile_mp3` 作为兜底

## 变更

- 主电台流改为 Smooth Jazz Planet
- hostname 设为 `ssd1322`
- 默认音量降至 2%
- 夜间模式稳定在 22:00-6:00，4×4 棋盘格抖动
- 移除加载图片 (loading_img) 和天气日期行
- 配置入口统一到 `idf.py menuconfig`

## 修复

- 深度睡眠 GPIO 唤醒可靠性
- I2S 看门狗崩溃 + 添加 fallback URL
- 时钟重配置容错

## 烧录

```bash
esptool.py -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 builds/2.0/bootloader.bin \
  0x8000 builds/2.0/partition-table.bin \
  0x10000 builds/2.0/project-name.bin
```

> 固件版本: `v2.0-7-g5f1e78a-dirty` | 目标: ESP32-C3 | 分区: 1.5MB app (94% 已用)
