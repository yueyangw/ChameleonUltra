![logo](docs/images/ultra-logo.png)

[English](README.md) | **简体中文**

# ChameleonUltra 固件 — Next 分支

本仓库是 [RfidResearchGroup/ChameleonUltra](https://github.com/RfidResearchGroup/ChameleonUltra)
的分支，在固件中加入了 **16 个模拟卡槽** 和 **按读卡器行为切换的卡槽轮询**。
它是 [Chameleon Ultra Next](https://github.com/yueyangw/ChameleonUltraNext)
客户端的配套固件，同时与官方客户端保持协议兼容：上游的所有命令行为不变，
新功能全部使用新的命令号。

固件下载见 [Releases](https://github.com/yueyangw/ChameleonUltra/releases)。

![ultra picture](docs/images/ultra-overview.png)

## 与上游的区别

### 16 个模拟卡槽

- `TAG_MAX_SLOT_NUM` 改为 16。卡槽配置记录升级到 v9，首次启动时从 v8 自动迁移，
  原有的 1–8 号卡槽保持不变。
- 旧的 `GET_SLOT_INFO` / `GET_ENABLED_SLOTS` 仍只返回 8 个卡槽，字节完全一致，
  旧客户端可以照常使用；16 卡槽客户端使用新的 `*_V2` 命令。
- 加载时校验当前卡槽号，损坏的数据不会再造成越界。9–16 号卡槽的 LED 动画映射到 8 颗物理 LED 上。

### 按读卡器行为切换的卡槽轮询

开启轮询后，设备在读卡器的场中会依次切换已启用的卡槽。它不按固定时间切换，
而是观察读卡器对当前卡片做了什么（REQA/WUPA 轮询、选卡、HALT、MIFARE Classic
认证结果、场复位），只有在读卡器明显已经处理完或者用不上这张卡时才换下一个卡槽。

| 读卡器的行为 | 引擎的处理 |
|---|---|
| 读取 UID 后 HALT（门禁、只读 UID 的读卡器） | 让这张卡被读 `1 + 重读次数` 次，之后静默 `读完后静默等待` 毫秒就换卡槽 |
| MIFARE Classic 认证成功 | 读卡器持续通信时保持当前卡槽，静默 `认证成功后放手` 时长后才释放 |
| MIFARE Classic 认证失败 | 失败达到 `认证失败次数` 后离开；如果读卡器只是在找别的卡，会更早离开；该卡槽在下一轮被跳过一次 |
| 认证失败后读卡器完全静默（如手机把标签交给 App） | 保持该卡槽 `失败后的耐心` 毫秒 |
| 一直轮询但从不选卡 | 约 3 个轮询周期后换卡槽（周期由 REQA/WUPA 的间隔实测得出） |
| 交易进行中场被复位 | 场恢复时出示下一个卡槽 |
| 手机每隔约 2 秒暂停一次场 | 在 `手机重复发现保留` 次发现会话内保持它处理过的卡槽 |
| 有场但没有任何轮询帧（如 LF） | 每 `无轮询帧时的停留` 毫秒切换一次 |

其他细节：

- 只有被 RF 场唤醒的那次开机才会启动轮询。按键、USB、BLE 唤醒不会启动；
  手动切换卡槽后，本次唤醒周期内轮询保持停止。
- 轮询中的卡槽切换不会关闭 NFC 感应，也跳过 140 ms 的 LED 渐变，一次切换远小于 1 ms。
  轮询期间每次掉场都会复位 NFC 外设，避免多次快速切换后回复乱码。
- 增加了无场超时：即使某次切换关闭了感应而没有收到掉场事件，设备也能正常回到休眠。
- 配置保存在 Flash 中（24 字节记录，版本 7）。本分支早期版本写入的记录（v2–v6）
  会自动迁移，新增字段使用当前值。

### 轮询配置（`SET_POLLING_CONFIG`，24 字节，小端）

| 偏移 | 字段 | 默认值 | 含义 |
|---|---|---|---|
| 0 | `version` | 7 | 记录版本 |
| 1 | `enabled` | 0 | 轮询开关 |
| 2 | `slot_mask` | 0 | 第 n 位对应第 n 个卡槽，0 表示所有已启用卡槽 |
| 4 | `idle_dwell_ms` | 500 | 无场时每个卡槽的停留时间 |
| 6 | `active_dwell_ms` | 2000 | 有场但无轮询帧时每个卡槽的停留时间 |
| 8 | `cooldown_ms` | 200 | 掉场后的暂停时间 |
| 10 | `adaptive` | 1 | 无场轮换时加入随机抖动 |
| 11 | `jitter_percent` | 10 | 抖动幅度 |
| 12 | `order` | 0 | 0 顺序，1 随机 |
| 13 | `keep_awake` | 1 | 轮询期间保持唤醒 |
| 14 | `done_quiet_ms` | 100 | 只读 UID 的读卡器：读完后静默多久离开 |
| 16 | `patience_ms` | 800 | 认证失败且静默：保持卡槽多久 |
| 18 | `sticky_sessions` | 3 | 手机重复发现时保持卡槽的会话数，0 关闭 |
| 19 | `fail_poll_quiet_ms` | 150 | 认证失败后读卡器在找别的卡：静默多久离开 |
| 21 | `represent_count` | 1 | 只读 UID 的读卡器：每个卡槽额外被读几次 |
| 22 | `auth_fail_count` | 3 | 认证失败几次结束本次停留，0 关闭 |
| 23 | `lock_quiet_100ms` | 10 | 认证成功的卡槽静默多久（×100 ms）后释放 |

### 新增命令

| 命令号 | 命令 | 数据 |
|---|---|---|
| 1900 | `GET_SLOT_INFO_V2` | 16 个卡槽的 HF/LF 卡类型（各为 u16 大端） |
| 1901 | `GET_ENABLED_SLOTS_V2` | 16 个卡槽的 HF/LF 启用标志 |
| 1902 | `GET_POLLING_CONFIG` | 上述配置记录 |
| 1903 | `SET_POLLING_CONFIG` | 上述配置记录（兼容旧的 16/24 字节格式） |
| 1904 | `GET_POLLING_STATUS` | 20 字节：运行状态、当前卡槽、阶段、命中次数、最后命中卡槽、被跳过的卡槽掩码、实测轮询周期 |
| 1905 | `POLLING_LAB_SET` | 仅开发用：实验标志（任意场、事件日志、无场不轮换） |
| 1906 | `POLLING_LAB_LOG` | 仅开发用：读取轮询事件环形缓冲区 |

实验命令和配套工具（用 Proxmark3 作读卡器、试验矩阵与分析脚本）在
[software/script/polling_lab](software/script/polling_lab/README.md)。

### 其他修复

- 早期版本的轮询配置无法保存（14 字节记录与 16 字节的 FDS 写入不一致），
  现在改为按字对齐的记录并加了大小断言。
- 修复扫光和对称动画中 9–16 号卡槽的 LED 数组越界。

## 刷写固件

Release 中的 `ultra-dfu-app.zip` 是只含应用程序的 DFU 包，使用标准的
ChameleonUltra DFU 密钥签名，原厂 bootloader 可以直接接受；想回到官方固件也用同样的方法刷回去。

- **Chameleon Ultra Next**：工具 → 固件升级，选择 zip，刷写期间保持 USB 连接。
- **其他客户端 / nrfutil**：任何能刷 DFU zip 的客户端都可以，例如在 CLI 中用 `hw dfu`
  进入 DFU，再用 `nrfutil dfu usb-serial` 刷写。

目前只发布 Ultra 版本。Lite 可以从源码构建（`CURRENT_DEVICE_TYPE=lite`），但尚未测试。

## 构建

```bash
cd firmware/application
make -j8 GNU_INSTALL_ROOT=/path/to/arm-none-eabi/bin/
cd ../objects
nrfutil pkg generate --hw-version 0 --key-file ../../resource/dfu_key/chameleon.pem \
  --application application.hex --application-version 1 --sd-req 0x0100 ultra-dfu-app.zip
```

`firmware/build.sh` 与上游一样，构建包含 bootloader 和 softdevice 的完整包。

---

# 上游项目

设备本身的文档：[ChameleonUltra wiki](https://github.com/RfidResearchGroup/ChameleonUltra/wiki)
· [文档仓库](https://github.com/RfidResearchGroup/ChameleonUltraDocs)。

## 兼容的客户端

* [Chameleon Ultra Next](https://github.com/yueyangw/ChameleonUltraNext) — 支持 16 卡槽和轮询配置
* [ChameleonUltraGUI](https://github.com/GameTec-live/ChameleonUltraGUI)
* [MTools BLE](https://github.com/RfidResearchGroup/ChameleonUltra/wiki/mtoolsble)
* [Mifare Chameleon Tool（仅 iOS，Beta）](https://apps.apple.com/it/app/mifare-chameleon-tool/id6761231484)
* [Chameleon Ultra（仅 Sailfish OS）](https://sailfishos-chum.github.io/apps/harbour-chameleon-ultra)
* [Chameleon GO（仅 iOS）](https://apps.apple.com/us/app/id6802015306)
* [Device console for ChameleonUltra（Web）](https://sec1.dk/phreakbyte.html)

除 Chameleon Ultra Next 外，其他客户端只能看到 1–8 号卡槽，也无法配置轮询。

## 授权经销商

法国里昂：[Lab401](https://lab401.com/)

美国圣安娜：[Hackerwarehouse](https://hackerwarehouse.com/)

英国黑斯廷斯：[KSEC](https://labs.ksec.co.uk/product/proxgrind-chameleon-ultra/)

加拿大蒙特利尔：[TechSecurityTools](https://techsecuritytools.com/product/chameleon-ultra/)

中国深圳：[Sneaktechnology](https://sneaktechnology.com)

中国广东：[MTools Tec](https://shop.mtoolstec.com/)

新加坡 Lazada One：[Aliexpress by RRG](https://proxgrind.aliexpress.com/store/1101312023)

## 视频

*录制之后部分操作可能已有变化，如有疑问请以当前文档为准。*

* [下载并编译官方 CLI](https://www.youtube.com/watch?v=VGpAeitNXH0)
* [下载 ChameleonUltraGUI](https://www.youtube.com/watch?v=rHH7iqbX3nY)
* [ChameleonUltraGUI 功能概览](https://www.youtube.com/watch?v=YqE8wyVSse4)
* [使用 ChameleonUltraGUI 与 Chameleon Ultra](https://www.youtube.com/watch?v=9jtKNJ5-kVY)
* [MTools BLE：用 ChameleonUltra 复制卡片](https://youtu.be/IvH-xtdW1Wk?si=4exqgAAeJ-kxU3aN)

## 社区

* [RFID Hacking 社区 Discord](https://t.ly/d4_C)
  * Software/chameleon-dev：固件与客户端开发讨论
  * Devices/chameleon-ultra：使用讨论
* [GameTec_live Discord](https://discord.gg/DJ2A4wxncK)

其他值得关注的项目：
* [Fantasi](https://fantasi.cloud)（[Discord](https://fantasi.cloud/discord)）
* [chameleon-ultra.js](https://github.com/taichunmin/chameleon-ultra.js/)
