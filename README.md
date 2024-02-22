# 从官方社区下载的openjdk 21，本地打包编译没有问题，学习jvm时添加中文笔记注释、笔记，以便理解

# Welcome to the JDK!

For build instructions please see the
[online documentation](https://openjdk.org/groups/build/doc/building.html),
or either of these files:

- [doc/building.html](doc/building.html) (html version)
- [doc/building.md](doc/building.md) (markdown version)

See <https://openjdk.org/> for more information about the OpenJDK
Community and the JDK and see <https://bugs.openjdk.org> for JDK issue
tracking.

# Building 
bash configure --with-target-bits=64 --enable-ccache --with-jvm-variants=server  --with-boot-jdk-jvmargs="-Xlint:deprecation -Xlint:unchecked" --disable-warnings-as-errors --with-debug-level=slowdebug 2>&1 | tee configure_mac_x64.log
bash configure   --with-target-bits=64  --disable-warnings-as-errors --enable-debug --enable-dtrac

make images 