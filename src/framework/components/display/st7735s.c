/*
 * st7735s.c - ST7735S 面板驱动的具体实现。
 *
 * 层级：src/framework/components/display
 * 职责：
 *   1. 4 线 SPI 下的命令/数据切换；
 *   2. 硬件复位与上电初始化命令序列；
 *   3. 设置显示窗口并把 RGB565 像素写入面板；
 *   4. 背光控制。
 *
 * 上下文：任务上下文，允许阻塞在 SPI 传输上。
 * 依赖：fw/st7735s.h、fw/clock.h。
 *
 * 命令/数据协议：
 *   CS 拉低选中面板；
 *   DC=0 时发送的是命令字节；
 *   DC=1 时发送的是数据字节；
 *   一次传输结束后 CS 拉高。
 *
 * 所有 SPI 写都是单向发送（tx != NULL, rx == NULL），
 * 面板不需要读回数据，因此使用 fw_spi_transfer 的“只发”模式。
 */
#include "fw/st7735s.h"
#include "fw/clock.h"

/* ST7735S 常用命令字；完整命令表见面板数据手册。 */
#define ST7735_CMD_SWRESET 0x01U /* 软件复位 */
#define ST7735_CMD_SLPOUT  0x11U /* 退出睡眠 */
#define ST7735_CMD_NORON   0x13U /* 正常显示模式 */
#define ST7735_CMD_INVOFF  0x20U /* 关闭反色 */
#define ST7735_CMD_INVON   0x21U /* 开启反色 */
#define ST7735_CMD_DISPON  0x29U /* 打开显示 */
#define ST7735_CMD_CASET   0x2AU /* 列地址范围 */
#define ST7735_CMD_RASET   0x2BU /* 行地址范围 */
#define ST7735_CMD_RAMWR   0x2CU /* 写显存 */
#define ST7735_CMD_MADCTL  0x36U /* 扫描方向/颜色顺序 */
#define ST7735_CMD_COLMOD  0x3AU /* 像素格式 */

/* 单次 SPI 传输超时；12MHz 下写一小块像素远小于 100ms。 */
#define ST7735_SPI_TIMEOUT_MS 100U

/*
 * 写一个控制引脚。
 *
 * 参数 number 为负表示该引脚未接（例如没有背光控制），
 * 此时直接返回成功，让上层代码不需要到处判断“有没有背光”。
 */
static fw_status_t pin_set(st7735s_t *lcd, int number, bool level)
{
    int result;
    if (number < 0) return FW_OK;
    result = fw_pin_write(lcd->pin, number, level ? 1 : 0);
    return result < 0 ? (fw_status_t)result : FW_OK;
}

/*
 * 通过 SPI 发送一段数据。
 * 只发不收；返回 FW_OK 或底层错误码。
 */
static fw_status_t spi_tx(st7735s_t *lcd, const uint8_t *data, size_t length,
                          uint32_t timeout_ms)
{
    if (lcd == NULL || lcd->spi == NULL || data == NULL || length == 0U)
        return FW_EINVAL;
    {
        int result = fw_spi_transfer(lcd->spi, data, NULL, length, timeout_ms);
        return result < 0 ? (fw_status_t)result : FW_OK;
    }
}

/*
 * 发送一条“只有命令、没有参数”的命令。
 *
 * 时序：
 *   1. CS 拉低；
 *   2. DC=0；
 *   3. 发送命令字节；
 *   4. CS 拉高。
 *
 * 无论中间是否失败，最后都会尝试释放 CS，避免总线一直被占用；
 * 返回值是第一个错误（如果有）。
 */
static fw_status_t write_cmd(st7735s_t *lcd, uint8_t command,
                             uint32_t timeout_ms)
{
    fw_status_t result;
    result = pin_set(lcd, lcd->cs, false);
    if (result != FW_OK) return result;
    result = pin_set(lcd, lcd->dc, false);
    if (result == FW_OK) result = spi_tx(lcd, &command, 1U, timeout_ms);
    (void)pin_set(lcd, lcd->cs, true);
    return result;
}

