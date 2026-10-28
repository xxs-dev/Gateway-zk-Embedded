# AArch64 共享内存 Robust 互斥量门禁

## 结论

`MemoryPointStore` 依赖 Linux `PTHREAD_PROCESS_SHARED` 和 `PTHREAD_MUTEX_ROBUST`。
验证环境必须能够在持锁进程异常退出后向下一持锁者返回 `EOWNERDEAD`。

2026-09-15 在 `192.168.22.11` 的 QEMU user-mode 2.11.1 中观察到：

- 普通共享内存读写和持久化 ring 边界通过；
- 父、子进程的 `set_robust_list` 系统调用均返回 `ENOSYS`；
- 子进程持锁退出后，父进程的 `pthread_mutex_timedlock` 返回 `ETIMEDOUT`。

这是该 QEMU 运行时不支持 robust-list 的验证环境限制，不是产品通过证据，也不能据此豁免产品生命周期测试。

## 两层门禁

1. `posix_robust_mutex_probe`：只验证内核、libc 和执行环境能否完成 process-shared robust mutex 的 owner-death 恢复。
2. `memory_point_store_robust_owner_death`：验证产品在收到 `EOWNERDEAD` 后调用 `pthread_mutex_consistent`，并丢弃可能只写入一半的共享载荷。

本地原生 Linux 可执行：

```bash
ctest --test-dir build --output-on-failure \
  -R '^(posix_robust_mutex_probe|memory_point_store_robust_owner_death|memory_point_store_lifecycle)$'
```

原生 x86_64 通过只能作为开发回归，不能替代 AArch64 发布门禁。QEMU 若探针失败，探针以非零状态退出并输出 `status=ENVIRONMENT_UNSUPPORTED`；产品结论必须记录为 `NOT_PROVEN` 并保持发布阻断。不得将该结果配置成 CTest skip、expected failure 或自动转为 `PASS`。

## AArch64 发布要求

发布候选必须在总协调和实施测试明确指定的 Allwinner T536 AArch64 原生测试机上，对同一 commit 交叉编译得到的测试二进制执行。当前协调指定后续在 `192.168.22.16` 完成该门禁，但边端源码任务不得自行连接、部署或修改该设备：

```bash
./posix_robust_mutex_probe
./memory_point_store_lifecycle_test --robust-owner-death-only
./memory_point_store_lifecycle_test
```

三项都必须退出 `0`。测试需使用私有 IPC/mount namespace 和独立 `/dev/shm`，只上传测试二进制到事务临时目录，不安装候选包、不替换运行二进制、不停止服务，不得连接串口、CAN、MQTT、现场设备或复用生产共享内存段。证据必须记录 commit、测试二进制 SHA256、内核、glibc、机器型号、每项退出码和原始日志。

没有可用的专用 T536 测试机时，此门禁保持阻断；不得改用生产设备规避。
