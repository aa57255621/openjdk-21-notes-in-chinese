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
---
# JDK 21    源代码目录结构
```
- bin - 这个目录包含了构建JDK时产生的可执行文件和一些脚本。这些可执行文件包括用于管理Java运行时环境（如 java、javac、jar 等）的命令行工具。此外，还有一些用于编译源代码、生成文档、运行测试等的工具。
- make - 这个目录包含了Makefile文件和其他用于构建JDK的脚本。这里的工具和脚本使用Makefile语言来定义如何编译和组装JDK的各个组件。开发者通常会使用这些脚本来构建JDK，或者进行其他的构建相关的任务。
- doc - 这个目录包含了JDK的文档资源。这些文档可能包括用户手册、API参考、开发者指南、JEPs（Java Enhancement Proposals）等。这些文档对于理解和使用JDK是非常重要的资源。
- test - 这个目录包含了JDK的测试代码。这些测试用于验证JDK的各种功能和组件是否正常工作。测试代码可能包括单元测试、集成测试、性能测试等。这些测试是确保JDK质量的关键部分。
- src/ - 这是源代码的主要目录。它通常包含以下子目录：
  - java/* - 包含了Java语言相关的代码，比如jni（Java Native Interface）和一些内部API的实现。
  - hotspot/ - 是HotSpot JVM的核心源代码，进一步细分为：
    - os_cpu/ - 包含特定操作系统和CPU架构的适配代码。
    - cpu/ - 这个目录包含了特定操作系统的相关代码。例如，linux 子目录可能包含了针对Linux操作系统的特定实现和调用。这些代码处理诸如进程管理、文件操作、信号处理等操作系统的特定功能。
    - os/ - 这个目录包含了特定CPU架构的相关代码。例如，x86 子目录可能包含了针对x86架构的CPU的特定实现。这些代码处理诸如指令集、处理器特定的优化、缓存管理等硬件相关的功能。
    - share/ - 包含了JVM虚拟机核心的实现，以及jvm相关工具：
      - adlc -（Alt-Death Linking Compiler）是一个用于HotSpot JVM的字节码编译器，它是HotSpot JVM中JIT（Just-In-Time）编译器的一部分。ADLC主要作用是将HotSpot JVM中的解释器（Interpreter）生成的字节码转换为优化的机器代码，以提高Java程序的执行效率。
      - asm - 这是ASM库的源代码，ASM是一个用于操作字节码的框架，它允许程序员以最小的开销生成、分析或转换Java字节码。在HotSpot中，ASM用于生成和修改字节码。
      - C1 - C1是HotSpot JVM中的客户端编译器（Client Compiler），也称为C1编译器。它是JVM内置的即时编译器（JIT），主要用于客户端模式下的JVM。C1编译器专注于快速编译和优化，适用于有限的资源环境。
      - CDS - CDS是Class Data Sharing的缩写，它是一个可选的特性，用于在JVM启动时预先加载和验证所有的类数据。这样可以减少JVM启动时间，因为不需要在运行时动态加载和验证类数据。
      - ci - ci目录包含了HotSpot JVM中的解释器（Interpreter）的源代码。解释器是JVM中用于直接执行字节码的组件，它逐条解释并执行字节码，不进行编译。虽然解释器的执行速度不如JIT编译器，但它可以在不需要编译的情况下立即执行代码。
      - classfile - 这个目录包含了读取和处理Java类文件的代码。类文件是Java程序的字节码文件，包含了Java虚拟机执行的指令。
      - code - 这个目录包含了JVM编译器生成的机器代码。在这里，你可以找到编译器为Java方法生成的本地代码。
      - compiler - 编译器相关的源代码，包括客户端编译器（C1）和服务器编译器（C2）的代码。这里也包含了与编译器架构相关的代码。
      - gc - gc目录包含了垃圾回收器的源代码。垃圾回收器是JVM的一个重要部分，负责自动管理内存，回收不再使用的对象所占用的空间。
      - include - 这个目录包含了编译器和其他组件使用的头文件。头文件包含了宏定义、类型声明和函数原型，用于指导编译过程。
      - interpreter - 解释器的源代码。解释器用于逐条解释和执行Java字节码，而不进行编译。
      - jfr - JFR是Java Flight Recorder的缩写，它是一个用于记录Java应用程序运行时数据的工具。JFR可以捕获应用程序的详细事件数据，并可用于性能分析和问题诊断
      - jvmci - JVM Compiler Interface (JVMCI) 是一个允许在HotSpot JVM中集成第三方编译器的前端接口。它提供了一个通用的接口，使得第三方的编译器（如LLVM）可以更容易地与HotSpot JVM集成。
      - libadt - 抽象数据类型库（Abstract Data Type Library）包含了一系列用于实现各种抽象数据类型的源文件。这些数据类型通常用于JVM内部的算法和数据结构。
      - logging - 日志记录相关的源代码，包括日志记录框架和具体的日志记录器实现。这些代码用于记录JVM的运行信息，方便开发者诊断问题和跟踪性能瓶颈。
      - memory - 内存管理相关的源代码，包括内存分配、垃圾回收策略和内存屏障等。这个目录包含了管理JVM内存空间所需的底层实现。
      - metaprogramming - 元编程相关的源代码，包括用于生成JVM代码的框架和工具。这些工具通常用于自动化生成重复性的代码，或者在编译时执行某些操作。
      - opto - 优化相关的源代码，包括各种优化技术和算法。这些优化旨在提高JVM的运行效率，包括字节码的优化和编译后的机器代码的优化。
      - precompiled - 预编译相关的源代码，包括预编译的本地代码和与预编译代码相关的工具。这些代码通常是为了提高性能而预先编译的JIT代码。
      - prims - 基本操作相关的源代码，包括JVM内部使用的各种基本操作和函数。这些操作是JVM内部实现各种功能的基础。
      - runtime - 运行时相关的源代码，jvm线程相关操作也在这个里面，包括JVM运行时所需的核心类和接口。这些类和接口定义了JVM在运行时的行为和功能。
      - sanitizers - 安全检查相关的源代码，包括各种内存安全检查和数据流跟踪工具。这些工具用于检测JVM中的内存错误和安全漏洞。
      - services - 服务相关的源代码，包括JVM提供的各种服务，如垃圾回收器、JFR（Java Flight Recorder）等。
      - utilities - 实用工具相关的源代码，包括各种帮助工具和辅助类，用于简化开发和维护工作。
```
# Building

