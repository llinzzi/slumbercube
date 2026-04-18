# SSD1322 OLED 驱动测试程序

这是一个基于ESP-IDF 5.5框架的SSD1322灰度OLED驱动程序，支持256x64分辨率显示。

## 硬件连接

| 引脚 | GPIO | 功能 |
|------|------|------|
| SCL  | IO3  |  SPI时钟 |7
| SDA  | IO12 | SPI数据(MOSI) | 10
| RST  | IO10 | 复位 | null
| CS   | IO0  | 片选 | GND
| DC   | IO1  | 数据/命令选择 | 8

## 功能特性

- 支持SSD1322 256x64灰度OLED显示屏
- 使用ESP-IDF的SPI驱动
- 支持文本显示（9x15字体）
- 支持单色和灰度位图显示
- 支持清屏、窗口设置等基本操作

## 编译和烧录

```bash
# 设置目标芯片（根据实际情况选择）
idf.py set-target esp32c3

# 配置项目
idf.py menuconfig

# 编译
idf.py build

# 烧录
idf.py -p /dev/ttyUSB0 flash

# 查看日志
idf.py -p /dev/ttyUSB0 monitor
```

## API说明

### 初始化
```c
esp_err_t ssd1322_init(void);
```

### 显示控制
```c
void ssd1322_clear(void);              // 清除屏幕（上半部分）
void ssd1322_clear_all(void);          // 清除整个屏幕
void ssd1322_set_window(uint8_t x_start, uint8_t y_start, uint8_t x_end, uint8_t y_end);
```

### 文本显示
```c
void ssd1322_char(uint8_t x, uint8_t y, char ch, uint8_t mode);
void ssd1322_string(uint8_t x, uint8_t y, const char *str, uint8_t mode);
```
- mode: 0=正常显示，1=反色显示

### 位图显示
```c
void ssd1322_bitmap_mono(const uint8_t *bitmap);   // 单色位图
void ssd1322_bitmap_gray(const uint8_t *bitmap);   // 灰度位图
```

## 文件结构

```
main/
├── ssd1322.h         # 驱动头文件
├── ssd1322.c         # 驱动实现
├── font_9x15.h       # 字体头文件
├── font_9x15.c       # 字体数据
├── main.c            # 主程序
└── CMakeLists.txt    # 构建配置
```

## 示例代码

```c
#include "ssd1322.h"

void app_main(void)
{
    // 初始化显示屏
    ssd1322_init();
    
    // 清除屏幕
    ssd1322_clear_all();
    
    // 显示文本
    ssd1322_string(0, 0, "Hello World!", 0);
    ssd1322_string(0, 16, "ESP-IDF 5.5", 0);
}
```

## 注意事项

1. 确保ESP-IDF环境已正确安装（版本5.5）
2. 根据实际使用的芯片型号设置正确的target
3. 如需修改GPIO引脚，请在ssd1322.h中修改相应定义
4. SPI时钟频率默认为10MHz，可根据实际情况调整

---

## Arduino参考代码

#include "as_oled1322.h"
#include <pgmspace.h>
#include "font_9x15.c" // 确保这个文件存在并包含正确的字体数据

// 命令和数据发送函数
void command(uint8_t cmd) {
  digitalWrite(OLED_DC, LOW);
  digitalWrite(OLED_CS, LOW);
  SPI.transfer(cmd);
  digitalWrite(OLED_CS, HIGH);
}

void data(uint8_t dat) {
  digitalWrite(OLED_DC, HIGH);
  digitalWrite(OLED_CS, LOW);
  SPI.transfer(dat);
  digitalWrite(OLED_CS, HIGH);
}

