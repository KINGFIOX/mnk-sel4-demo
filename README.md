# mnk-sel4-demo

seL4 上的用户态线程系统：**5 个用户程序 > 4 个内核线程（3 worker + 1 rootserver 调度器）>
2 个程序地址空间 > 2 个 vCPU**。用户程序是 `user/` 下可自定义的 **PIC 模块**
（编译成 .so，零绝对地址），死循环里调 `mnk_yield()` 让出 worker、用
`mnk_mutex_*()` 抢全局锁——抢不到就 yield。program1..3 的家在地址空间 A，
program4..5 在 B；任何程序可被任何 worker 执行，任何 worker 可进任何空间。
rootserver 是唯一的调度器：yield = 对带 badge 的 endpoint 发 Call
（seL4 里「自定义系统调用」的正统形态）。

## 架构

```
   user/program1..5.c —— 死循环：lock → global_counter++ → unlock → spin → yield
        │  include/mnk_user.h 就是全部用户可见 API（yield / mutex / shared / stats）
        │  程序文本零绝对地址；调用 API 走 PLT→GOT，GOT 由装载器填运行时地址
        ▼
   运行时（src/payload.c，每空间一份独立链接镜像 A@0x40000000 / B@0x60000000）
   ─ mnk_yield：记 trace → 切回 worker 引擎 → sched_call 问调度器要下一个 G
   ─ mnk_mutex：ldaxr/stxr 抢锁，抢不到就 mnk_yield；stlr 释放
        │                     ▲ Reply(下一个G)
        ▼                     │
   ═══════════ rootserver（第 4 个内核线程，唯一调度器 + 装载器）═══════════
        Recv(sched_ep, badge=worker号)；轮转派 G，预算 ASSIGN_BUDGET=500 次：
        · home == worker 当前空间 → seL4_Reply(下一个G)          （纯 IPC）
        · home ≠  当前空间 → Suspend + SetSpace + WriteRegisters  （跨空间重定向）
        · 预算耗尽           → Suspend(worker) 收工
   启动时装载：两份运行时 ELF + 五份 PIC .so 程序（任意基址，处理
   R_AARCH64_JUMP_SLOT/GLOB_DAT/RELATIVE 重定位，把 API 地址写进各程序 GOT）
```

两级调度：rootserver 把 5 个程序复用到 3 个 worker；seL4 把 4 个内核线程调到
2 个 vCPU 真并行（亲和 a@cpu0、b/c@cpu1，`DebugDumpScheduler` 的 Core 列即证据）。

## 目录

```
mnk-sel4-demo/
├── include/mnk_user.h  用户程序 API：mnk_yield / mnk_mutex_{try,un}lock / mnk_shared / mnk_stats
├── user/program1..5.c  用户程序（可自定义）：死循环 + yield + mutex，-fPIC 编译
├── src/
│   ├── main.c          rootserver：ELF/.so 装载器+动态重定位、调度循环、统计
│   ├── payload.c       运行时：API 实现 + worker 执行器（freestanding，链两次）
│   ├── asm.S           ctx_switch + sched_call（约 25 行）
│   ├── mnk.h           内部契约（池/manifest/API 表）
│   ├── payload.ld      运行时链接脚本（IMAGE_BASE 等参数化）
│   └── CMakeLists.txt  构建描述（rt_a/rt_b + prog1..5.so + blob 嵌入）
├── kernel/             seL4 内核树（cmake-tool 约定：必须在根目录）
├── tools/seL4          seL4_tools（cmake-tool）
├── projects/           musllibc, sel4runtime, seL4_libs, util_libs
└── build/              产物（prog*.so / rt_*.elf 可 readelf 对照）
```

## 依赖

- 交叉工具链 `gcc-aarch64-linux-gnu`（含 objcopy）
- `cmake`、`ninja-build`、`device-tree-compiler`、`libxml2-utils`、`qemu-system-arm`
- python3 及 seL4 构建脚本所需的包（jinja2 / ply / pyfdt / sortedcontainers /
  pyyaml / pyelftools / jsonschema / lxml 等；apt 装 `python3-*`，pyfdt 用
  `pip install --user`）。内核构建脚本硬编码调用 PATH 里的 `python3`，
  **构建命令须加 `env PATH=/usr/bin:/bin` 前缀**保证取到系统解释器

## 构建与运行

