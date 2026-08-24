- yingmusic-svc-mlx增加功能而不是改，因为其还要被audiokit等其他app所使用；
- 需要实现一个build.py脚本，在构建的时候你需要运行的是`python build.py` (不允许带任何shell变量、环境变量、cd、参数等)，然后DAW就应该可以找到安装好的插件。

将更新后的plan写入 docs/ara.md文件。不是让你进入plan mode。