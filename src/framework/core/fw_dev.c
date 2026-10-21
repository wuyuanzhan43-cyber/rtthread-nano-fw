/*
 * fw_dev.c - 静态 mr 风格设备注册表。
 *
 * 层级：src/framework/core
 * 职责：
 *   1. 维护“设备名 -> fw_dev_t*”的固定容量数组；
 *   2. 对路径做归一化，兼容 "serial1" 与 "/dev/serial1"；
 *   3. 实现引用计数式的 open/close；
 *   4. 把 read/write/ioctl/isr 转发给设备类 ops。
 *
 * 上下文：
 *   - 注册在 rt_hw_board_init()、调度器启动前完成；
 *   - 查找与读写通常在任务上下文；isr 由驱动在中断中调用。
 *
 * 依赖：fw/dev.h、string.h。
 *
 * 并发说明：
 *   设备表在初始化完成后不再修改，因此查找可以无锁进行；
 *   引用计数与设备类内部缓冲区由具体类负责保护。当前应用只有
 *   main 线程使用设备，所以 core 层没有加锁，保持最小实现。
 */
#include "fw/dev.h"
#include <string.h>

/*
 * 设备表本体。
 *
 * devices 只保存基类指针，不拥有对象内存；具体对象由驱动静态定义。
 * device_count 是当前已注册数量，同时也是下一次注册的写入位置。
 * 数组在 .bss 中，占 FW_DEV_MAX * sizeof(pointer)，默认 16*4=64 字节。
 */
static fw_dev_t *devices[FW_DEV_MAX];
static size_t device_count;

/*
 * 把用户传入的路径归一化为短名。
 *
 * 规则：
 *   - 跳过开头的所有 '/'，因此 "/serial1" 与 "serial1" 等价；
 *   - 如果以 "dev/" 开头，再跳过这 4 个字符，因此 "/dev/serial1" 等价；
 *   - 归一化后必须非空且能放进 out（含 '\0'）。
 *
 * 参数：
 *   path     - 输入路径；
 *   out      - 输出缓冲区，容量至少 FW_DEV_NAME_MAX；
 *   capacity - out 的字节容量。
 *
 * 返回：成功 true；参数非法或名字为空/过长 false。
 */
static bool normalize(const char *path, char *out, size_t capacity)
{
    size_t length;
    if (path == NULL || out == NULL || capacity == 0U) return false;

    /* 允许调用者写成 "/serial1"、"//serial1" 等形式。 */
    while (*path == '/') ++path;

    /* 允许 POSIX 风格的 "/dev/serial1"。 */
    if (strncmp(path, "dev/", 4U) == 0) path += 4U;

    length = strlen(path);
    if (length == 0U || length >= capacity) return false;
    memcpy(out, path, length + 1U);
    return true;
}

/*
 * 注册设备。
 *
 * 关键步骤：
 *   1. 归一化名字并检查容量；
 *   2. 拒绝类型为 NONE 的对象；
 *   3. 检查设备表是否已满、名字是否重复；
 *   4. 清零基类对象并写入 name/type/drv；
 *   5. 把指针追加到设备表。
 *
 * 注意：
 *   - 这里只清零 fw_dev_t 基类部分，不会清除具体设备结构体的其他字段；
 *     具体类应在调用本函数之前/之后自行初始化自己的字段；
 *   - ops 先置 NULL，等具体类注册完成后再由类填写。
 */
fw_status_t fw_dev_register(fw_dev_t *dev, const char *name, int type,
                            const fw_drv_t *drv)
{
    char normalized[FW_DEV_NAME_MAX];
    size_t i;

    if (dev == NULL || drv == NULL ||
        !normalize(name, normalized, sizeof(normalized)))
        return FW_EINVAL;
    if (type == FW_DEV_TYPE_NONE) return FW_EINVAL;
    if (device_count >= FW_DEV_MAX) return FW_EOVERFLOW;

    /* 设备名必须唯一：重复注册通常意味着板级初始化被调用了两次。 */
    for (i = 0U; i < device_count; ++i) {
        if (strcmp(devices[i]->name, normalized) == 0) return FW_EEXIST;
    }

    /* 只初始化基类部分，具体类字段由调用者管理。 */
    memset(dev, 0, sizeof(*dev));
    memcpy(dev->name, normalized, strlen(normalized) + 1U);
    dev->type = type;
    dev->ref_count = 0U;
    dev->ops = NULL;
    dev->drv = drv;

    devices[device_count++] = dev;
    return FW_OK;
}

/*
 * 按名字查找设备。
 *
 * 支持 "serial1" 与 "/dev/serial1"。线性扫描在 16 个设备以内足够快，
 * 且不需要哈希表/动态内存；初始化完成后设备表只读，因此无需加锁。
 */