```bash
env PATH=/usr/bin:/bin cmake -G Ninja -B build \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DKernelMaxNumNodes=2 -DQEMU_GIC_VERSION=3 \
  -DKernelPlatform=qemu-arm-virt -DKernelSel4Arch=aarch64 \
  -DKernelArmHypervisorSupport=ON \
  -DKernelVerificationBuild=OFF -DKernelDebugBuild=ON -DKernelPrinting=ON \
  -DKernelArmExportPCNTUser=ON \
  -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/kernel/gcc.cmake \
  .
env PATH=/usr/bin:/bin ninja -C build

timeout 60 qemu-system-aarch64 -machine virt,virtualization=on,secure=off,gic-version=3 \
  -cpu cortex-a53 -smp 2 -nographic -m 1024 \
  -kernel build/images/mnk-sel4-demo-image-arm-qemu-arm-virt
```

构建流：payload.c/asm.S 编一次、链两次成 rt_a/rt_b（`--defsym` 注入各自的
IMAGE_BASE/POOL_VA/MARK_VA/TRACE_VA/CSTACK_VA/GSTACK_VA）；user/programN.c 各自
`-fPIC` 编译、`-shared -e program_main` 链成 progN.so；全部 objcopy 成 blob
链进 rootserver，运行时由 rootserver 装载并重定位。

## 机制

1. **用户程序可重定位**：.so 的文本只有 adrp/adr 相对寻址；对外部 API 的调用
   走 PLT→GOT。rootserver 装载器解析 PT_DYNAMIC（DT_SYMTAB/STRTAB/RELA/JMPREL），
   按符号名把运行时 API 地址写进 GOT 槽（R_AARCH64_JUMP_SLOT/GLOB_DAT），
   R_AARCH64_RELATIVE 写 base+addend。程序装载在任意基址
   （0x48000000 / 0x48123000 / 0x48456000 / 0x68000000 / 0x68123000）。
2. **yield 语义**：程序调 `mnk_yield()` → 运行时记 trace（ldaxr/stlxr 原子取号）、
   yields++ → ctx_switch 切回 worker 引擎（纯用户态）→ `sched_call`（raw svc，
   Call）→ rootserver 决定下一个 G 并 Reply 或重定向。对程序而言这就是阻塞式
   yield 系统调用：返回时可能已在别的 worker、别的空间。
3. **mutex**：`mnk_mutex_lock` = ldaxr/stxr 抢 0→1，失败即 `mnk_yield()` 让出
   worker（不空转烧 CPU）；`unlock` = stlr 写 0。锁与 global_counter 住在全局池
   （一帧三址：rootserver@0x30000000 A@0x42000000 B@0x62000000），跨地址空间争用。
4. **运行时 manifest 自述**：每份运行时镜像首字是 `struct image_manifest`
   （布局 VA + api[] 六个函数地址表），两份镜像 VA 无一处相同。
5. **worker 身份 = TPIDR_EL0**（=IPC buffer VA，per-TCB 常量跨空间不变）；
   运行时靠它 + `workers[w].current_gid` 定位当前程序上下文。
6. **重定向三连**：跨空间时 Suspend（取消 Call 阻塞）→ SetSpace →
   WriteRegisters(pc=landing, sp)，pc/sp 恰为 UserContext 前两词。
7. **优先级让路**：resume 后 rootserver 自降 90（worker 100）；SMP 亲和
   a@cpu0、b/c@cpu1。fault_endpoint 全指向 rootserver 但正常流程零触发
   ——纯页表执法。

## 实测证据（2 vCPU）

```
=== program & mutex stats ===
  program1 (home A): yields=100 locks=100
  program2 (home A): yields=100 locks=99    ← 100 次 yield 中有 1 次是抢锁失败
  program3 (home A): yields=100 locks=100      的让出（SMP 下真实争用过锁）
  program4 (home B): yields=100 locks=100
  program5 (home B): yields=100 locks=100
  sum(locks)=499  global_counter=499  ->  mutex atomicity holds

  yields total=500（=ASSIGN_BUDGET）per program: 各 100
  home==observed violations: 0
  program1..5 每一个都 ran on: worker-a worker-b worker-c
  调度器账本 assignments a=232/b=133/c=135 与 trace 实测逐位一致
  cross-space redirects: 341；stray faults: 0
```

mutex 的原子性断言：**Σ 每程序 locks_taken == global_counter**——任何丢失
更新或双重进入都会打破等式；program2 的 99/100 差值正是「抢不到锁就 yield」
路径被执行的直接证据。

## 已知边界

- 协作式调度：程序只在 yield/锁等待点交出 worker（无抢占/超时）
- 每次让出一条调度 IPC + 跨空间三次 invocation；语义规整优先于开销
- 全局池是两空间的共享窗口，隔离是页表级而非信息级
