# 架构决策 004：开锁从手机 NFC 改为手机 BLE

状态：**已采纳**（2026-09-02）。取代 [`archive/ADR-003-48v-nfc-only.md`](archive/ADR-003-48v-nfc-only.md)
的 §2、§3.4、§4.3、§7.1，以及 [`DESIGN.md`](DESIGN.md) §2 全节、§3.3、§3.5、§5.1、§5.4。
供电、GNSS、运动唤醒、4G 链路、MQTT 契约、服务端与 HA **一个字都没变**。

> **触发**：用户决定「从手机 NFC 改成蓝牙（BLE）」。这不是讨论要不要，是执行。
> 本文记录**执行前核实到的事实**、**取舍**、以及**丢掉了什么** ——
> 特别是最后一条：ADR-003 §3.4 把 NFC 的 ~4 cm 作用距离称为「最强的防御」，
> 这次改动把它整个丢掉了。不显式记账，半年后没人知道为什么被偷。

## §0 一句话

nRF52840 从 **NFC 标签** 变成 **BLE GATT peripheral**；手机从读卡器变成 central；
**报文一字未改**（同一套 ISO7816-4 APDU、同一个 HMAC-SHA256 挑战应答、
同一份 `unlock.c` 三重校验），换的只是载体：
`nfc_t4t_response_pdu_send()` → `bt_gatt_notify()`，`NFC_T4T_EVENT_DATA_IND` → GATT write 回调。

**只做「手动开锁」，不做「走近自动开锁」。** 后者是本次改动唯一的新攻击入口（§4）。

净收益与净代价一次说清：

| | 内容 |
| --- | --- |
| **赚到** | 开锁步数 4 → 2；锁屏下可用、不用摸车；iOS 从「放弃」变「可做」；释放 P0.09/P0.10 两个 GPIO，余量 19 → 21；**§3.3 整节的 NFC 天线调谐工作（VNA/示波器收敛两颗电容）消失**；`UICR.NFCPINS` 那个一次性写入不再需要 |
| **付出** | **~4 cm 物理近场防御消失，中继攻击成为真实威胁**（§4）；多一套 GATT/广播/连接管理代码；Android 11 及以下要精确位置权限 + 系统定位开关常开（§2.2）；设备地址可被沿街扫描长期跟踪（§6 决策 C） |

---

## §1 链路：谁是 central

**方向反了，而且这次是我们更方便的那个方向。** NFC 下 nRF52840 只能当标签
（NFCT 只有 load modulator，产生不了载波 —— ADR-003 §0 的硬约束），
所以必须手机发场、手机发命令。BLE 下没有这个限制，但我们**仍然让手机主动**：

- 设备 = **peripheral**（`CONFIG_BT_PERIPHERAL=y`），只广播、只等连接，**从不扫描、从不发起连接**。
- 手机 = central，连上来写 APDU。

保持这个方向的理由不是技术限制，是**状态机不用改**：`unlock.c` 的
`selected` / `nonce_valid` / `cur_nonce` 三个状态原本就是「等手机发下一条命令」的形状。

GATT 表（`ble_unlock.c`）：

| 特征 | 属性 | 用途 |
| --- | --- | --- |
| CMD | `WRITE`（write request，不是 write-without-response） | 手机写整条 C-APDU |
| RSP | `NOTIFY`，`BT_GATT_PERM_NONE` | 车回 R-APDU |

UUID 是一个随机生成的 128-bit 基址 `2c1327ba-a717-4314-827e-92532d7a xxxx`，
低 16 位 `0001`/`0002`/`0003` 分别是服务/CMD/RSP。

**为什么 nonce 不做成独立的 read 特征。** 直觉上 GET CHALLENGE 更像「客户端拉」，
用 read 还能省掉写 CCCD 那一个往返。但 read 回调里生成 nonce 意味着
**任何人连上来读一下就能作废合法用户的 nonce** —— `unlock.c` 的 `nonce_valid`
会变成一个远程可 DoS 的状态位（攻击者反复读，合法用户永远拿不到有效 nonce）。
所以三步命令全部走 CMD 写，nonce 只在收到合法 GET CHALLENGE 时生成。
代价是每次连接多一个 CCCD 写往返（一个连接事件，几十毫秒），可接受。

---

## §2 安卓侧：BLE 到底好在哪、坏在哪