/*
 * 发送“命令 + 参数数据”。
 *
 * 与 write_cmd 的区别：发送完命令后把 DC 拉高，再发送 data。
 * 如果 length==0，则只发送命令。
 */
static fw_status_t write_cmd_data(st7735s_t *lcd, uint8_t command,
                                  const uint8_t *data, size_t length,
                                  uint32_t timeout_ms)
{
    fw_status_t result;
    result = pin_set(lcd, lcd->cs, false);
    if (result != FW_OK) return result;

    /* 先发命令字节（DC=0）。 */
    result = pin_set(lcd, lcd->dc, false);
    if (result == FW_OK) result = spi_tx(lcd, &command, 1U, timeout_ms);

    /* 再发参数字节（DC=1）。 */
    if (result == FW_OK && length != 0U) {
        result = pin_set(lcd, lcd->dc, true);
        if (result == FW_OK) result = spi_tx(lcd, data, length, timeout_ms);
    }

    (void)pin_set(lcd, lcd->cs, true);
    return result;
}

/*
 * 设置显示窗口。
 *
 * CASET 写入列起止地址，RASET 写入行起止地址；
 * 两者都是 16 位大端（高字节在前）。
 *
 * x_offset/y_offset 用于补偿不同面板的显存偏移；
 * 调用者传入的是逻辑坐标，这里加上偏移再发给控制器。
 */
static fw_status_t set_window(st7735s_t *lcd, uint16_t x0, uint16_t y0,
                              uint16_t x1, uint16_t y1, uint32_t timeout_ms)
{
    uint8_t data[4];
    fw_status_t result;
    uint16_t xs = (uint16_t)(x0 + lcd->x_offset);
    uint16_t xe = (uint16_t)(x1 + lcd->x_offset);
    uint16_t ys = (uint16_t)(y0 + lcd->y_offset);
    uint16_t ye = (uint16_t)(y1 + lcd->y_offset);

    /* CASET: XS[15:8], XS[7:0], XE[15:8], XE[7:0] */
    data[0] = (uint8_t)(xs >> 8U);
    data[1] = (uint8_t)xs;
    data[2] = (uint8_t)(xe >> 8U);
    data[3] = (uint8_t)xe;
    result = write_cmd_data(lcd, ST7735_CMD_CASET, data, 4U, timeout_ms);
    if (result != FW_OK) return result;

    /* RASET: YS[15:8], YS[7:0], YE[15:8], YE[7:0] */
    data[0] = (uint8_t)(ys >> 8U);
    data[1] = (uint8_t)ys;
    data[2] = (uint8_t)(ye >> 8U);
    data[3] = (uint8_t)ye;
    return write_cmd_data(lcd, ST7735_CMD_RASET, data, 4U, timeout_ms);
}

/*
 * 硬件复位。
 *
 * RES 低有效：
 *   - 先拉高保持稳定；
 *   - 拉低至少 10us（这里给 10ms，远大于手册要求）；
 *   - 拉高后等待 120ms，让内部电源和振荡器稳定。
 *
 * 复位期间 CS 保持高（未选中），避免把复位脉冲当成数据。
 */
static fw_status_t hardware_reset(st7735s_t *lcd)
{
    fw_status_t result;
    result = (fw_status_t)pin_set(lcd, lcd->cs, true);
    if (result != FW_OK) return result;
    result = pin_set(lcd, lcd->res, true);
    if (result != FW_OK) return result;
    fw_delay_ms(10U);
    result = pin_set(lcd, lcd->res, false);
    if (result != FW_OK) return result;
    fw_delay_ms(20U);
    result = pin_set(lcd, lcd->res, true);
    if (result != FW_OK) return result;
    fw_delay_ms(120U);
    return FW_OK;
}

/*
 * 上电初始化命令序列。
 *
 * 顺序基于 ST7735S 常见 128x160 模块的厂商示例：
 *   1. 软件复位、退出睡眠；
 *   2. 帧率/电源/电压/伽马等模拟参数；
 *   3. 反色设置；
 *   4. MADCTL 扫描方向；
 *   5. COLMOD=0x05（16 位 RGB565）；
 *   6. 正常显示、打开显示。
 *
 * 这些常量是面板模拟特性，通常不需要按应用修改；
 * 若更换面板批次出现颜色/方向异常，优先检查这里和 board Kconfig。
 */
