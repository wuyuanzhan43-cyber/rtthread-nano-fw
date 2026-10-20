/*
 * fw/ring.c - 固定容量字节 FIFO 的实现。
 *
 * 层级：src/framework/core
 * 职责：在调用者提供的静态存储区上实现无分配、无锁的环形缓冲。
 * 上下文：无内部锁；并发访问必须由调用者串行化。
 * 依赖：fw/ring.h。
 *
 * 实现约定：
 *   - head 指向下一个写入位置；
 *   - tail 指向下一个读取位置；
 *   - count 记录有效字节数；
 *   - 索引到达 capacity 时回到 0，使用取模而不是位掩码，
 *     因此容量可以是任意正整数（不需要是 2 的幂）。
 */
#include "fw/ring.h"

/*
 * 初始化。
 *
 * 只记录存储区和容量，然后调用 clear 复位索引。
 * storage 由调用者负责生命周期：通常是驱动的静态数组，
 * 也可能是 Host 测试中的栈/静态缓冲区。
 */
fw_status_t fw_ring_init(fw_ring_t *ring, uint8_t *storage, size_t capacity)
{
    if (ring == NULL || storage == NULL || capacity == 0U) return FW_EINVAL;
    ring->storage = storage;
    ring->capacity = capacity;
    fw_ring_clear(ring);
    return FW_OK;
}

/*
 * 清空 ring。
 *
 * 只重置 head/tail/count，不写 storage：
 * 旧数据仍然留在数组里，但 count==0 使得后续 read 不会碰它们。
 * 这样清空操作是 O(1)，不会在中断临界区里产生大块 memset。
 */
void fw_ring_clear(fw_ring_t *ring)
{
    if (ring != NULL) {
        ring->head = 0U;
        ring->tail = 0U;
        ring->count = 0U;
    }
}

/*
 * 写入数据。
 *
 * 循环条件同时检查 done < length 和 count < capacity：
 *   - 写满后立即停止并返回短计数；
 *   - 不覆盖未读数据，保证读取侧看到的是完整字节流。
 *
 * 返回短计数而不是错误码，是因为“ring 满”在串口发送/接收路径里
 * 是正常背压，调用者可以稍后重试；是否当作溢出由上层决定。
 */
size_t fw_ring_write(fw_ring_t *ring, const uint8_t *data, size_t length)
{
    size_t done = 0U;
    if (ring == NULL || data == NULL || ring->storage == NULL) return 0U;

    /* 到达容量即停止并返回短计数；由调用者决定这是背压、溢出还是重试。 */
    while (done < length && ring->count < ring->capacity) {
        ring->storage[ring->head] = data[done++];
        ring->head = (ring->head + 1U) % ring->capacity;
        ++ring->count;
    }
    return done;
}

/*
 * 读取数据。
 *
 * 只要 count 不为 0 就继续拷贝，最多拷贝调用者要求的 capacity 字节。
 * 读取后 tail 前移、count 减少，空出的位置可以被写入侧复用。
 */
size_t fw_ring_read(fw_ring_t *ring, uint8_t *data, size_t capacity)
{
    size_t done = 0U;
    if (ring == NULL || data == NULL || ring->storage == NULL) return 0U;

    while (done < capacity && ring->count != 0U) {
        data[done++] = ring->storage[ring->tail];
        ring->tail = (ring->tail + 1U) % ring->capacity;
        --ring->count;
    }
    return done;
}

/*
 * 只读预览。
 *
 * 从 tail 开始按逻辑顺序复制，但不修改任何索引：
 *   - 适合“先看数据，确认发送完成后再消费”的场景；
 *   - 调用者必须保证在 peek 和实际消费之间，数据没有被其他上下文改写。
 *
 * 注意循环有两个上界：done < capacity（调用者缓冲）和
 * done < ring->count（有效数据量）。
 */
size_t fw_ring_peek(const fw_ring_t *ring, uint8_t *data, size_t capacity)
{
    size_t done = 0U;
    size_t index;
    if (ring == NULL || data == NULL || ring->storage == NULL) return 0U;

    /* 从 tail 开始遍历但不修改索引：DMA/IT 发送器在完成回调消费之前，
     * 必须保持被 peek 的 chunk 稳定。 */
    index = ring->tail;
    while (done < capacity && done < ring->count) {
        data[done++] = ring->storage[index];
        index = (index + 1U) % ring->capacity;
    }
    return done;
}
