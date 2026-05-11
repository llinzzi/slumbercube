# Release v1.0

## 烧录命令

```bash
esptool.py -p /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 2MB --flash_freq 80m \
  0x0 bootloader.bin \
  0x8000 partition-table.bin \
  0x10000 project-name.bin
```

## 改动摘要

- 使用外部 32.768kHz 晶振作为 RTC 时钟源，深度睡眠期间保持时间
- GPIO2 拉低，关闭 NS4168 音频功放以降低功耗
- 修复深度睡眠唤醒白闪（屏幕加载顺序）
- 移除 flush 回调调试日志