### §2.1 好的那一半：两道门都没有

ADR-003 §2.1 / DESIGN.md §2.3 记录的 NFC 两道门，都引自 AOSP：

- 门 1：`NfcService` 的 `NFC_POLLING_MODE = ScreenStateHelper.SCREEN_STATE_ON_UNLOCKED`
  （档位 `OFF_UNLOCKED=0x01 < OFF_LOCKED=0x02 < ON_LOCKED=0x04 < ON_UNLOCKED=0x08`，
  门槛取最高档）—— **息屏或锁屏时手机根本不发场**；
- 门 2：`setReaderMode` 要求调用方在前台（`ForegroundUtils.isInForegroundLocked()`，
  判 `getUidImportance(uid) == IMPORTANCE_FOREGROUND`），App 一进后台就
  `resetReaderModeParams()`，日志原文 `Disabling reader mode because app died or moved to background`。

**BLE 侧这两条一条都没有。** 在 AOSP 镜像
`GrapheneOS/platform_packages_modules_Bluetooth` 分支 `17` 上核实：

- `android/app/src/com/android/bluetooth/gatt/GattService.java` 里
  grep `Keyguard|isInteractive|SCREEN_STATE|isScreenOn|PowerManager` → **零匹配**；
  `clientConnect()`（`:1064-1101`）没有任何屏幕/锁屏判断。
- `le_scan/ScanManager.java` 里 grep `Keyguard` → **零匹配**；屏幕状态只用于**降频**，
  取自 `mScreenOn = mAdapterService.getDisplayListener().isScreenOn()`（`:251`），
  语义是 `Display.STATE_ON`，**不含 keyguard**。

⇒ **锁屏、息屏下 GATT 连接与读写都能做。这是本次改动唯一量级上的收益。**
开锁步数从「亮屏 → 解锁 → 开 App → 贴车」的 **4 步**，降到「亮屏 → 点开锁」的 **2 步**
（用锁屏小组件/快捷设置触发可以更少），务实基线 3 步（App 冷启动）。

### §2.2 坏的那一半：权限与定位开关

| | Android 12+（API 31） | Android 11 及以下 |
| --- | --- | --- |
| 权限 | `BLUETOOTH_SCAN` + `BLUETOOTH_CONNECT`，一次「附近的设备」弹窗 | `BLUETOOTH` + `BLUETOOTH_ADMIN` + **`ACCESS_FINE_LOCATION` 运行时授权** |
| 位置 | 加 `android:usesPermissionFlags="neverForLocation"` 后**完全不碰位置** | **强制**，官方理由逐字：*"because, on Android 11 and lower, a Bluetooth scan could potentially be used to gather information about the location of the user"* |
| 系统定位开关 | 不依赖 | **必须打开** |

版本边界卡得很干净，代码层面可查：

- Android 12 起有逃生口：`Util.kt:274-316` 的 `hasDisavowedLocationForScan()` 末行
  `return (flags and PackageInfo.REQUESTED_PERMISSION_NEVER_FOR_LOCATION) != 0`；
  `le_scan/ScanUtil.kt:118-131` 的 `hasScanResultPermission()` 在
  `client.hasDisavowedLocation` 时直接短路返回 true。
  常量 `REQUESTED_PERMISSION_NEVER_FOR_LOCATION` = 0x00010000，官方 reference 明写 **Added in API level 31**。
- Android 11（`LineageOS/android_packages_apps_Bluetooth` 分支 `lineage-18.1`）**没有**：
  `gatt/GattService.java:1196-1204` 的 `hasScanResultPermission()` 只有一句
  `return client.hasLocationPermission && !Utils.blockedByLocationOff(this, client.userHandle);`，
  而 `Utils.java:383-386` 的 `blockedByLocationOff()` 就是
  `!getSystemService(LocationManager.class).isLocationEnabledForUser(userHandle)`。
  该文件里 grep `disavow|neverForLocation|NEVER_FOR_LOCATION` → **零匹配**。

**但这一整段只影响「扫描」。** 见 §2.3。

### §2.3 主连接路径：按 MAC 直连，绕开上面全部

`BluetoothAdapter.java:1084-1088` 的 `getRemoteDevice(String address)` 标着
**`@RequiresNoPermission`** —— 拿设备对象零权限。之后 `connectGatt()` 六个重载
（`BluetoothDevice.java:2972-3170`）只要 `BLUETOOTH_CONNECT`。