static fw_status_t init_sequence(st7735s_t *lcd)
{
    static const uint8_t frmctr1[] = {0x01U, 0x2CU, 0x2DU};
    static const uint8_t frmctr2[] = {0x01U, 0x2CU, 0x2DU};
    static const uint8_t frmctr3[] = {0x01U, 0x2CU, 0x2DU, 0x01U, 0x2CU, 0x2DU};
    static const uint8_t invctr[] = {0x07U};
    static const uint8_t pwctr1[] = {0xA2U, 0x02U, 0x84U};
    static const uint8_t pwctr2[] = {0xC5U};
    static const uint8_t pwctr3[] = {0x0AU, 0x00U};
    static const uint8_t pwctr4[] = {0x8AU, 0x2AU};
    static const uint8_t pwctr5[] = {0x8AU, 0xEEU};
    static const uint8_t vmctr1[] = {0x0EU};
    static const uint8_t colmod[] = {0x05U};
    static const uint8_t gmctrp1[] = {
        0x02U, 0x1CU, 0x07U, 0x12U, 0x37U, 0x32U, 0x29U, 0x2DU,
        0x29U, 0x25U, 0x2BU, 0x39U, 0x00U, 0x01U, 0x03U, 0x10U
    };
    static const uint8_t gmctrn1[] = {
        0x03U, 0x1DU, 0x07U, 0x06U, 0x2EU, 0x2CU, 0x29U, 0x2DU,
        0x2EU, 0x2EU, 0x37U, 0x3FU, 0x00U, 0x00U, 0x02U, 0x10U
    };
    uint8_t direction[1];
    const uint32_t t = ST7735_SPI_TIMEOUT_MS;
    fw_status_t result;

    /* 软件复位后等待内部状态稳定。 */
    result = write_cmd(lcd, ST7735_CMD_SWRESET, t);
    if (result != FW_OK) return result;
    fw_delay_ms(150U);

    /* 退出睡眠模式。 */
    result = write_cmd(lcd, ST7735_CMD_SLPOUT, t);
    if (result != FW_OK) return result;
    fw_delay_ms(120U);

    /* 帧率控制、电源控制、VCOM 等模拟参数。 */
    if ((result = write_cmd_data(lcd, 0xB1U, frmctr1, sizeof(frmctr1), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xB2U, frmctr2, sizeof(frmctr2), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xB3U, frmctr3, sizeof(frmctr3), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xB4U, invctr, sizeof(invctr), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xC0U, pwctr1, sizeof(pwctr1), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xC1U, pwctr2, sizeof(pwctr2), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xC2U, pwctr3, sizeof(pwctr3), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xC3U, pwctr4, sizeof(pwctr4), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xC4U, pwctr5, sizeof(pwctr5), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xC5U, vmctr1, sizeof(vmctr1), t)) != FW_OK) return result;

    /* 根据板级配置决定是否反色。 */
    if (lcd->invert) {
        if ((result = write_cmd(lcd, ST7735_CMD_INVON, t)) != FW_OK) return result;
    } else {
        if ((result = write_cmd(lcd, ST7735_CMD_INVOFF, t)) != FW_OK) return result;
    }

    /* 扫描方向：MADCTL 决定行列递增方向和 RGB/BGR 顺序。 */
    direction[0] = lcd->madctl;
    if ((result = write_cmd_data(lcd, ST7735_CMD_MADCTL, direction, 1U, t)) != FW_OK) return result;
    /* 像素格式：0x05 = 16 位/像素（RGB565）。 */
    if ((result = write_cmd_data(lcd, ST7735_CMD_COLMOD, colmod, sizeof(colmod), t)) != FW_OK) return result;
    /* 正/负伽马曲线。 */
    if ((result = write_cmd_data(lcd, 0xE0U, gmctrp1, sizeof(gmctrp1), t)) != FW_OK) return result;
    if ((result = write_cmd_data(lcd, 0xE1U, gmctrn1, sizeof(gmctrn1), t)) != FW_OK) return result;
    /* 正常显示模式并打开显示。 */
    if ((result = write_cmd(lcd, ST7735_CMD_NORON, t)) != FW_OK) return result;
    if ((result = write_cmd(lcd, ST7735_CMD_DISPON, t)) != FW_OK) return result;

    fw_delay_ms(100U);
    return FW_OK;
}

