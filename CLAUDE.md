# ESP-IDF 项目指南

## 用户偏好
- **回复语言**: 中文
- **风格**: 简洁直接，不啰嗦
- **执行方式**: 命令一条一条执行，严禁批量执行
- **工具限制**: 仅使用终端/命令行工具，不使用编辑器
- **技术栈**: ESP-IDF (C/C++), ESP32-S3

## 常用 IDF 命令
- **构建项目**: `idf.py build`
- **烧录程序**: `idf.py flash` (需指定端口，如 `idf.py -p /dev/tty.usbserial-xxx flash`)
- **打开串口监听**: `idf.py monitor`
- **清理工程**: `idf.py fullclean`
- **配置菜单**: `idf.py menuconfig`

## 状态管理
- **每次对话开始时，必须先读取以下文件了解重构进度**：
  - `task_plan.md`
  - `progress.md`
  - `findings.md`
- 记录关键决策和调试发现到 `findings.md`。
- 每次对话结束时，必须更新上述规划文件。