⇒ **已知 MAC 的连接路径不需要 `BLUETOOTH_SCAN`、不需要位置权限、不需要定位开关、
不需要亮屏解锁。** 扫描只在**首次配对**用一次（且必须带 `ScanFilter`，见下）。

这条路径依赖**固定地址**，因此和 §6 决策 C（要不要开 `CONFIG_BT_PRIVACY`）是同一个决定：
开了 privacy，MAC 每 `CONFIG_BT_RPA_TIMEOUT`（默认 900 s）轮换一次，`getRemoteDevice(mac)` 直接失效；
按 IRK 过滤的 `ScanFilter.Builder.setDeviceAddress(address, addressType, irk)` 是
`@Hide @SystemApi`（`ScanFilter.java:862-872`）且需要 `BLUETOOTH_PRIVILEGED`，普通 App 拿不到。

### §2.4 广播包里不放服务 UUID，但扫描仍能过滤

设备侧的取舍：**广播包（AD）只放 Flags**，服务 UUID 放在**扫描响应（SD）**里。
理由是防盗产品不该对着街上广播「我是一台可以被 BLE 开锁的车」；
而安卓的 BLE 扫描是主动扫描（发 SCAN_REQ），`ScanRecord` 由广播包与扫描响应**合并**而成，
所以 `ScanFilter.setServiceUuid()` 仍然匹配得到 —— 被动嗅探只看到一个裸 Flags。

**手机侧必须带 `ScanFilter`**，这是硬要求不是优化：无过滤扫描在息屏或定位关闭时会被
**静默挂起**（`ScanManager.java:373-392`，日志 `Cannot start unfiltered scan in screen-off...`
后 `mSuspendedScanClients.add(client); return;` —— App 收不到任何回调）。
判定函数 `ScanUtil.kt:326-331`：
`requiresScreenOn = !isOpportunistic && !hasNonEmptyFilters` /
`requiresLocationOn = !hasDisavowedLocation && !hasNonEmptyFilters`。带过滤器 ⇒ 两道挂起门都不适用。

### §2.5 App 侧三个必须写对的地方

1. **`onServicesDiscovered` 之前不能读写。** `getServices()` javadoc 逐字：
   *"Returns an empty list if service discovery has not yet been performed."*（`BluetoothGatt.java:1244-1252`）
2. **同时只能有一个 GATT 操作在飞**，框架层硬锁：`BluetoothGatt.java:87-90` 的
   `mDeviceBusyLock`/`mDeviceBusy`，每个操作入口 `if (mDeviceBusy) return false;`
   （`writeCharacteristic` 在 `:1478-1483` 返回 `ERROR_GATT_WRITE_REQUEST_BUSY` = 201）。
   **必须自己做串行队列。**
3. **每个 `BluetoothGatt` 用完 `close()`**，不是 `disconnect()`：
   `GattService.java:158` 的 `GATT_CLIENT_LIMIT_PER_APP = 32`，超限只 `Log.w` +
   `onClientRegistered(GATT_FAILURE)`。javadoc 逐字：*"Application should call this
   method as early as possible after it is done with this GATT client."*（`:972-980`）

另外 **`requestMtu` 不能省**：默认 ATT MTU 23（`gatt_api.h:320` 的
`GATT_DEF_BLE_MTU_SIZE`）只给 20 字节可写，而 UNLOCK 的 C-APDU 是 30 字节（§3.2）。
分支 17 的新 API 默认自动协商（`BluetoothGattConnectionSettings.java:115`
`mAutomaticMtuEnabled = true`），但旧的三参 `connectGatt` 内部显式
`.setAutomaticMtuEnabled(false)`（`BluetoothDevice.java:2976-2981`）—— 走旧 API 必须自己调。
`[未核实]` 自动 MTU 是 AOSP 主线行为还是 GrapheneOS 分支行为、从哪个 API level 起。

### §2.6 时延

