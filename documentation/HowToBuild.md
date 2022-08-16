
# How to Build by yourself:

## Install required software

- Git - [Download](https://git-scm.com/downloads) for Windows, on Linux/Mac install via package manager (`brew`, `apt`, ...)

For development:
- Git
- Python3
- VSCode

## Clone the Repository

You should clone with 
```shell
$ git clone --recursive https://github.com/RogueMaster/flipperzero-firmware-wPlugins.git
```

# Build on Linux/macOS

Check out [documentation/fbt.md](documentation/fbt.md) for details on building and flashing firmware. 

### Compile everything for development

```sh./fbt
```

### Compile everything for release + get updater package to update from microSD card

```sh
./fbt COMPACT=1 DEBUG=0 updater_package
```

Check `dist/` for build outputs.

Use **`flipper-z-{target}-full-{suffix}.dfu`** to flash your device.

If compilation fails, make sure all submodules are all initialized. Either clone with `--recursive` or use `git submodule update --init --recursive`.

# Build on Linux/macOS

Check out [documentation/fbt.md](documentation/fbt.md) for details on building and flashing firmware. 

### Compile everything for development

```sh
./fbt
```

### Compile everything for release + get updater package to update from microSD card

```sh
./fbt --with-updater COMPACT=1 DEBUG=0 updater_package
```

Check `dist/` for build outputs.

Use **`flipper-z-{target}-full-{suffix}.dfu`** to flash your device.

## Build with Docker

### Prerequisites

1. Install [Docker Engine and Docker Compose](https://www.docker.com/get-started)
2. Prepare the container:

 ```sh
 docker-compose up -d
 ```

### Compile everything for development

```sh
docker-compose exec dev ./fbt
```

### Compile everything for release + get updater package to update from microSD card

```sh
docker-compose exec dev ./fbt COMPACT=1 DEBUG=0 updater_package
```

Check `dist/` for build outputs.

Use **`flipper-z-{target}-full-{suffix}.dfu`** to flash your device.

If compilation fails, make sure all submodules are all initialized. Either clone with `--recursive` or use `git submodule update --init --recursive`.
