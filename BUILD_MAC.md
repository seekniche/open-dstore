# macOS 编译 Dstore

本文只针对 macOS。编译在 OrbStack/Docker 的 Linux 容器里完成，不修改 Homebrew、系统编译器或其他全局环境。

目录约定：

```text
open-dstore/
├── dstore/       # 当前仓库
├── deps_src/     # Mac 本机目录，依赖源码，自动下载
└── local_libs/   # Mac 本机目录，依赖编译产物，自动生成
```

`deps_src/` 和 `local_libs/` 都在容器外面，也就是 Mac 本机的 `dstore` 同级目录；Docker 只是通过 `-v "$PROJECT_ROOT:/workspace"` 把整个 `open-dstore/` 挂到容器里的 `/workspace`。

下面命令默认在 `dstore` 目录执行：

```bash
cd /path/to/open-dstore/dstore
export PROJECT_ROOT="$(cd .. && pwd)"
```

## 1. 准备 Docker 镜像

该镜像使用 GCC 10.3。Dstore 的编译脚本会在 `local_libs/buildtools/gcc7.3/gcc/bin/` 下查找编译器，后面的依赖准备步骤会把这里的 `gcc`、`g++` 指向容器里的 GCC 10.3。

```bash
docker image inspect dstore-build:gcc10 >/dev/null 2>&1 || \
  docker build -t dstore-build:gcc10 - <<'EOF'
FROM gcc:10.3
ENV DEBIAN_FRONTEND=noninteractive
RUN set -eux; \
    sed -i \
      -e 's|http://deb.debian.org/debian|http://archive.debian.org/debian|g' \
      -e 's|http://security.debian.org/debian-security|http://archive.debian.org/debian-security|g' \
      -e '/buster-updates/d' \
      /etc/apt/sources.list; \
    apt-get -o Acquire::Check-Valid-Until=false update; \
    apt-get install -y --no-install-recommends \
    git ca-certificates curl wget make unzip patch \
    libaio-dev zlib1g-dev; \
    CMAKE_VERSION=3.22.6; \
    CMAKE_ARCH="$(uname -m)"; \
    case "$CMAKE_ARCH" in aarch64|x86_64) ;; *) echo "unsupported arch: $CMAKE_ARCH"; exit 1 ;; esac; \
    wget -q "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${CMAKE_ARCH}.tar.gz"; \
    tar -C /opt -xzf "cmake-${CMAKE_VERSION}-linux-${CMAKE_ARCH}.tar.gz"; \
    ln -sf /opt/cmake-${CMAKE_VERSION}-linux-${CMAKE_ARCH}/bin/* /usr/local/bin/; \
    rm -f "cmake-${CMAKE_VERSION}-linux-${CMAKE_ARCH}.tar.gz"; \
    cmake --version; \
    gcc --version | head -1; \
    rm -rf /var/lib/apt/lists/*
EOF
```

## 2. 下载依赖源码

```bash
mkdir -p "$PROJECT_ROOT/deps_src"

[ -d "$PROJECT_ROOT/deps_src/openGauss-third_party/.git" ] || \
  git clone --depth 1 https://gitcode.com/opengauss/openGauss-third_party.git \
    "$PROJECT_ROOT/deps_src/openGauss-third_party"

[ -d "$PROJECT_ROOT/deps_src/lz4/.git" ] || \
  git clone --depth 1 --branch v1.10.0 https://github.com/lz4/lz4.git \
    "$PROJECT_ROOT/deps_src/lz4"

[ -d "$PROJECT_ROOT/deps_src/googletest/.git" ] || \
  git clone --depth 1 --branch release-1.10.0 https://github.com/google/googletest.git \
    "$PROJECT_ROOT/deps_src/googletest"
```

依赖来源：

- `secure`：`openGauss-third_party/platform/Huawei_Secure_C`
- `cjson`：`openGauss-third_party/dependency/cJSON` 的 `v1.7.17.tar.gz` 和官方补丁
- `lz4`：`v1.10.0`
- `gtest`：`release-1.10.0`

## 3. 生成 local_libs

如果你删掉了 `local_libs/`，直接重新执行本节即可。