`[推断]`（参数有出处，加总是估算）：前台最好 ≈ 0.3 s，务实 1~4 s。
构成：扫描发现 0.1~2 s（手机 LOW_LATENCY 是 100 ms 窗/100 ms 间隔即 100% 占空比，
`ScanUtil.kt:59-67`；设备广播 100~150 ms）+ 连接建立 30~50 ms 起
（AOSP 默认 `BTM_BLE_CONN_INT_MIN_DEF = 24` / `MAX_DEF = 40`，即 30/50 ms，
`btm_ble_api_types.h:167-181`；与 Zephyr `gap.h:82-84` 的
`BT_GAP_INIT_CONN_INT_MIN/MAX` 一致，不用谈判）+ 服务发现 0.05~0.6 s
+ 三步协议 0.1~0.3 s。`requestConnectionPriority(CONNECTION_PRIORITY_HIGH)` 可压缩。

对比 NFC 贴上之后的 `[推断]` 100~300 ms：**BLE 赢在人的动作，不赢在射频时延。**

---

## §3 设备侧：功耗与配置

### §3.1 广播功耗（手算，标 `[未核实]`）

Nordic Online Power Profiler 页面全部 HTTP 403 取不到，所以用产品规格书的电流 +
包时长手算。每项输入有出处（nRF52840 PS v1.1）：
`I_TX,0dBm,DCDC` = 4.8 mA（§6.20.15.2）、`I_RX,1M,DCDC` = 4.6 mA（§6.20.15.3）、
`t_TXEN,BLE,1M` = 140 µs（§6.20.15.8）、`I_ON_RAMON_RTC` = 3.16 µA（§5.2.1.1）。
⚠ **不开 DC/DC 的话 TX 是 10.6 mA，两倍多。**

一次 legacy connectable 广告事件 ≈ 3 信道 × (376 µs TX + 150 µs RX 窗 + 140 µs ramp-up)
+ HFXO 起振 ≈ **13 µC**：

| 广播间隔 | 平均电流（含 3.16 µA 底噪） |
| --- | --- |
| 100 ms | **~133 µA** |
| 1 s | **~16 µA** |
| 2 s | **~10 µA** |

**对比基准**：NFCT ACTIVATED 是 **400 µA**（PS §6.2.1.4，与 DESIGN.md §2.5 一致），
4G 模组低功耗档 500~1500 µA。也就是说：

> **BLE 广播比它取代的 NFCT 更省电，且完全淹没在 4G 模组的功耗地板下面。**
> 折到 48V 20Ah（960 Wh）车电池：100 ms 间隔约 0.0013 %/天，1 s 间隔约 0.00016 %/天。

⇒ **省电不是关广播的理由。** 本工程选 **100~150 ms**（`BT_GAP_ADV_FAST_INT_*_2`），
买的是发现时延；静止 5 分钟后关广播的理由是**防跟踪**（不广播 = 不可被发现、
不可被枚举、不可被沿街扫描长期跟踪一辆车）。`main.c` 里那句
「关 NFC 省电（400 µA）」的日志与注释因此全部改写 —— 不改的话，
半年后看代码的人会以为在省电，然后为了「省电」把间隔调到 2 s，白白牺牲时延。

### §3.2 MTU 必须调，差的不止一点

- ATT MTU 默认 23（`host/att_internal.h:23` `BT_ATT_DEFAULT_LE_MTU`）；
  写请求可写载荷 = ATT_MTU − 3（1 B opcode + 2 B handle）= **20 字节**。
- 最长 C-APDU 是 UNLOCK：`80 10 00 00 Lc [uid(4)||counter(4)||mac(16)] 00` = **30 字节**。
  即使剥掉 ISO7816 外壳只发 24 字节 body 也装不下 20。
- 本地上限 = `MIN(BT_L2CAP_RX_MTU, BT_L2CAP_TX_MTU)`（`att_internal.h:38`），
  其中 `BT_L2CAP_RX_MTU = CONFIG_BT_BUF_ACL_RX_SIZE − 4`（`l2cap.h:47` + `:41`）。

所以 `prj.conf`：`CONFIG_BT_L2CAP_TX_MTU=40` + `CONFIG_BT_BUF_ACL_RX_SIZE=44`。
**保留整条 APDU 不剥壳**：`unlock_handle_apdu()` 一行都不用改，而 5 字节的
ISO7816 头换来的是「传输层可替换」这个性质本身 —— 这次改动能只花一个模块的代价，
正是因为上一轮把协议定在了 APDU 层。

