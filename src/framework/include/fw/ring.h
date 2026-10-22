/*
 * fw/ring.h - 固定容量字节 FIFO。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 为串口 RX/TX 路径提供无动态分配的有界缓冲；
 *   2. 为 Host 假设备提供同样的存储语义，保证测试与固件行为一致。
 *
 * 上下文：
 *   本模块没有内部锁。任务与 ISR 同时访问同一个 ring 时，
 *   调用者必须提供互斥（本项目由 serial 类通过驱动的 lock/unlock
 *   关闭中断）。仅把字段声明为 volatile 不能替代串行化：
 *   head/tail/count 三者必须作为一个整体保持一致。
 *
 * 依赖：fw/status.h、stddef.h、stdint.h。
 *
 * 容量说明：
 *   capacity 不要求是 2 的幂，索引统一用取模运算。
 *   代价是一次除法/取模，但 STM32F407 上串口数据量不大，
 *   换来的是任意容量都能精确匹配 Kconfig 配置。
 */
#ifndef FW_RING_H
#define FW_RING_H

#include <stddef.h>
#include <stdint.h>
#include "fw/status.h"

/*
 * ring 控制块。
 *
 * storage 指向调用者的静态数组，ring 本身不分配内存；
 * head 指向下一个写入位置，tail 指向下一个读取位置；
 * count 是当前有效字节数，用它区分“满”和“空”——
 * 如果只看 head==tail，无法区分这两种状态。
 */
typedef struct {
    uint8_t *storage;  /* 调用者拥有；生命周期必须覆盖 ring 本身 */
    size_t capacity;   /* 字节容量；初始化后必须大于 0；不要求 2 的幂 */
    size_t head;       /* 下一个写入位置，按 capacity 取模 */
    size_t tail;       /* 下一个读取位置，按 capacity 取模 */
    size_t count;      /* 当前有效字节数，范围 0..capacity */
} fw_ring_t;

/*
 * 初始化 ring。
 * storage 必须非空、capacity 必须大于 0，否则返回 FW_EINVAL。
 * 初始化会把 head/tail/count 全部清零，但不会清空 storage 内容。
 */
fw_status_t fw_ring_init(fw_ring_t *ring, uint8_t *storage, size_t capacity);

/* 丢弃所有未读数据，仅重置索引和计数，不擦除底层数组。 */
void fw_ring_clear(fw_ring_t *ring);

/*
 * 写入最多 length 字节。
 * 返回实际写入数量；返回值小于 length 表示 ring 已满（短写）。
 * 该函数不会覆盖未读数据，也不会阻塞。
 */
size_t fw_ring_write(fw_ring_t *ring, const uint8_t *data, size_t length);

/*
 * 读取最多 capacity 字节。
 * 返回实际读取数量；不会读取超过 count 的数据。
 */
size_t fw_ring_read(fw_ring_t *ring, uint8_t *data, size_t capacity);

/*
 * 丢弃最多 length 字节（只移动 tail/count，不拷贝数据、不清 storage）。
 * 返回实际丢弃数量。DMA TX 完成回调只需要“消费已发送块”，
 * 不需要把数据再读出来，因此用它比 fw_ring_read 更直接。
 */
size_t fw_ring_discard(fw_ring_t *ring, size_t length);

/*
 * 查看数据但不消费。
 * 从 tail 开始复制最多 capacity 字节，head/tail/count 保持不变。
 * 典型用途：DMA/IT 发送器在真正发送完成前保持数据稳定。
 */
size_t fw_ring_peek(const fw_ring_t *ring, uint8_t *data, size_t capacity);

#endif