/*
 * display 类的 init 回调（由 fw_display_setup 调用）。
 *
 * 步骤：
 *   1. 校验设备与引脚；
 *   2. 硬件复位；
 *   3. 发送初始化命令；
 *   4. 标记 ready，清屏为黑色；
 *   5. 如果有背光控制，打开背光。
 *
 * 先清屏再开背光，避免上电时闪出随机显存内容。
 * 如果背光驱动返回 FW_ENOTSUP（没有实现），按成功处理。
 */
static fw_status_t st7735s_hw_init(void *ctx)
{
    st7735s_t *lcd = (st7735s_t *)ctx;
    fw_status_t result;
    if (lcd == NULL || lcd->spi == NULL || lcd->pin == NULL ||
        lcd->cs < 0 || lcd->dc < 0 || lcd->res < 0) return FW_EINVAL;
    lcd->ready = false;
    result = hardware_reset(lcd);
    if (result != FW_OK) return result;
    result = init_sequence(lcd);
    if (result != FW_OK) return result;
    lcd->ready = true;
    result = fw_display_fill(&lcd->base, 0x0000U, ST7735_SPI_TIMEOUT_MS);
    if (result != FW_OK) return result;
    if (lcd->bl >= 0) {
        result = fw_display_set_backlight(&lcd->base, true);
        if (result == FW_ENOTSUP) result = FW_OK;
    }
    return result;
}

/*
 * 开始一次窗口写入。
 *
 * 该类把“设置窗口 + 发 RAMWR + 切到数据模式”合并到 begin_write，
 * 后续 write_pixels 只需要连续发送像素，不再重复命令。
 *
 * 状态机：
 *   - ready 必须为 true；
 *   - writing 必须为 false（禁止嵌套）；
 *   - 设置窗口后拉低 CS、发 RAMWR、把 DC 拉高；
 *   - 成功后 writing=true，直到 end_write 释放 CS。
 *
 * 如果中途失败，立即释放 CS 并保持 writing=false。
 */
static fw_status_t st7735s_begin_write(void *ctx, uint16_t x, uint16_t y,
                                       uint16_t width, uint16_t height,
                                       uint32_t timeout_ms)
{
    st7735s_t *lcd = (st7735s_t *)ctx;
    const uint8_t ramwr = ST7735_CMD_RAMWR;
    fw_status_t result;

    if (lcd == NULL || !lcd->ready || lcd->writing) return FW_EBUSY;
    if (width == 0U || height == 0U) return FW_EINVAL;

    result = set_window(lcd, x, y, (uint16_t)(x + width - 1U),
                        (uint16_t)(y + height - 1U), timeout_ms);
    if (result != FW_OK) return result;

    /* 选中面板，发送“写显存”命令，然后把 DC 切到数据模式。 */
    result = pin_set(lcd, lcd->cs, false);
    if (result != FW_OK) return result;
    result = pin_set(lcd, lcd->dc, false);
    if (result == FW_OK) result = spi_tx(lcd, &ramwr, 1U, timeout_ms);
    if (result == FW_OK) result = pin_set(lcd, lcd->dc, true);
    if (result != FW_OK) {
        (void)pin_set(lcd, lcd->cs, true);
        return result;
    }
    lcd->writing = true;
    return FW_OK;
}

/*
 * 写入像素。
 *
 * 面板期望大端 RGB565（高字节先发），而内存中的 uint16_t
 * 在 Cortex-M 上是小端，因此这里逐像素拆成两个字节再发送。
 *
 * 使用 64 字节栈缓冲（32 像素）分批发送：
 *   - 避免一次大传输占用过长；
 *   - 避免为整行/整屏分配缓冲；
 *   - 每批发送后 offset 前进，失败时立即停止。
 */
