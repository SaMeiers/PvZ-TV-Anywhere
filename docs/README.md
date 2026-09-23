<div align="center">

# PvZ TV Anywhere

**English** | **[简体中文](./README.zh-cn.md)**

[![license](https://img.shields.io/github/license/SaMeiers/PvZ-TV-Anywhere)][GPL-3.0]
[![Android CI](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/android.yml/badge.svg)](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/android.yml "Android CI")
[![Desktop CI](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/desktop.yml/badge.svg)](https://github.com/SaMeiers/PvZ-TV-Anywhere/actions/workflows/desktop.yml "Desktop CI")

A mod of _Plants vs. Zombies_ Android TV version, playable beyond the devices it
was built for: on 64-bit-only phones and on PC.

</div>

## About this project

This is a fork of [PlantsVsZombies-AndroidTV](https://github.com/ZombieYetis/PlantsVsZombies-AndroidTV)
by ZombieYetis, whose work is the mod itself and everything under `app/`. The
game ships as 32-bit ARM libraries, and newer cores (Cortex-A715/X4 and up) no
longer run 32-bit code at all, so this fork adds a *runner*: the original
libraries are mapped into a guest address space and executed on a Dynarmic
ARM32 JIT. The same runner gives the game a desktop build.

Upstream declined to take the runner, so it lives here. Everything remains
GPL-3.0; see [Architecture](./ARCHITECTURE.md) for how it works.

## Build

- Ensure the following is installed:
    * Android SDK Platform 34
    * NDK v27.3.13750724 (r27d)
    * CMake v3.20+

- Clone the repository **with its submodules** (they carry dynarmic, SDL, zlib and glad).
    ```sh
    git clone --recursive https://github.com/SaMeiers/PvZ-TV-Anywhere.git
    cd PlantsVsZombies-AndroidTV
    ```
    > Already cloned without `--recursive`? Run `git submodule update --init --recursive`.
    > To build against dependency copies you already have on disk, configure with
    > `-DPVZTV_DEPS_DIR=<dir>` (see [`third_party/CMakeLists.txt`](/third_party/CMakeLists.txt)).

- Copy assets files to the path `PlantsVsZombies-AndroidTV/app/src/main/assets/`.
    > If you need resource files, please contact the repository author.

- Build with:
    * Android Studio: Click on the build button.
    * Command line: Run the following command:
        ```sh
        ./gradlew assembleDebugV115
        ```

- If release, configure signing using the file `keystore.properties` located in the project root directory (you must
    create this file yourself). The file content format is as follows:
    ```properties
    storePassword=myStorePassword
    keyPassword=mykeyPassword
    keyAlias=myKeyAlias
    storeFile=myStoreFileLocation
    ```

## Desktop player

The same runner that lets the arm64 build execute the game's original 32-bit ARM
libraries also runs them on a PC, through SDL2 instead of the Android system
libraries.

Requires CMake 3.20+, a C++20 compiler and Python with `jinja2` (glad generates
its GL loader at configure time: `pip install jinja2`).

```sh
cmake -S desktop -B build/desktop
cmake --build build/desktop --config Release
```

The player looks for everything next to its own executable, so put it in a
folder like this and just run it:

```
pvztv_player(.exe)
assets/                 the game's assets
libGameMain.so          the guest libraries, loose or in libs/
libHomura.so            required: the mod implements the touch UI
libGameRegister.so
libnative_code.so
libfmodex.so
data/                   created on first run: saves and settings
```

A path passed on the command line (`./pvztv_player other/libGameMain.so`)
overrides the lookup. See [Architecture](./ARCHITECTURE.md) for what gets loaded
and why.

## Contributing

### Working on the runner

`runner/` is shared by the Android app and the PC player, so anything you change
outside `runner/src/platform/` affects both. Build both before sending a change:

```sh
./gradlew assembleV115Release                      # Android (arm64 + armeabi-v7a)
cmake -S desktop -B build/desktop && cmake --build build/desktop --config Release
```

[Architecture](./ARCHITECTURE.md) explains how the guest libraries are loaded and
how their calls reach the host.


### Coding Style (C++)

#### Name Convention

- Functions / types / concepts: `PascalCase`
- Variables: `camelCase`
- Namespaces: `snake_case`
- Macros / constants / enumerators / non-type template parameters: `UPPER_CASE`

#### Format

See [`.clang-format`](/.clang-format).

> It is recommended to format the code using the IDE before each commit.

### Commit

Refer to [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/).

## License

The source code for this project is licensed under the [GPL-3.0][GPL-3.0] license.

This project is not associated with or endorsed by Transmension, PopCap or Electronic Arts.

[GPL-3.0]: https://www.gnu.org/licenses/gpl-3.0.html "GPL-3.0"