fw_dev_t *fw_dev_find(const char *name)
{
    char normalized[FW_DEV_NAME_MAX];
    size_t i;
    if (!normalize(name, normalized, sizeof(normalized))) return NULL;
    for (i = 0U; i < device_count; ++i) {
        if (strcmp(devices[i]->name, normalized) == 0) return devices[i];
    }
    return NULL;
}

/* 返回已注册设备数量。用于启动日志和 Host 测试断言。 */
size_t fw_dev_count(void)
{
    return device_count;
}

/*
 * 遍历设备表。
 * 遍历期间不允许注册新设备，否则 device_count 变化会导致漏项或越界。
 */
void fw_dev_walk(void (*visit)(fw_dev_t *dev, void *arg), void *arg)
{
    size_t i;
    if (visit == NULL) return;
    for (i = 0U; i < device_count; ++i) visit(devices[i], arg);
}

/*
 * 清空设备表。
 *
 * 仅用于 Host 单元测试在用例之间复位全局状态；固件运行期调用会让
 * 所有已注册设备“消失”，因此不要在板上调用。
 * 这里只清指针，不清具体设备对象，便于测试重新注册同一批静态对象。
 */
void fw_dev_registry_reset(void)
{
    size_t i;
    for (i = 0U; i < device_count; ++i) devices[i] = NULL;
    device_count = 0U;
}

/* 判断设备类型，供各类的 fw_xxx_find() 做二次校验。 */
bool fw_dev_is_type(const fw_dev_t *dev, int type)
{
    return dev != NULL && dev->type == type;
}

/*
 * 打开设备（引用计数语义）。
 *
 * 第一次打开（ref_count == 0）才调用类的 open：
 *   - serial 会清空 ring 并重新配置波特率；
 *   - pin/spi 没有 open，直接增加计数。
 *
 * 如果类 open 失败，ref_count 不变，调用者可以安全重试。
 * oflags 目前不参与判断，保留给未来做访问权限控制。
 */
fw_status_t fw_dev_open(fw_dev_t *dev, int oflags)
{
    fw_status_t result;
    (void)oflags;
    if (dev == NULL || dev->ops == NULL) return FW_EINVAL;

    if (dev->ref_count == 0U && dev->ops->open != NULL) {
        result = dev->ops->open(dev);
        if (result != FW_OK) return result;
    }
    ++dev->ref_count;
    return FW_OK;
}

/*
 * 关闭设备（引用计数语义）。
 *
 * 只有计数减到 0 时才调用类的 close。
 * 多余的 close（计数已经为 0）返回 FW_EINVAL，便于尽早发现调用错误。
 */
fw_status_t fw_dev_close(fw_dev_t *dev)
{
    fw_status_t result = FW_OK;
    if (dev == NULL || dev->ops == NULL || dev->ref_count == 0U)
        return FW_EINVAL;

    --dev->ref_count;
    if (dev->ref_count == 0U && dev->ops->close != NULL) {
        result = dev->ops->close(dev);
    }
    return result;
}

/*
 * 统一读入口。
 *
 * core 只做参数防御：
 *   - dev/ops/read/buf 必须有效；
 *   - size==0 直接返回 0，避免无意义地进入驱动。
 * 真正的阻塞策略、超时和错误转换由设备类负责。
 */
int fw_dev_read(fw_dev_t *dev, int off, void *buf, size_t size,
                uint32_t timeout_ms)
{
    if (dev == NULL || dev->ops == NULL || dev->ops->read == NULL ||
        buf == NULL) return FW_EINVAL;
    if (size == 0U) return 0;
    return dev->ops->read(dev, off, buf, size, timeout_ms);
}

/*
 * 统一写入口。
 * 返回值可能是短写计数；调用者需要比较返回值与 size，决定是否重试。
 */
int fw_dev_write(fw_dev_t *dev, int off, const void *buf, size_t size,
                 uint32_t timeout_ms)
{
    if (dev == NULL || dev->ops == NULL || dev->ops->write == NULL ||
        buf == NULL) return FW_EINVAL;
    if (size == 0U) return 0;
    return dev->ops->write(dev, off, buf, size, timeout_ms);
}

/*
 * 统一控制入口。
 * 设备类未实现 ioctl 时返回 FW_ENOTSUP，调用者可以据此回退到默认行为。
 */
int fw_dev_ioctl(fw_dev_t *dev, int off, int cmd, void *args)
{
    if (dev == NULL || dev->ops == NULL || dev->ops->ioctl == NULL)
        return FW_ENOTSUP;
    return dev->ops->ioctl(dev, off, cmd, args);
}

/*
 * 统一中断入口。
 * 只允许驱动在 ISR 中调用；类实现必须非阻塞。
 */
int fw_dev_isr(fw_dev_t *dev, int event, void *args)
{
    if (dev == NULL || dev->ops == NULL || dev->ops->isr == NULL)
        return FW_ENOTSUP;
    return dev->ops->isr(dev, event, args);
}