⚠ 副作用：`ACL_RX_SIZE > 27` 会让 `BT_DATA_LEN_UPDATE` 变 default y，
进而 `BT_CTLR_DATA_LENGTH=y`，SDC 的 per-link 缓冲跟着涨。**这是划算的** ——
换掉的是应用层分包逻辑，而分包会让「nonce 一次性」的安全论证被半条报文复杂化。

### §3.3 写回调不能跑密码学

调用链核实：`host/hci_core.c:124-156` 起 `bt_workq`（线程名 `"BT RX WQ"`，
`K_PRIO_COOP(CONFIG_BT_RX_PRIO)`，`BT_RX_PRIO` 默认 8，**协作式**）→
`rx_work_handler()` → `hci_acl()` → `bt_conn_recv()` → `bt_l2cap_recv()` →
`host/att.c:2145-2147` **`write = attr->write(data->conn, attr, data->value, data->len, data->offset, flags);`**

即写回调**同步跑在协议栈自己的协作式工作队列线程上**。官方指导
（`doc/services/connectivity/bluetooth/bluetooth-le-host.rst:102-142`）逐字：
*"time spent in the callback delays other Bluetooth activity"*、
*"Keep callbacks short, and defer work that is long-running or blocks indefinitely
to an application-owned thread or work queue."*

所以 `on_cmd_write()` **只做 `memcpy` + `k_work_submit_to_queue()`**，
`unlock_handle_apdu()` 跑在 `ble_unlock.c` 自有的 work queue 上。三条理由：

1. **DoS**：攻击者可以无限灌 UNLOCK 写请求，每条都要一次 CC310 HMAC。
   不 defer 的话这会拖垮同一队列上的所有 BT 处理（含连接维持）。
   **这是 NFC 侧不存在的攻击面** —— NFC 得贴上来。
2. 不用 sysworkq：nvstore 的 counter 延迟落盘挂在那上面，而 flash 写被
   `SOC_FLASH_NRF_RADIO_SYNC_MPSL` 排到 MPSL timeslot 之后。别让开锁应答排在一次擦写后面。
3. 分离之后写回调可以立刻 `return len`（Write Response 秒回），应答走 notify 天然异步。

**并发保护**：`unlock.c` 的 `cur_nonce`/`nonce_valid`/`selected` 是无锁静态状态。
NFC 下只有一个回调线程所以安全；改 BLE 后仍然安全，**前提是所有 unlock 状态的访问
都只在 work handler 里**（写回调只 memcpy + submit）。
⚠ `CONFIG_BT_MAX_CONN` 一旦 > 1，或者写回调也要读 unlock 状态，这个论证就失效，必须补锁。
`prj.conf` 里 `CONFIG_BT_MAX_CONN=1` 是这个论证的一部分，不是随手写的。

另外 `on_cmd_write()` 里那个 `busy` 原子标志**不是防御性代码**：写回调立刻返回、
应答异步，手机完全可以在上一条还在算的时候发下一条（ATT 只保证「一请求一响应」，
不保证我们处理完了），没有它第二条写会踩掉正在处理的缓冲。

### §3.4 进 System OFF 之前要 `bt_disable()`

`bt_disable()` 存在且可反复 enable/disable —— 「Zephyr 不支持 disable」在 v3.1 就过时了
（`doc/releases/release-notes-3.1.rst:203`）；上游有专门的回归测试跑 35 轮
（`tests/bsim/bluetooth/host/misc/disable/src/main_disable.c:21-39`）。
nRF52 侧真的释放硬件：`sdk-nrf/subsys/bluetooth/controller/hci_driver.c:1506-1538`
`hci_driver_close()` → `sdc_disable()`（同步，「After the SoftDevice Controller is
disabled, Bluetooth LE functionality is no longer available」）。

**但日常门控用 `bt_le_adv_start/stop`，不用 `bt_disable()`**：
`bt_enable()` 有启动延迟（一串 HCI 命令），而运动唤醒后用户正等着开锁，
几十毫秒可感知；`bt_disable()` 还会清 identity（地址变）。

`bt_disable()` 只在 `enter_system_off()` 里调，且**必须在 `nvstore_flush()` 之前**：
radio 是 EasyDMA master（产品规格书要求进 System OFF 前 EasyDMA 传输已完成，
这和现有第 4 步对 UARTE/TWIM/SAADC 的处理是同一条推理），
而 `SOC_FLASH_NRF_RADIO_SYNC_MPSL` 会让 flash 写等 MPSL timeslot ——
radio 开着时那次 flush 的最坏延迟不可控。
失败**不放弃关机**（和「武装唤醒失败就不关机」的处理不同：醒不过来是致命的，
radio 多耗电只是浪费）。