// 初始化函数
void as_oled_begin() {
  pinMode(OLED_RES, OUTPUT);
  pinMode(OLED_DC, OUTPUT);
  pinMode(OLED_CS, OUTPUT);
  
  // 初始化SPI
  SPI.begin(OLED_SCK, -1, OLED_MOSI, OLED_CS);
  SPI.setFrequency(SPI_Clock); // 使用定义的SPI时钟频率
  
  digitalWrite(OLED_CS, LOW);
  digitalWrite(OLED_RES, HIGH);
  delay(10);
  digitalWrite(OLED_RES, LOW);
  delay(10);
  digitalWrite(OLED_RES, HIGH);
 
  command(0xFD); /*SET COMMAND LOCK*/ 
  data(0x12); /* UNLOCK */ 
  command(0xAE); /*DISPLAY OFF*/ 
  command(0xB3);/*DISPLAYDIVIDE CLOCKRADIO/OSCILLATAR FREQUANCY*/ 
  data(0x91); 
  command(0xCA); /*multiplex ratio*/ 
  data(0x3F); /*duty = 1/64*/ 
  command(0xA2); /*set offset*/ 
  data(0x00);
  command(0xA1); /*start line*/ 
  data(0x00); 
  command(0xA0); /*set remap*/
  data(0x14); 
  data(0x11);
  	
  command(0xAB); /*funtion selection*/ 
  data(0x01); /* selection external vdd */ 
  command(0xB4); /* */ 
  data(0xA0);
  data(0xfd); 
  command(0xC1); /*set contrast current */ 
  data(0x80); 
  command(0xC7); /*master contrast current control*/ 
  data(0x0f); 
  	
  command(0xB1); /*SET PHASE LENGTH*/
  data(0xE2); 
  command(0xD1); /**/
  data(0x82); 
  data(0x20); 
  command(0xBB); /*SET PRE-CHANGE VOLTAGE*/ 
  data(0x1F);
  command(0xB6); /*SET SECOND PRE-CHARGE PERIOD*/
  data(0x08); 
  command(0xBE); /* SET VCOMH */ 
  data(0x07); 
  command(0xA6); /*normal display*/ 
  command(0xAF); /*display ON*/  
}

// 设置显示窗口
void as_oled_SetWindow(uint8_t Xstart, uint8_t Ystart, uint8_t Xend, uint8_t Yend) { 
  command(0x15);
  data(Xstart + 0x1c);
  data(Xend + 0x1c);
  command(0x75);
  data(Ystart);
  data(Yend);
  command(0x5c); // write ram command
}

// 清除屏幕
void as_oled_clear() {
  as_oled_SetWindow(0, 0, 63, 63);
  as_start_data();
  for(int i = 0; i < 8192; i++) {
    as_data(0x00);      
  }        
  as_end_data();
}

// 完全清除屏幕
void as_oled_clear_all() {
  as_oled_SetWindow(0, 0, 63, 127);
  as_start_data();
  for(int i = 0; i < 16384; i++) {
    as_data(0x00);
  }        
  as_end_data();
}

// 数据处理函数
void as_data_processing(uint8_t temp) {
  as_data(((temp & 0x80) ? 0xf0 : 0x00) | ((temp & 0x40) ? 0x0f : 0x00)); // Pixel1,Pixel2
  as_data(((temp & 0x20) ? 0xf0 : 0x00) | ((temp & 0x10) ? 0x0f : 0x00)); // Pixel3,Pixel4
  as_data(((temp & 0x08) ? 0xf0 : 0x00) | ((temp & 0x04) ? 0x0f : 0x00)); // Pixel5,Pixel6
  as_data(((temp & 0x02) ? 0xf0 : 0x00) | ((temp & 0x01) ? 0x0f : 0x00)); // Pixel7,Pixel8
}

// 显示单个字符
void as_oled_char(uint8_t x, uint8_t y, const char ch, uint8_t mode) {
  x = x / 4;
  int OffSet = (ch - 32) * 15 + 4;
  as_oled_SetWindow(x, y, x + 1, y + 14);
  as_start_data();
  for (int i = 0; i < 15; i++) {
    uint8_t str = pgm_read_byte(&font9x15[OffSet + i]);  
    if(mode) str = ~str;
    as_data_processing(str);             
  }
  as_end_data();
}

// 显示字符串
void as_oled_string(uint8_t x, uint8_t y, const char *pString, uint8_t Mode) {     
  while (*pString != 0) {
    as_oled_char(x, y, *pString, Mode);
    x += 8; // 字体宽度
    pString++;              
  }
}

// 显示单色位图
void as_oled_bitmap_mono(const uint8_t *pBuf) {
  as_oled_SetWindow(0, 0, 255 / 4, 63);
  as_start_data();
  for (int row = 0; row < 64; row++) {              
    for(int col = 0; col < 256 / 8; col++) {
      uint8_t dat = pgm_read_byte(pBuf);
      pBuf++;  
      as_data_processing(dat);               
    }
  }    	
  as_end_data();
}

// 显示灰度位图
void as_oled_bitmap_gray(const uint8_t *pBuf) {
  as_oled_SetWindow(0, 0, 255 / 4, 63);
  as_start_data();
  for (int row = 0; row < 64; row++) {              
    for(int col = 0; col < 128; col++) {
      as_data(pgm_read_byte(pBuf));
      pBuf++;
    }
  }      
  as_end_data();
}
