# 实验代码说明

## 实验一

FPC目录是实验一中实现FPC压缩方案的代码。

这里实现了两种方式，采用其中之一都能统计数据类型。

1. `./FPC/FPCompress`目录中的代码，是采用加一个`class`的方式实现。这里即实现了统计，也实现了压缩。
2. `./FPC/nvmain.cpp`文件，只是在`IssueCommand`函数中实现了统计而已。实现这种方式主要是由于在实现第一中方式时遇到了难以理解的错误，于是，多实现了一种来更好地了解代码框架。

`gem5-nvmain-hybrid-simulator`是GEM5和nvmain混合编译的代码。

`nvmain-origin`是原始的nvmain代码。

## 实验二

`quartz`是修改后能满足我电脑环境的代码。
`hashmap`是采用的开源hash代码，已经修改其分配释放内存的方式。不过运行时有些问题，还未解决。