`[未核实]`：`CONFIG_BT_UNINIT_MPSL_ON_DISABLE`（默认 n）不开时，残留的 MPSL 状态
是否影响 0.40 µA 的 System OFF 实测值。需要真机 PPK2 测一次。

---

## §4 中继攻击：本次改动的真实代价

### §4.1 密码学一点没变，也一点都不防中继

`unlock.c` 的三重校验（MAC 正确、counter 严格递增、nonce 一次性）保证的是
**「录到一次完整交互不能重放第二次」**。**中继不是重放。** 攻击者不需要理解也不需要解密，
只是把链路层比特实时转发，让真手机在几百米外「诚实地」完成一次全新的挑战应答 ——
三重校验会全部通过并回 `90 00`。

NCC Group 2022-05 做出的 BLE link-layer relay，逐字：

> By forwarding data from the baseband at the link layer, the hack gets past known
> relay attack protections, **including encrypted BLE communications**, because it
> circumvents upper layers of the Bluetooth stack and the need to decrypt

> we can convince a Bluetooth device that we are near it—even from hundreds of miles
> away ... **even when the vendor has taken defensive mitigations like encryption and
> latency bounding**

> All it takes is 10 seconds—and these exploits can be repeated endlessly

已实证 Tesla Model 3/Y、Kwikset/Weiser Kevo 智能锁。
出处：<https://www.nccgroup.com/newsroom/ncc-group-uncovers-bluetooth-low-energy-ble-vulnerability-that-puts-millions-of-cars-mobile-devices-and-locking-systems-at-risk/>

**ADR-003 §3.4 那句「NFC 的 ~4 cm 作用距离本身就是最强的防御 —— 这是它相对蓝牙
10~20 m 的核心优势」在本次改动后失效。这是最大的一笔代价，必须显式记账。**
同时 ADR-003 §7.1 记的「BLE 好处：中继攻击防御白送、少一套 GATT 攻击面、
少写配对/连接管理」三条现在全部反向变成成本。

### §4.2 测距类缓解在这颗芯片上不成立

- **Channel Sounding（BT 6.0）在 nRF52840 上不存在。** 依赖链：
  `zephyr/subsys/bluetooth/Kconfig:220-223` 的 `BT_CHANNEL_SOUNDING`
  `depends on !HAS_BT_CTLR || BT_CTLR_CHANNEL_SOUNDING_SUPPORT`；
  NCS 侧 `subsys/bluetooth/controller/Kconfig:46`
  `select BT_CTLR_CHANNEL_SOUNDING_SUPPORT if HAS_HW_NRF_RADIO_CS`；
  而 `HAS_HW_NRF_RADIO_CS` 是 `soc/nordic/common/Kconfig.peripherals:143-144`
  的 `def_bool $(dt_nodelabel_bool_prop,radio,ble-cs-supported)`，
  **`dts/arm/nordic/nrf52840.dtsi:123-131` 的 radio 节点只有
  `ieee802154-supported` / `ble-2mbps-supported` / `ble-coded-phy-supported` /
  `radio-tx-high-power-supported`，没有 `ble-cs-supported`。** ⇒ 编不出来。
- **RSSI 能读**（`BT_HCI_OP_READ_RSSI` = 0x1405，`hci_types.h:1241-1248`；
  Kconfig `BT_CTLR_CONN_RSSI`，SDC 已 `select BT_CTLR_CONN_RSSI_SUPPORT`），
  **但放大型中继直接击穿它** —— 中继设备可以用大功率发射让 RSSI 看起来很近。
  RSSI 只能挡「懒人中继」，不能当安全边界。
- **往返时延门限**：BLE 连接间隔是几十毫秒量级，而 NCC 明说他们绕过了 latency bounding。
  在这个时间粒度上做有意义的距离约束不现实。

### §4.3 结论：靠「用户显式动作」，这正是 NCC 点名的防御

NCC 给的三条缓解，逐字：

1. *"Manufacturers can reduce risk by disabling proximity key functionality when the
   user's phone or key fob has been stationary for a while (based on the accelerometer)"*