```bash
mkdir -p "$PROJECT_ROOT/local_libs"

docker run --rm -i \
  -v "$PROJECT_ROOT:/workspace" \
  -w /workspace \
  dstore-build:gcc10 bash <<'EOF'
set -euo pipefail

LOCAL_LIBS=/workspace/local_libs
DEPS_SRC=/workspace/deps_src
THIRD_PARTY=$DEPS_SRC/openGauss-third_party

mkdir -p "$LOCAL_LIBS/buildtools/gcc7.3/gcc/bin"
ln -sf "$(command -v gcc)" "$LOCAL_LIBS/buildtools/gcc7.3/gcc/bin/gcc"
ln -sf "$(command -v g++)" "$LOCAL_LIBS/buildtools/gcc7.3/gcc/bin/g++"

cd "$THIRD_PARTY/platform/Huawei_Secure_C"
bash build.sh -m all
rm -rf "$LOCAL_LIBS/secure"
mkdir -p "$LOCAL_LIBS/secure/include" "$LOCAL_LIBS/secure/lib"
cp -a "$THIRD_PARTY/output/kernel/platform/Huawei_Secure_C/comm/include/." "$LOCAL_LIBS/secure/include/"
cp -a "$THIRD_PARTY/output/kernel/platform/Huawei_Secure_C/comm/lib/libsecurec.a" "$LOCAL_LIBS/secure/lib/"
cp -a "$THIRD_PARTY/output/kernel/platform/Huawei_Secure_C/Dynamic_Lib/libsecurec.so" "$LOCAL_LIBS/secure/lib/"

CJSON_SRC=$THIRD_PARTY/dependency/cJSON
CJSON_BUILD=$CJSON_SRC/cJSON-1.7.17-dstore-build
rm -rf "$CJSON_BUILD" "$LOCAL_LIBS/cjson"
mkdir -p "$CJSON_BUILD"
tar -zxf "$CJSON_SRC/v1.7.17.tar.gz" -C "$CJSON_BUILD" --strip-components 1
cd "$CJSON_BUILD"
patch -p1 < "$CJSON_SRC/CVE-2024-31755.patch"
patch -p1 < "$CJSON_SRC/issue_IASWHC.patch"
patch -p1 < "$CJSON_SRC/CVE-2023-53154.patch"
patch -p1 < "$CJSON_SRC/CVE-2025-57052.patch"
cmake -S . -B tmp_build \
  -DENABLE_CJSON_UTILS=ON \
  -DENABLE_SAFE_STACK=ON \
  -DCMAKE_PROJECT_INCLUDE="$CJSON_SRC/project_include.cmake" \
  -DCMAKE_INSTALL_PREFIX="$LOCAL_LIBS/cjson"
cmake --build tmp_build --parallel "$(nproc)"
cmake --build tmp_build --target install

rm -rf "$LOCAL_LIBS/lz4"
make -C "$DEPS_SRC/lz4" -j"$(nproc)"
make -C "$DEPS_SRC/lz4" install PREFIX="$LOCAL_LIBS/lz4"

rm -rf "$LOCAL_LIBS/gtest"
rm -rf "$DEPS_SRC/googletest/dstore-build"
cmake -S "$DEPS_SRC/googletest" -B "$DEPS_SRC/googletest/dstore-build" \
  -DCMAKE_C_COMPILER="$(command -v gcc)" \
  -DCMAKE_CXX_COMPILER="$(command -v g++)" \
  -DCMAKE_INSTALL_PREFIX="$LOCAL_LIBS/gtest" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF
cmake --build "$DEPS_SRC/googletest/dstore-build" --parallel "$(nproc)"
cmake --build "$DEPS_SRC/googletest/dstore-build" --target install
EOF
```

## 4. 创建长期容器

如果 `dstore-build` 容器已经被删除，执行下面命令会重新创建。

```bash
docker inspect dstore-build >/dev/null 2>&1 || \
  docker create \
    --name dstore-build \
    -v "$PROJECT_ROOT:/workspace" \
    -w /workspace/dstore \
    dstore-build:gcc10 \
    sleep infinity

docker start dstore-build
```

## 5. 编译

Release：

```bash
docker exec dstore-build \
  bash -lc 'export LOCAL_LIB_PATH=/workspace/local_libs; cd /workspace/dstore/utils && bash build.sh -m release && cd /workspace/dstore && bash build.sh -m release'
```

Debug：

```bash
docker exec dstore-build \
  bash -lc 'export LOCAL_LIB_PATH=/workspace/local_libs; cd /workspace/dstore/utils && bash build.sh -m debug && cd /workspace/dstore && bash build.sh -m debug'
```

成功时会看到：

```text
====== Utils build success ======
====== dstore build success ======
```

主要产物在：

```text
dstore/utils/output/lib/
dstore/output/lib/
dstore/output/bin/
```