static fw_status_t st7735s_write_pixels(void *ctx, const uint16_t *pixels,
                                        size_t count, uint32_t timeout_ms)
{
    st7735s_t *lcd = (st7735s_t *)ctx;
    uint8_t buffer[64];
    fw_status_t result = FW_OK;
    size_t offset = 0U;

    if (lcd == NULL || !lcd->writing || pixels == NULL) return FW_EINVAL;

    while (offset < count && result == FW_OK) {
        size_t chunk = count - offset;
        size_t i;
        if (chunk > sizeof(buffer) / 2U) chunk = sizeof(buffer) / 2U;
        for (i = 0U; i < chunk; ++i) {
            uint16_t color = pixels[offset + i];
            buffer[i * 2U] = (uint8_t)(color >> 8U);      /* 高字节先发 */
            buffer[i * 2U + 1U] = (uint8_t)color;         /* 低字节后发 */
        }
        result = spi_tx(lcd, buffer, chunk * 2U, timeout_ms);
        offset += chunk;
    }
    return result;
}

/*
 * 结束窗口写入。
 *
 * 释放 CS，并把 writing 复位。
 * 即使调用者没有写完整个窗口，也允许正常结束；
 * 面板会在下一次 RAMWR 时重新设置窗口。
 */
static fw_status_t st7735s_end_write(void *ctx)
{
    st7735s_t *lcd = (st7735s_t *)ctx;
    if (lcd == NULL) return FW_EINVAL;
    lcd->writing = false;
    return pin_set(lcd, lcd->cs, true);
}

/*
 * 背光控制。
 *
 * on 是逻辑状态，需要根据 bl_active_high 转成实际电平：
 *   高有效：on -> 1
 *   低有效：on -> 0
 * 没有背光控制引脚时返回 FW_ENOTSUP。
 */
static fw_status_t st7735s_set_backlight(void *ctx, bool on)
{
    st7735s_t *lcd = (st7735s_t *)ctx;
    int result;
    if (lcd == NULL) return FW_EINVAL;
    if (lcd->bl < 0) return FW_ENOTSUP;
    result = fw_pin_write(lcd->pin, lcd->bl,
                          (on == lcd->bl_active_high) ? 1 : 0);
    return result < 0 ? (fw_status_t)result : FW_OK;
}

/* display 类 ops：把面板实现挂到通用显示接口上。 */
static const fw_display_ops_t st7735s_ops = {
    st7735s_hw_init,
    st7735s_begin_write,
    st7735s_write_pixels,
    st7735s_end_write,
    st7735s_set_backlight
};

/*
 * 注册面板。
 *
 * 这里只保存配置并注册设备，不发送任何 SPI 命令：
 *   - 注册发生在 rt_hw_board_init 中，此时调度器/延时可能不可用；
 *   - 真正的初始化由 main 调用 fw_display_setup 完成。
 *
 * 校验 CS/DC/RES 必须有效（>=0），BL 允许为负（表示不控制背光）。
 */
fw_status_t st7735s_init(st7735s_t *lcd, const char *name,
                         const st7735s_config_t *config)
{
    fw_display_info_t info;
    if (lcd == NULL || name == NULL || config == NULL || config->spi == NULL ||
        config->pin == NULL || config->cs < 0 || config->dc < 0 ||
        config->res < 0 || config->width == 0U || config->height == 0U)
        return FW_EINVAL;

    /* 保存配置副本，后续所有操作都只依赖 lcd 自身。 */
    lcd->spi = config->spi;
    lcd->pin = config->pin;
    lcd->cs = config->cs;
    lcd->dc = config->dc;
    lcd->res = config->res;
    lcd->bl = config->bl;
    lcd->bl_active_high = config->bl_active_high;
    lcd->x_offset = config->x_offset;
    lcd->y_offset = config->y_offset;
    lcd->madctl = config->madctl;
    lcd->invert = config->invert;
    lcd->ready = false;
    lcd->writing = false;

    info.width = config->width;
    info.height = config->height;
    info.bits_per_pixel = 16U;
    return fw_display_register(&lcd->base, name, &st7735s_ops, lcd, &info);
}
