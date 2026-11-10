# hexagonrpcd 补丁

DBY-W09 的传感器运行在 SDSP（主线中的 SLPI，`0x5c00000`）。固件在初始化
时需要经 FastRPC 反向隧道向 AP 索取注册表与配置文件，并在改动后写回。
上游 `hexagonrpcd` 只提供只读文件服务，也缺少固件调用的 `sns_registry`
本地接口，传感器因此无法注册。

补丁序列来自三星 Galaxy Tab S8（SM-X800）的 linux-msm 叠加层，前七个
原样保留，第八个为 DBY-W09 补充。

## 补丁

| 补丁 | 作用 |
|:--|:--|
| 0001 | HexagonFS 增加 create、write、truncate、unlink、rename |
| 0002 | `apps_std` 支持 `fopen` 的 `w` / `a` / `r+` 与 `fwrite`、`fsync`、`fremove`、`ftrunc` |
| 0003 | 监听循环对未知请求作答并取回输入 |
| 0004 | 把注册表父目录整目录映射给固件 |
| 0005 | 修正文件描述符生命周期与写入流程 |
| 0006 | 实现 `sns_registry` 本地接口 |
| 0007 | 标准输出行缓冲 |
| 0008 | 映射 `/odm/etc/sensors/config`，补充 DBY-W09 属性表 |

## 构建

源码取 `linux-msm/hexagonrpc` 的 `598b591`。先导出目标内核头：

```bash
make -C build/linux-7.3.1 ARCH=arm64 headers_install \
  INSTALL_HDR_PATH=$PWD/build/kheaders
```

交叉文件 `aarch64-cross.ini`：

```ini
[binaries]
c = 'aarch64-linux-gnu-gcc'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = ['-I/path/to/build/kheaders/include']
```

构建：

```bash
git clone https://github.com/linux-msm/hexagonrpc.git
cd hexagonrpc
git checkout 598b591
for p in /path/to/tools/upstream/hexagonrpc/*.patch; do git apply "$p"; done
meson setup build --cross-file /path/to/aarch64-cross.ini --prefix=/usr
ninja -C build hexagonrpcd/hexagonrpcd
```

`tools/sscregistrygen` 依赖宿主机的 json-c，交叉构建时省略，只编译
`hexagonrpcd/hexagonrpcd`。

## 部署

```bash
scp build/hexagonrpcd/hexagonrpcd root@10.0.0.2:/usr/bin/
ssh root@10.0.0.2 'chmod 755 /usr/bin/hexagonrpcd &&
  systemctl enable --now dby-slpi-firmware hexagonrpcd-sdsp'
```

文件树位于 `/usr/share/qcom/sm8250/HUAWEI/dby-w09/`，布局见
[固件清单](../../../docs/firmware.md)。

## 已知限制

- 固件请求的 `hw_platform`、`platform_subtype` 等属性在内核 `soc0` 中
  没有对应项，当前回退到文件树的 `socinfo` 目录；该目录缺少原厂值，
  回答为空字符串。
- 注册表的 `placement` 需要按 [固件清单](../../../docs/firmware.md) 从
  原厂注册表取得。

## 来源与许可证

源码为 linux-msm/hexagonrpc 的 `598b591`，上游项目地址
`https://github.com/linux-msm/hexagonrpc`。项目采用 `GPL-3.0-or-later`，
本目录的补丁沿用该许可证。前七份补丁保留原作者 Fady 与
Aaron Bockelie 的 `From` 行，第八份由本项目补充。
