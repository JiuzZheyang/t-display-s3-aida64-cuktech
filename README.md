# T-Display-S3 AIDA64 + CUKTECH 充电头 BLE 监视器

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.4-blue?style=flat-square&logo=espressif)](https://github.com/espressif/esp-idf)
[![LVGL](https://img.shields.io/badge/LVGL-v9-orange?style=flat-square)](https://lvgl.io)
[![Platform](https://img.shields.io/badge/Platform-LilyGO_T--Display--S3-red?style=flat-square)](https://www.lilygo.cc/products/t-display-s3)
[![License](https://img.shields.io/badge/License-MIT-yellow?style=flat-square)](LICENSE)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen?style=flat-square)]()

> **LilyGO T-Display-S3** (ESP32-S3, 320×170 LCD) 双功能监视器，同时支持 AIDA64 电脑硬件监控与 CUKTECH 酷态科充电头 BLE 实时监控。

---

## 📖 目录

- [功能概览](#-功能概览)
- [硬件要求](#-硬件要求)
- [快速开始](#-快速开始)
- [功能说明](#-功能说明)
  - [AIDA64 电脑监控](#-aida64-电脑监控)
  - [充电头监控](#-充电头监控)
  - [智能页面切换](#-智能页面切换)
  - [Web 仪表盘](#-web-仪表盘)
  - [巴法云智能家居](#-巴法云智能家居)
- [使用方法](#-使用方法)
  - [按键操作](#-按键操作)
  - [首次配置](#-首次配置)
  - [Web 仪表盘](#-web-仪表盘-1)
  - [巴法云 + 小爱同学设置](#-巴法云--小爱同学设置)
- [API 接口](#-api-接口)
- [项目结构](#-项目结构)
- [许可证](#-许可证)

---

## ✨ 功能概览

| 功能 | 描述 |
|------|------|
| 🖥️ **AIDA64 监控** | 实时显示 CPU/GPU 温度、使用率、功率和内存使用率 |
| ⚡ **充电头监控** | 蓝牙 5.0 实时监控 CUKTECH 酷态科充电头各端口 |
| 🔄 **智能页面切换** | 有数据自动切到充电页，无数据 3 秒回 AIDA64 |
| 🌐 **Web 仪表盘** | 内置 HTTP 服务器，手机/电脑实时查看和控制 |
| 🏠 **巴法云接入** | 小爱同学 / 小度音箱语音控制充电头端口开关 |
| 🎨 **精美 UI** | 完美复刻酷态科充电器屏幕显示（横屏） |

---

## 🛠 硬件要求

| 组件 | 规格 |
|------|------|
| **主板** | LilyGO T-Display-S3 (ESP32-S3) |
| **屏幕** | ST7789, 320×170, RGB565 |
| **闪存** | 4MB (自定义分区: 3MB factory) |
| **PSRAM** | 已禁用 |
| **USB** | USB-C |

### 支持的充电头

- **CUKTECH 酷态科 10 号超级电能充 Ultra**

---

## 🚀 快速开始

### 环境准备

```bash
# 安装 ESP-IDF v5.5.4
# 参考: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/
```

### 编译

```bash
git clone https://github.com/JiuzZheyang/t-display-s3-aida64-cuktech-cuktech.git
cd t-display-s3-aida64-cuktech
idf.py set-target esp32s3
idf.py build
```

### 烧录

```bash
# 1. 按住 BOOT 键
# 2. 按一下 RESET 键
# 3. 松开 BOOT 键（进入下载模式）
idf.py -p COM10 flash monitor
```

### 在线烧录（网页）

无需安装任何软件，直接用浏览器烧录固件：

```
1. 按住 BOOT 键 → 按 RESET 键 → 松开 BOOT（进入下载模式）
2. 打开 Chrome/Edge 浏览器，访问：https://web.espressif.com
3. 点击「连接」→ 选择对应的 COM 端口
4. 在「编程」页面，选择 firmware 目录下的 .bin 文件，起始地址对应填写：
   - 0x0        → bootloader.bin
   - 0x10000    → t_display_s3_xxx.bin
   - 0x8000     → partition-table.bin
5. 点击「开始编程」，等待完成自动重启
```

### 预编译固件烧录

无需编译环境，直接用 esptool 烧录 `firmware/` 目录下的预编译固件：

```bash
# 1. 按住 BOOT → 按 RESET → 松 BOOT（进入下载模式）
# 2. 运行以下命令：
python -m esptool --chip esp32s3 -p COM10 write_flash \
  0x0 firmware/bootloader.bin \
  0x10000 firmware/t_display_s3_aida64.bin \
  0x8000 firmware/partition-table.bin
```

---

## 📦 功能说明

### 🖥️ AIDA64 电脑监控

实时显示 CPU/GPU 温度、使用率、功率和内存使用率，通过 AIDA64 网络 API 自动刷新。

#### 支持的传感器

| 类别 | 传感器 |
|------|--------|
| CPU | 温度、使用率、功率、频率 |
| GPU | 温度、使用率、功率、频率 |
| 内存 | 使用率、已用/总量 |
| 主板 | 温度 |
| 硬盘 | 温度 |
| 网络 | 上传/下载速率 |
（部分需要自行修改代码）

#### AIDA64 配置步骤

**步骤 1：安装 AIDA64**

下载安装 [AIDA64 Extreme](https://www.aida64.com/downloads) 或 Engineer 版本。

**步骤 2：开启远程监控**

打开 AIDA64 → **文件** → **设置** → **LCD / 外部显示器**

- 勾选 **启用 LCD / 外部显示器支持**
- 在 **远程 LCD 监控** 中设置端口号（默认 `7789`）
- 点击 **确定** 保存

**步骤 3：配置设备**

方式一（推荐）：设备首次启动后连接 WiFi `AIDA64-Setup` → 浏览器打开 `192.168.4.1` → 输入 AIDA64 电脑 IP 和端口

方式二：在 Web 仪表盘 (`http://<设备IP>/config`) 中直接修改

**步骤 4：验证**

- 设备屏幕显示电脑硬件数据
- 如无数据，检查防火墙是否放行 AIDA64 端口
- 确保电脑和设备在同一局域网

#### 高级显示设置 (SIV)

SIV (Sensor Item Value) 是 AIDA64 远程监控协议中每个传感器项的编号索引。

**查找 SIV 索引：**

1. AIDA64 → **文件** → **设置** → **LCD / 外部显示器**
2. 点击 **SensorItem 选择**
3. 列表中的**第一个为 SIV1、第二个为 SIV2**，以此类推
4. 记录需要显示的传感器对应的 SIV 编号

**在 Web 仪表盘中配置：**

打开 `http://<设备IP>/config` → **高级显示设置 (SIV)**：

| 字段 | 说明 |
|------|------|
| **行数** | 1-4 行，每行显示 CPU/GPU 两个值 |
| **列标题** | 如 "CPU" / "GPU" |
| **标签** | 每行左侧的说明文字（如 "使用率"、"温度"） |
| **SIV0 / SIV1** | 对应 CPU 列和 GPU 列的 SIV 编号 |
| **内存 SIV** | 设置内存使用率和已用内存的 SIV 索引 |

**默认配置：**

```
行数: 4          列标题: CPU | GPU
第1行: 使用率    | SIV1 | SIV2
第2行: 温度      | SIV3 | SIV5
第3行: 功率      | SIV3 | SIV7
第4行: 频率      | SIV4 | SIV8
内存: 使用率 SIV9, 已用 SIV10
```

### ⚡ 充电头监控

基于 [kairui1108/cuktech-ble-ha](https://github.com/kairui1108/cuktech-ble-ha) 的 BLE 协议实现，蓝牙 5.0 直连 CUKTECH 酷态科 10 号超级电能充 Ultra。

**监控数据：**

| 端口 | 电压 | 电流 | 功率 |
|------|------|------|------|
| C1 | ✅ | ✅ | ✅ |
| C2 | ✅ | ✅ | ✅ |
| C3 | ✅ | ✅ | ✅ |
| USB-A | ✅ | ✅ | ✅ |

**界面布局：**

- **6 格布局**：总功率（大字体）+ C1/C2/C3/A 四端口
- **端口颜色**：每个端口独立颜色主题（橙/蓝/青/黄）
- **数字动画**：功率变化时数字滚动动画
- **端口禁用**：禁用时显示灰色背景、`--` 数据
- **融合检测**：C3 + USB-A 合并时自动合并显示

### 🔄 智能页面切换

```
AIDA64 页面 ──── 检测到充电数据 ────▶ 充电页面
                                    │
    ◀── 数据全无，等待 3 秒 ───────┘
    ◀── 手动按 BOOT 键切换 ────────┘
```

- 充电口有数据时**自动切换到充电页面**
- 所有端口数据为零时**等待 3 秒后自动返回**
- 手动按键进入充电页面后**不会自动返回**

### 🌐 Web 仪表盘

设备内置 HTTP 服务器（端口 80），无需额外安装：

| 页面 | 地址 | 功能 |
|------|------|------|
| **主页** | `http://<设备IP>/` | 实时充电数据、端口控制 |
| **配置页** | `http://<设备IP>/config` | 修改 WiFi、AIDA64、巴法云等设置 |
| **API** | `http://<设备IP>/api/ports` | JSON 格式端口数据 |

### 🏠 巴法云智能家居

通过 MQTT 连接巴法云，实现小爱同学 / 小度音箱语音控制：

| 主题 | 功能 |
|------|------|
| `hass...C1...006` | C1 端口开关 |
| `hass...C2...006` | C2 端口开关 |
| `hass...C3...006` | C3 端口开关 |
| `hass...A...006` | USB-A 端口开关 |
| `hass...BLE...006` | BLE 连接状态 |

---

## 📖 使用方法

### 🔘 按键操作

| 操作 | 功能 |
|------|------|
| **BOOT 短按** (<1s) | 切换 AIDA64 ↔ 充电页面 |
| **KEY 短按** | 切换屏幕亮度主题 |
| **KEY 长按** (3s) | 关机（深度睡眠），短按 KEY 唤醒开机 |
| **BOOT 长按** (3s) | 进入 AP 配网模式 |

### 🔰 首次配置

设备首次启动进入 **AP 模式**，WiFi 热点名称为 `AIDA64-Setup`。

**步骤 1：连接设备**

手机/电脑连接 WiFi `AIDA64-Setup`（密码为空）。

**步骤 2：打开配网页面**

浏览器输入 `192.168.4.1`。

**步骤 3：填写配置**

| 字段 | 必填 | 说明 |
|------|:----:|------|
| **WiFi 名称 (SSID)** | ✅ | 你的 2.4G 路由器 WiFi 名称 |
| **WiFi 密码** | ✅ | 对应的 WiFi 密码 |
| **AIDA64 服务器** | ✅ | 运行 AIDA64 的电脑 IP（如 `192.168.1.100`）|
| **AIDA64 端口** | ✅ | AIDA64 远程监控端口（默认 `7789`）|
| **设备 MAC** | ❌ | 充电头的蓝牙 MAC 地址 |
| **Device Token** | ❌ | 充电头的 Token |
| **BLE Key** | ❌ | 充电头的 BLE 认证密钥 |
| **启用巴法云** | ❌ | 开启后可用小爱/小度语音控制 |
| **巴法云 UID** | ❌ | 巴法云私钥（32 位十六进制）|
| **AppID** | ❌ | 巴法云连接密钥 AppID |
| **SecretKey** | ❌ | 巴法云连接密钥 SecretKey |

**获取充电头的蓝牙信息：**

使用 [Xiaomi-cloud-tokens-extractor](https://github.com/PiotrMachowski/Xiaomi-cloud-tokens-extractor) 从米家云端获取：

```bash
pip install xiaomi_cloud_tokens_extractor
python -m xiaomi_cloud_tokens_extractor
```

选择你的 CUKTECH 充电器，获取：
- **MAC** — 设备蓝牙 MAC 地址
- **Token** — 设备 Token（12 字节 hex）
- **BLE Key** — BLE 认证密钥（16 字节 hex）

**步骤 4：保存并重启**

点击 **保存并重启**，设备自动重启并连接到你的 WiFi。

**步骤 5：验证**

- 屏幕显示 AIDA64 电脑监控数据
- 充电头自动通过 BLE 连接并显示数据

### 🌐 Web 仪表盘

设备正常连接网络后，浏览器打开 `http://<设备IP>`：

- **主页** — 实时充电头端口数据（电压、电流、功率、状态）
- **端口控制** — 点击端口卡片切换启用/禁用
- **配置页** — `http://<设备IP>/config` 修改所有配置
- **API** — `http://<设备IP>/api/ports` 返回 JSON 数据

### 🏠 巴法云 + 小爱同学设置

1. 注册 [bemfa.com](https://bemfa.com)
2. 获取 AppID 和 SecretKey
3. 在设备配置页 (`/config`) 输入并保存
4. 设备自动连接并订阅 5 个主题
5. 在巴法云后台添加相同主题
6. 在小爱同学 / 小度音箱 App 中绑定巴法云
7. 语音控制：
   - "小爱同学，**打开 C1 开关**"
   - "小爱同学，**关闭 C2 开关**"

---

## 📡 API 接口

### `GET /api/ports`

返回 JSON 格式的端口数据：

```json
[
  {
    "port": "C1",
    "voltage": 5.1,
    "current": 1.6,
    "power": 8.16,
    "protocol": "5V",
    "active": true,
    "status": 17,
    "status_raw": 17,
    "enabled": true
  },
  {
    "port": "C2",
    "voltage": 0.0,
    "current": 0.0,
    "power": 0.0,
    "protocol": "idle",
    "active": false,
    "status": 0,
    "status_raw": 0,
    "enabled": true
  }
]
```

### `POST /api/provision`

保存设备配置：

```json
{
  "ssid": "MyWiFi",
  "password": "mypass",
  "aida64_host": "192.168.1.100",
  "aida64_port": 7789,
  "device_mac": "XX:XX:XX:XX:XX:XX",
  "device_token": "12byteshex",
  "ble_key": "16byteshex",
  "bemfa_enable": true,
  "bemfa_uid": "your-uid",
  "bemfa_appid": "your-appid",
  "bemfa_secret": "your-secret"
}
```

---

## 📁 项目结构

```
t-display-s3-aida64-cuktech/
├── main/                          # 核心源码
│   ├── main.c                     # 主入口、任务调度
│   ├── ui.c / ui.h                # LVGL UI (AIDA64 + 充电页面)
│   ├── http_server.c / .h         # Web 仪表盘 + REST API
│   ├── ble_manager.c / .h         # BLE 连接 + MiOT 协议
│   ├── bemfa.c / .h               # 巴法云 MQTT 客户端
│   ├── wifi_prov.c / .h           # WiFi 配网
│   ├── config_store.c / .h        # NVS 配置存储
│   ├── settings.c / .h            # AIDA64 设置
│   ├── queue_msg.h                # BLE 消息队列
│   ├── bt_icons.h                 # 蓝牙图标位图
│   └── CMakeLists.txt
├── components/
│   └── bsp/                       # 板级支持 (按键、LCD)
├── fonts/                         # 自定义字体文件
├── firmware/                      # 预编译固件
│   ├── bootloader.bin
│   ├── partition-table.bin
│   └── t_display_s3_aida64.bin
├── .github/workflows/             # GitHub Actions CI
├── sdkconfig                      # ESP-IDF 配置
├── sdkconfig.defaults             # 默认配置
├── partitions.csv                 # 分区表定义
├── CMakeLists.txt                 # 顶层 CMake
├── .gitignore
├── README.md                   # 中文文档（本文件）
└── README.md                      # 英文文档
```

---

## 📄 许可证

本项目基于 [kairui1108/cuktech-ble-ha](https://github.com/kairui1108/cuktech-ble-ha) 的 ESP32 固件开发，采用 **MIT License**。

---

> 💡 **提示：** 如遇到问题，请先检查串口日志（`idf.py monitor`）获取详细错误信息。欢迎提交 [Issues](https://github.com/JiuzZheyang/t-display-s3-aida64-cuktech-cuktech/issues) 和 [Pull Requests](https://github.com/JiuzZheyang/t-display-s3-aida64-cuktech-cuktech/pulls)。