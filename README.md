**Dstore是一个可独立编译，独立测试的数据库存储引擎组件。**

# macOS 编译说明

如果在 macOS 上编译 Dstore，建议使用 OrbStack/Docker 编译环境，并参考 [Dstore macOS 本机编译说明](BUILD_MAC.md)。

该说明仅针对 macOS 本机编译场景，包含从零准备同级 `local_libs/` 依赖目录、创建 Docker 镜像/长期容器、执行 release/debug 编译等步骤；其他系统请继续参考下方通用编译流程。


# 一、环境配置

如下所示为编译Dstore所需依赖库及建议版本：

```
XXX/
├── dstore/ -------------------- # 项目入口
└── local_libs/ ---------------- # 依赖库目录
    ├── buildtools/ ------------ # 编译工具
    │   └── gcc7.3/ ------------ # 建议版本：v7.3
    │       ├── gcc/
    │       ├── gmp/
    │       ├── isl/
    │       ├── mpc/
    │       └── mpfr/
    ├── secure/ ---------------- # 建议版本：v3.0.9
    │   ├── include/
    │   └── lib/
    ├── lz4/ ------------------- # 建议版本：v1.10.0
    │   ├── include/
    │   └── lib/
    ├── cjson/ ----------------- # 建议版本：v1.7.17
    │   ├── include/
    │   └── lib/
    └── gtest/ ----------------- # 建议版本：v1.10.0
        ├── include/
        └── lib/
```

## 1.1 配置Dstore所需依赖库

请按照上述指定的依赖库版本进行下载和编译，以避免因版本不一致而引发的兼容性问题。

### secure
**下载地址**：https://gitcode.com/opengauss/openGauss-third_party/tree/master/platform/Huawei_Secure_C
**编译命令**：`./build.sh -m all`

### cjson
**下载地址**：https://gitcode.com/opengauss/openGauss-third_party/tree/master/dependency/cJSON
**编译命令**：`./build.sh -m all`

### lz4
**下载地址**：https://github.com/lz4/lz4/releases
**编译命令**：`make -j$(nproc) && make install PREFIX=xxx/output`

### gtest
**下载地址**：https://github.com/google/googletest/releases
**编译命令**：
```
mkdir output && cd output
cmake \
-DCMAKE_INSTALL_PREFIX=xxx/output \
-DCMAKE_BUILD_TYPE=Debug \
-DBUILD_SHARED_LIBS=OFF \
-DCMAKE_CXX_COMPILER=g++ \
-DCMAKE_C_COMPILER=gcc \
..
make -j$(nproc) && make install
```

上述依赖库编译完成后，请按库名称调整目录结构。例如，在`local_libs/secure`目录下，应包含该库对应的include和lib子目录。


## 1.2 更新并加载环境配置文件
修改`dstore/buildenv`文件中`BUILD_ROOT`变量的值为当前项目所在目录，如：`BUILD_ROOT=/opt/project/dstore`
执行：`source dstore/buildenv`


# 二、项目编译
## 2.1 编译前置utils模块
`cd dstore/utils`
`bash build.sh -m release / debug`
确认utils/output/lib/路径下有`libgsutils.so`生成

## 2.2 编译dstore
`cd dstore`
`bash build.sh -m release / debug`
编译成功后在dstore/output/lib/路径下有`libdstore.so`与`libdstore.a`生成


# 三、TPCC测试
## 3.1 更新并加载环境配置文件
同1.2

## 3.2 编译运行tpcctest
```
cd dstore
rm -rf tmp_build
mkdir tmp_build && cd tmp_build
cmake .. -DUTILS_PATH=dstore/utils/output
make run_dstore_tpcctest
```