2. *"System makers should give customers the option of providing a second factor for
   authentication, or user presence attestation (e.g., tap an unlock button in an app on the phone)"*
3. *"Users of affected products should disable passive unlock functionality that does
   not require explicit user approval"*

**第 2 条就是「手动开锁」。** 本方案因此：

- **不做走近自动开锁。** 开锁必须由手机上的显式动作触发。
  这不只是「先不做」——**自动开锁是中继攻击唯一的实际入口**：
  没有它，攻击者中继出来的链路两端一边是车、一边是一个不会自动应答的手机，
  攻击链断在人这一环。
- 车侧的 LIS2DW12 已经在板上，但它测的是**车**动没动，不是手机。
  NCC 第 1 条要的「手机静止就关」需要安卓侧 `ACTIVITY_RECOGNITION` 权限，
  `[未核实]` 值不值这个权限成本 —— 在不做自动开锁的前提下这一条不需要。
- **机械钥匙孔无论如何都不能省**（ADR-003 §7 已承认「只有一条开锁路径 ⇒
  手机没电就完全打不开车」）。这条在本次改动后更重要：BLE 还多了
  「蓝牙被关」「权限被回收」两种失效模式，而且 Android 13 起
  App 不能自己开蓝牙（`BluetoothAdapter.java:1450-1492`，targetSdk ≥ 33 时
  `enable()` 一律返回 false，只能弹 `ACTION_REQUEST_ENABLE`）。

---

## §5 为什么不开 SMP（不做配对加密）

`prj.conf` 里 `CONFIG_BT_SMP=n`、`CONFIG_BT_SETTINGS=n`，
CMD 特征用 `BT_GATT_PERM_WRITE` 而不是 `BT_GATT_PERM_WRITE_ENCRYPT`。**这是有意的。**

**决定性事实：设备没有屏幕也没有键盘，IO capability 只能是 `NoInputNoOutput`，
那种配置下 LESC 只能退化成 Just Works，而 Just Works 没有 MITM 防护。**
Zephyr 源码把这条钉死：

- `host/smp.c:2919-2922` 的 `get_auth()`：
  `if ((get_io_capa(smp) == BT_SMP_IO_NO_INPUT_OUTPUT) || ...) auth &= ~(BT_SMP_AUTH_MITM);`
  —— NoInputNoOutput 时**主动清掉 MITM 位**。
- `host/smp.c:2984-2998` 的 `sec_level_reachable()`：`BT_SECURITY_L3` 和 `L4` 都要求
  `get_io_capa(smp) != BT_SMP_IO_NO_INPUT_OUTPUT`（或有 OOB 回调）。
- `host/smp.c:2946-2982` 的 `remote_sec_level_reachable()`：`L3`/`L4` 分支下
  `if (smp->method == JUST_WORKS ...) return BT_SMP_ERR_AUTH_REQUIREMENTS;`

⇒ `CONFIG_BT_SMP_SC_ONLY`（Secure Connections Only，强制 L4）在我们的 IO 配置下
**只会让配对直接失败**，不会带来 MITM 防护。`CONFIG_BT_SMP_ENFORCE_MITM`（默认 y）
同样被上面那个 `NO_INPUT_OUTPUT` 分支绕过。

**真正的认证在 §5.2 的 HMAC 挑战应答里，那一层不依赖链路加密**：
secret 是 per-user 的 32 字节，永不上链路，nonce 一次性，counter 严格递增。
链路加密能买到的只有「窃听者看不到明文 APDU」—— 而明文 APDU 里没有秘密
（nonce 是公开的挑战，mac 是一次性的应答）。

不开 SMP 的连带收益：
- 没有 LTK/IRK 要持久化 ⇒ 不需要 `CONFIG_BT_SETTINGS` ⇒ **BT 不去写 counter 住的那
  32 kB storage 分区**。counter 是「错了就有人偷走车」的那个字节，
  而那块分区的擦写寿命是每页 10000 次；让 BT 的绑定信息去分摊它是错的。
- 省掉 `settings_load()` 的时序要求（`bluetooth.h:326-333` 要求 `bt_enable()`
  **之后**才能 load bt 设置，而 `nvstore_init()` 在 main 早期就 `settings_load()`
  —— 开了 `BT_SETTINGS` 就必须把它拆成两段，那是个实打实的重构）。
