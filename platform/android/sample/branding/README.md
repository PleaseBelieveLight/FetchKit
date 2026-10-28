# 拾流 · Android 演示品牌

中文名：**拾流**，读作 Shí Liú。「拾」是收集与获得，「流」是网络数据流，寓意将数据可靠地收集到本地。
桌面名称与应用内标题统一为「拾流」，副标题使用「FetchKit 下载演示」。

图标将弯曲的数据流融入向下箭头，由下方托盘承接，表达下载与落盘。保持粗线条和简洁轮廓，适合小尺寸显示。

| 用途 | 色值 |
|------|------|
| 深青背景 | `#153E43` |
| 米白流线与箭头 | `#F5FAF7` |
| 浅玉色托盘 | `#79D7BD` |

## 资源

- [shiliu-icon.svg](shiliu-icon.svg)：最终品牌图标的可编辑矢量源文件。
- [shiliu-icon.png](shiliu-icon.png)：与最终矢量路径一致的 1024 × 1024 透明圆角预览。
- [shiliu-concept.png](shiliu-concept.png)：使用内置 image_gen 工具生成的透明概念原图。
- [imagegen-prompt.txt](imagegen-prompt.txt)：生成概念原图时使用的完整提示词。

最终图标以概念原图为参考重新绘制为平面矢量，清理边缘并调整箭头与托盘间距；App 使用矢量资源。
如调整造型，保持 SVG、PNG、`ic_shiliu_mark.xml` 与 `ic_launcher_foreground.xml` 的路径一致。

## Android 适配

- `drawable/ic_shiliu_mark.xml`：72 × 72 视口，用于 Compose 页头和 Android 7.x 的启动器图标。
- `drawable/ic_launcher_foreground.xml`：108 × 108 自适应前景；图形缩放至中心安全区域，留出遮罩与动效空间。
- `drawable/ic_launcher_background.xml`：完整深青背景，交由启动器选择圆形或其他遮罩。
- `mipmap-anydpi/`：Android 7.x 的圆角方形与圆形图标。
- `mipmap-anydpi-v26/`：Android 8.0 及以上自适应图标；Android 13 主题图标复用前景透明度生成单色轮廓。

![拾流图标](shiliu-icon.png)