```
### 设置编译Java的参数
bash configure --with-target-bits=64 --enable-debug --enable-ccache --with-jvm-variants=server  --disable-warnings-as-errors  2>&1 | tee configure_mac_x64.log

    --with-target-bits=64：这个选项指定了目标系统的位宽，在这里是64位。
    --enable-ccache：这个选项启用CCache，一个快速的编译器缓存，可以加速编译过程。
    --with-jvm-variants=server：这个选项指定了要构建的JVM变体，server表示构建的是服务器版本的JVM，它通常比客户端版本的JVM更加优化 for multi-threaded server applications.
    --with-boot-jdk-jvmargs="-Xlint:deprecation -Xlint:unchecked"：这个选项设置了启动JDK时的JVM参数，-Xlint:deprecation和-Xlint:unchecked会让编译器输出关于弃用API和未检查的类型转换的警告。
    --disable-warnings-as-errors：这个选项禁止将警告当作错误处理，也就是说，即使有警告，编译过程也不会因此而中断。
    --with-debug-level=slowdebug：这个选项指定了调试级别的JVM，slowdebug是OpenJDK中的一种级别，它提供了额外的调试信息，但可能会影响性能。
        --with-debug-level 接受以下几个参数：
            none：不包含任何调试信息，这是默认值。
            client：包含一些基础的调试信息，适用于客户端JVM。
            server：包含更多的调试信息，适用于服务器端JVM。
            extended：包含扩展的调试信息，这些信息对于诊断问题很有帮助，但可能会影响性能。
            full：包含所有可能的调试信息，用于最彻底的调试。
slowdebug：这是一个特殊的调试级别，它提供了额外的调试信息，以牺牲性能为代价。这个级别通常用于开发和调试JVM本身，而不是运行在JVM上的应用程序。
    2>&1：这是一个重定向操作，它将标准错误（2）重定向到标准输出（1），这样所有的输出都会被收集到同一个地方。
    | tee configure_mac_x64.log：|是管道操作符，它将前面的命令的输出传递给后面的命令。tee是一个命令，可以读取标准输入并写入标准输出和指定的文件。在这里，它用来将配置过程中的所有输出保存到名为configure_mac_x64.log的文件中。
```
打包：
sudo make clean
sudo make images 