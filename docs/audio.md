# 音频

更新时间：2026-11-15 21:20 +08:00

声音经 ADSP 上的 AFE、ADM 与 ASM 服务处理。扬声器走
`PRIMARY_TDM_RX_0` 的四槽 TDM，四颗 TFA9874 各监听一个槽位；模拟麦
经 WCD938x 的 ADC1..4 进入 `TX_CODEC_DMA_TX_3`；DisplayPort 音频走
`DISPLAY_PORT_RX`。用户空间由 PipeWire 通过 UCM 打开对应通路。

## 固件

ADSP 的 PAS 固件取自设备 modem 分区的 `image/adsp.mdt` 与
`image/adsp.b00..b18`，放在 `/lib/firmware/qcom/sm8250/dby-w09/`。
固件位于根文件系统，内核在挂载根之前 probe，因此
`dby-adsp-firmware.service` 在 `local-fs.target` 就绪后重新绑定一次。

## 功放配置

四颗 TFA9874 的初始化序列随设备 ODM 分区提供，安装后提取一次：

```bash
sudo extract-odm-audio.sh
```

脚本把 `tfa9874_0..3_configs.xml` 复制到 `/etc/loa-smartpa/`。
`loa-smartpa.service` 在播放开始前初始化芯片，按屏幕朝向分配槽位，
播放结束后下电。四颗功放各驱动一条声道，四个声道均可用；屏幕方向
变化时由该服务重新映射槽位，横屏、竖屏与两个翻转方向各自对应一组
映射。

## UCM 与 PipeWire

`rootfs/usr/share/alsa/ucm2/` 中的定义由安装脚本复制到目标系统的同名
位置。安装 PipeWire 的音频组件并启用服务：

```bash
sudo pacman -S pipewire pipewire-pulse pipewire-alsa wireplumber
sudo systemctl enable --now loa-smartpa
systemctl --user enable --now pipewire pipewire-pulse wireplumber
```

`wpctl status` 中会出现内置扬声器与四颗模拟麦克风两个设备。

## 验证

```bash
aplay -l
alsaucm -c hw:0 dump text
pw-play stereo.wav
pw-cat --record --raw --format s16 --rate 48000 --channels 4 -n 192000 mic.raw
journalctl -u loa-smartpa -n 20
```

## 已知限制

- 耳机通路已在设备树中接入，实机验证尚未完成。
- 音效与官方系统的差异来自校准数据，后者取自设备自带的 ODM 配置。
- 播放以 2 声道进入 ADSP，由 ADM 上混到四个槽位。
- 功放回采通路已接通，四个槽位的读数一致性尚待验证。
