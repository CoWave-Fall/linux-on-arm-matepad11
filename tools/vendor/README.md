# 第三方打包工具

本目录收录 AOSP 的镜像打包脚本，用于生成 Android recovery 格式的
U-Boot 镜像与关闭校验的 vbmeta。脚本保留原始版权与许可证声明。

| 文件 | 来源 | 许可证 |
|:--|:--|:--|
| `mkbootimg/mkbootimg.py` | AOSP `system/tools/mkbootimg` | Apache-2.0 |
| `mkbootimg/unpack_bootimg.py` | AOSP `system/tools/mkbootimg` | Apache-2.0 |
| `mkbootimg/gki/generate_gki_certificate.py` | AOSP `system/tools/mkbootimg/gki` | Apache-2.0 |
| `avbtool/avbtool.py` | AOSP `external/avb` | MIT / Apache-2.0 / BSD |

许可证全文位于 `LICENSES/` 与各子目录。`scripts/pack-recovery.sh` 直接调用
这些脚本，主机无需安装对应的系统包。