- 省掉 `BT_RX_STACK_SIZE` 因 `BT_SETTINGS` 而多占的 ~1.2 kB RAM。

**代价与边界**：任何人都能连上来写 CMD 特征。这不是漏洞（写进去也开不了锁），
但它意味着 §3.3 的那个 DoS 面是**未认证可达**的 —— 这正是必须 defer 到自有
work queue 的原因。将来若要「只允许配对过的手机连」，就得开 SMP + `BT_SETTINGS`，
上面所有代价全部回来，`settings_load` 时序也要改。**当前判断是不值得。**

---

## §6 留给用户拍板的四件事（都不阻塞施工）

| | 决策 | 本文的默认与理由 |
| --- | --- | --- |
| **A** | **走近自动开锁做不做** | **不做**（默认关，代码里根本没有这条路径）。它是中继攻击唯一的实际入口（§4.3），而 NCC 明确建议关掉 passive unlock |
| **B** | **最低支持的 Android 版本** | 建议 **12+（API 31）**：加 `neverForLocation` 后完全不碰位置权限与定位开关。含 11 及以下 ⇒ 必须要精确位置 + 系统定位开关常开，**代码层面没有任何绕法**（§2.2） |
| **C** | **设备地址策略** | 当前 **固定地址**（不开 `CONFIG_BT_PRIVACY`），换来「按 MAC 直连」这条最干净的路径（§2.3）。代价是一辆车可被沿街扫描长期跟踪；本方案用「静止 5 分钟后停广播」部分缓解。开 privacy 则必须先完成 SMP 绑定才能连（§5 的代价全部回来） |
| **D** | **要不要保留 NFC 当第二通道** | 当前**不保留**（`nfc_tag.c/h` 已删）。`unlock_handle_apdu()` 是传输无关的，两套传输共用同一状态机成本很低；但保留意味着 P0.09/P0.10 不释放、§3.3 的天线调谐工作不能删、`UICR.NFCPINS` 那个一次性写入还得做。**机械钥匙孔已经覆盖了「手机不可用」这个场景**，第二条电子通道的边际收益不足 |

---

## §7 本次改动的文件清单

| 文件 | 改动 |
| --- | --- |
| `firmware/nrf52840/src/ble_unlock.c/.h` | **新增**。GATT 服务、广播门控、defer 到自有 work queue、`bt_disable()` 收尾 |
| `firmware/nrf52840/src/nfc_tag.c/.h` | **删除** |
| `firmware/nrf52840/prj.conf` | 删 3 个 `CONFIG_NFC_*`；加 BT/MTU 一组（含为什么不开 SMP/SETTINGS 的推理） |
| `firmware/nrf52840/CMakeLists.txt` | `nfc_tag.c` → `ble_unlock.c` |
| `firmware/nrf52840/src/main.c` | 头注释状态机、`IDLE_AFTER_STILL_MS` 的动机（省电 → 防跟踪）、`on_motion`、`enter_system_off` 第 1 步、init 顺序 |
| `firmware/nrf52840/src/proto.h/.c` | `EV_NFC_ERR`/`"nfc_err"` → `EV_BLE_ERR`/`"ble_err"` |
| `firmware/nrf52840/src/{unlock,lock,gnss,battery,motion}.{c,h}`、`uplink.c`、`Kconfig`、overlay | 注释与文案：调用上下文从「NFC 回调」改为「GATT 派生的 work handler」；引脚余量 19 → 21 |
| `server/ebike_server/contract.py` | 事件闭集 `nfc_err` → `ble_err` |
| `server/ebike_server/{api,web,web_assets}.py`、`docs/{MQTT-CONTRACT,SERVER,WEB,HA}.md`、HA `binary_sensor.py`、`server/tests/test_api.py` | 文案「NFC 挑战应答」→「BLE 挑战应答」；事件标签「NFC 异常」→「BLE 异常」 |
| `docs/DESIGN.md` | §0 一句话、§1 架构图与角色表、§2 全节重写、§3.3（NFC 天线 → 删）、§3.5 引脚表、§5.1/§5.4 威胁模型、§10 分期 R3、§11 |

**报文契约本身一字未改**（topic、字段、指令闭集、`unlock` 的三步 APDU）。
唯一的契约变更是事件闭集里的一个字符串名 —— 那个事件从来没被实际发出过。
