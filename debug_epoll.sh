#!/bin/bash
# debug_epoll.sh - 快速调试 epoll 用户态中断支持

echo "=== 开始调试 epoll 用户态中断支持 ==="

# 1. 挂载 debugfs
if ! mountpoint -q /sys/kernel/debug; then
    echo "挂载 debugfs..."
    sudo mount -t debugfs none /sys/kernel/debug
fi

# 2. 启用 pr_debug
echo "启用 eventpoll pr_debug 日志..."
sudo sh -c 'echo -n "file fs/eventpoll.c +p" > /sys/kernel/debug/dynamic_debug/control'
ORIGINAL_LOGLEVEL=$(cat /proc/sys/kernel/printk | awk '{print $1}')
sudo sh -c 'echo 8 > /proc/sys/kernel/printk'

# 3. 清除旧日志
sudo dmesg -c

# 4. 编译测试程序
echo "编译测试程序..."
gcc test_epoll_uintr.c -o test_epoll_uintr -lpthread -muintr

if [ $? -ne 0 ]; then
    echo "编译失败！"
    exit 1
fi

# 5. 运行测试程序
echo "运行测试程序..."
./test_epoll_uintr

# 6. 显示日志
echo ""
echo "=== epoll 调试日志 ==="
sudo dmesg | grep -i epoll

# 恢复原始日志级别
sudo sh -c "echo $ORIGINAL_LOGLEVEL > /proc/sys/kernel/printk"
# 禁用 pr_debug 日志
sudo sh -c 'echo -n "file fs/eventpoll.c -p" > /sys/kernel/debug/dynamic_debug/control'