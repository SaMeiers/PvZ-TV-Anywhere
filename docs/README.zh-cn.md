<div align="center">

# PvZ TV Anywhere

**[English](./README.md)** | **简体中文**

[![license](https://img.shields.io/github/license/SaMeiers/PvZ-TV-Anywhere)][GPL-3.0]
[![Android CI](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/android.yml/badge.svg)](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/android.yml "Android CI")
[![Desktop CI](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/desktop.yml/badge.svg)](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/desktop.yml "Desktop CI")

一个基于植物大战僵尸 TV 版的改版, 并让它能在原本无法运行的地方游玩: 纯 64 位手机与 PC.

</div>

## 关于本项目

本项目 fork 自 ZombieYetis 的
[PlantsVsZombies-AndroidTV](https://github.com/ZombieYetis/PlantsVsZombies-AndroidTV),
模组本身以及 `app/` 下的内容均出自该项目. 游戏本体是 32 位 ARM 库,
而较新的核心 (Cortex-A715/X4 及以后) 已完全不再执行 32 位代码,
因此本 fork 增加了一个 *runner*: 把原始库映射进 guest 地址空间,
并在 Dynarmic 的 ARM32 JIT 上执行. 同一个 runner 也让游戏有了桌面版.

上游不打算合入该 runner, 因此它留在这里. 全部代码仍为 GPL-3.0;
工作原理详见[架构说明](./ARCHITECTURE.zh-cn.md).

## 构建

- 确保已安装下列组件:
    * Android SDK Platform 34
    * NDK v27.3.13750724 (r27d)
    * CMake v3.20+

- **连同子模块一起**克隆仓库 (子模块包含 dynarmic, SDL, zlib 和 glad).
    ```sh
    git clone --recursive https://github.com/SaMeiers/PvZ-TV-Anywhere.git
    cd PlantsVsZombies-AndroidTV
    ```
    > 如果克隆时忘了加 `--recursive`, 执行 `git submodule update --init --recursive`.
    > 若想使用本地已有的依赖副本, 配置时加上 `-DPVZTV_DEPS_DIR=<目录>`
    > (详见 [`third_party/CMakeLists.txt`](/third_party/CMakeLists.txt)).

- 复制 assets 文件到路径 `PlantsVsZombies-AndroidTV/app/src/main/assets/` 下.
    > 需要资源文件请联系仓库作者.

- 构建方式:
    * Android Studio: 点击构建按钮.
    * 命令行: 运行以下命令:
        ```sh
        ./gradlew assembleDebugV115
        ```

- 如果要发布, 先在位于项目根目录的 `keystore.properties` 文件 (需要自行创建) 中配置签名. 文件内容样式如下:
    ```properties
    storePassword=myStorePassword
    keyPassword=mykeyPassword
    keyAlias=myKeyAlias
    storeFile=myStoreFileLocation
    ```

## 桌面版

arm64 构建依靠 runner 执行游戏原本的 32 位 ARM 库, 同一份 runner 也能在 PC 上运行它们,
只是把 Android 系统库换成了 SDL2.

需要 CMake 3.20+, 支持 C++20 的编译器, 以及带 `jinja2` 的 Python
(glad 在配置阶段生成 GL 加载器: `pip install jinja2`).

```sh
cmake -S desktop -B build/desktop
cmake --build build/desktop --config Release
```

播放器会在自身可执行文件所在目录查找一切, 因此按下面的结构放好后直接运行即可:

```
pvztv_player(.exe)
assets/                 游戏资源
libGameMain.so          guest 库, 放在同级目录或 libs/ 下
libHomura.so            必需: 触控 UI 由该模组实现
libGameRegister.so
libnative_code.so
libfmodex.so
data/                   首次运行时创建: 存档与设置
```

命令行传入路径 (`./pvztv_player other/libGameMain.so`) 可覆盖该查找.
加载了什么以及为什么这样做, 详见[架构说明](./ARCHITECTURE.zh-cn.md).

## 参与贡献

### 修改 runner

`runner/` 由 Android 应用与 PC 播放器共用, 因此 `runner/src/platform/` 之外的改动会同时影响两端.
提交前请两边都构建一次:

```sh
./gradlew assembleV115Release                      # Android (arm64 + armeabi-v7a)
cmake -S desktop -B build/desktop && cmake --build build/desktop --config Release
```

guest 库如何加载, 以及它们的调用如何到达宿主, 详见[架构说明](./ARCHITECTURE.zh-cn.md).


### 编码风格 (C++)

#### 命名约定

- 函数/类型/概念: `PascalCase`
- 变量: `camelCase`
- 命名空间: `snake_case`
- 宏/常量/枚举成员/非类型模板参数: `UPPER_CASE`

#### 格式

见 [`.clang-format`](/.clang-format).

> 建议在每次提交前先用 IDE 对代码进行格式化.

### 提交

参考[约定式提交](https://www.conventionalcommits.org/zh-hans/v1.0.0/).

## 许可协议

本项目的源代码使用 [GPL-3.0][GPL-3.0] 许可进行授权.

本项目与渡维科技、宝开或艺电无关, 也未获得他们的认可.

[GPL-3.0]: https://www.gnu.org/licenses/gpl-3.0.html "GPL-3.0"
