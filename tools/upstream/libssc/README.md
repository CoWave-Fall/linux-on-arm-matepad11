# libssc CCT 补丁

DBY-W09 的 TCS3408 由 SDSP 固件驱动。`ambient_light` 报文的第一项为
照度（lux），第二项为相关色温（K）。上游 libssc 只取 `intensity[0]`，
色温被丢弃。

补丁在 `SSCSensorLight` 上增加：

- `cct-measurement` 信号，参数为 `gfloat`，单位为 K；
- `ssc_sensor_light_get_cct()`，返回最近一次收到的色温；
- `ssccli --sensor light` 同时输出 `Light sensor CCT: <值> K`。

补丁同时修正 GIR 生成条件：Arch 的 `gobject-introspection-runtime` 提供
pkg-config 文件，缺少 `g-ir-scanner` 时跳过 GIR 生成。既有 `measurement`
信号的参数列表维持原样。

## 版本

- libssc 0.4.4
- 补丁：`0001-light-expose-cct.patch`

## 构建

在设备上构建到独立前缀 `/usr/local/loa-libssc`，系统包提供的
`/usr/lib/libssc.so` 保持原样：

```bash
curl -LO https://codeberg.org/DylanVanAssche/libssc/archive/v0.4.4.tar.gz
tar -xf v0.4.4.tar.gz
cd libssc
patch -p1 < /usr/local/share/loa-tools/upstream/libssc/0001-light-expose-cct.patch
meson setup build --prefix=/usr/local/loa-libssc --libexecdir=lib
meson compile -C build
meson install -C build
```

## 验证

```bash
LD_LIBRARY_PATH=/usr/local/loa-libssc/lib \
  /usr/local/loa-libssc/bin/ssccli --sensor light --timeout 3
```

输出应包含：

```
Light sensor measurement: 143.800003 Lux
Light sensor CCT: 3798.654053 K
```

CCT 数据由 `tools/device/loa-alsd` 发布到会话 D-Bus，再由
`tools/device/kwin-ambient-color` 应用到屏幕。

## 来源与许可证

源码为 libssc 0.4.4，上游项目地址
`https://codeberg.org/DylanVanAssche/libssc`。libssc 采用
`GPL-3.0-or-later`，本目录的补丁沿用该许可证。
