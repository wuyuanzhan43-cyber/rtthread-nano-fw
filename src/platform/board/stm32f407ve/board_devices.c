/*
 * board_devices.c - STM32F407VE 板级设备注册与装配。
 *
 * 层级：src/platform/board/stm32f407ve
 * 职责：
 *   1. 校验板级接线；
 *   2. 按固定顺序注册 pin、LED、serial、spi、LCD；
 *   3. 绑定控制台串口；
 *   4. 向应用暴露板级设备查询函数。
 *
 * 上下文：board_devices_init 在 rt_hw_board_init 中、调度器启动前调用；
 *         其余查询函数在任务上下文调用。
 * 依赖：board.h、board_config.h、board_pins.h、drv_pin.h、
 *       drv_serial.h、drv_spi.h、stm32f407_console.h、fw/st7735s.h。
 *
 * 注册顺序：
 *   1. board_pins_validate：先拒绝非法端口/AF/保留引脚/重复引脚；
 *   2. pin 设备：LED 和 LCD 控制脚都通过它配置；
 *   3. LED；
 *   4. 串口 + 控制台绑定；
 *   5. SPI；
 *   6. LCD（依赖 spi + pin）。
 *
 * UART 中断不在这里打开，由 board_serial_start_all 统一打开。
 */
#include "board.h"
#include "board_config.h"
#include "board_pins.h"
#include "drv_pin.h"
#include "drv_serial.h"
#include "drv_spi.h"
#include "stm32f407_console.h"
#include "fw/st7735s.h"
#include <stddef.h>

/* 防止重复注册：rt_hw_board_init 只调用一次，但显式保护更安全。 */
static bool devices_ready;

/* LCD 设备对象；Kconfig 关闭 LCD 时不分配，节省 RAM。 */
#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
static st7735s_t lcd;
#endif

#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
/*
 * 根据 Kconfig 选择 LCD 使用的 SPI 设备。
 *
 * BOARD_LCD_SPI_INSTANCE 是 1..3，对应 spi1..spi3。
 * 这里用编译期分支返回设备指针；如果对应的 SPI 没有启用，
 * fw_dev_find 会返回 NULL，后续 board_devices_init 会报 FW_ENOTREADY。
 */
static fw_dev_t *lcd_spi_dev(void)
{
#if BOARD_LCD_SPI_INSTANCE == 1
    return fw_dev_find("spi1");
#elif BOARD_LCD_SPI_INSTANCE == 2
    return fw_dev_find("spi2");
#else
    return fw_dev_find("spi3");
#endif
}

/*
 * 配置 LCD 的四个控制引脚。
 *
 * 全部设为推挽输出，并给一个安全的初始电平：
 *   CS=1（不选中）
 *   DC=1（数据模式，避免上电时被当成命令）
 *   RES=1（不复位）
 *   BL=灭（根据有效电平取反）
 *
 * 背光先关闭，等初始化完成后再由 st7735s_hw_init 打开，
 * 避免上电时闪出随机显存内容。
 */
static fw_status_t lcd_pins_init(fw_dev_t *pin)
{
    int pins[] = {
        FW_PIN_NUMBER(BOARD_LCD_CS_PORT, BOARD_LCD_CS_PIN),
        FW_PIN_NUMBER(BOARD_LCD_DC_PORT, BOARD_LCD_DC_PIN),
        FW_PIN_NUMBER(BOARD_LCD_RES_PORT, BOARD_LCD_RES_PIN),
        FW_PIN_NUMBER(BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN)
    };
    size_t i;

    for (i = 0U; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        int result = fw_pin_configure(pin, pins[i], FW_PIN_MODE_OUTPUT);
        if (result < 0) return (fw_status_t)result;
    }

    (void)fw_pin_write(pin, FW_PIN_NUMBER(BOARD_LCD_CS_PORT, BOARD_LCD_CS_PIN), 1);
    (void)fw_pin_write(pin, FW_PIN_NUMBER(BOARD_LCD_DC_PORT, BOARD_LCD_DC_PIN), 1);
    (void)fw_pin_write(pin, FW_PIN_NUMBER(BOARD_LCD_RES_PORT, BOARD_LCD_RES_PIN), 1);
    (void)fw_pin_write(pin, FW_PIN_NUMBER(BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN),
                       BOARD_LCD_BL_ACTIVE_HIGH ? 0 : 1);
    return FW_OK;
}
#endif

/*
 * 板级设备初始化。
 *
 * 每一步都检查返回值；失败会向上返回，最终由 rt_hw_board_init panic。
 * 不使用“尽量继续”的策略，因为设备缺失会让 main 的行为不可预测。
 */
fw_status_t board_devices_init(void)
{
    fw_status_t result;
    const drv_serial_hw_t *serial_hw;
    const drv_spi_hw_t *spi_hw;
    size_t count;

    if (devices_ready) return FW_OK;

    /* 0. 接线校验：端口/AF/SWD 保留脚/重复引脚。 */
    result = board_pins_validate();
    if (result != FW_OK) return result;

    /* 1. 唯一的 pin 设备：后续所有 GPIO 操作的基础。 */
    result = drv_pin_register();
    if (result != FW_OK) return result;

#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    /*
     * 2. LED 引脚。
     *
     * 引脚号由 Kconfig 的 BOARD_LED_PORT/PIN 生成，
     * active_high 决定“灭”对应的初始电平。
     * 这里只配置模式和初始电平；应用通过 pin 设备控制亮灭。
     */
    {
        fw_dev_t *pin = fw_dev_find("pin");
        int led = FW_PIN_NUMBER(BOARD_LED_PORT, BOARD_LED_PIN);
        if (pin == NULL) return FW_ENOTREADY;
        if (fw_pin_configure(pin, led, FW_PIN_MODE_OUTPUT) < 0)
            return FW_EIO;
        (void)fw_pin_write(pin, led, BOARD_LED_ACTIVE_HIGH ? 0 : 1);
    }
#endif

    /* 3. 串口：按板级描述符注册 serial1..N，并绑定控制台。 */
    serial_hw = board_serial_hw(&count);
    result = drv_serial_register_all(serial_hw, count);
    if (result != FW_OK) return result;
    if (drv_serial_instance(BOARD_CONSOLE_UART_NUM) == NULL)
        return FW_ENOTREADY;
    stm32f407_console_bind(drv_serial_instance(BOARD_CONSOLE_UART_NUM));

    /* 4. SPI：按板级描述符注册 spi1..N。 */
    spi_hw = board_spi_hw(&count);
    result = drv_spi_register_all(spi_hw, count);
    if (result != FW_OK) return result;

#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
    /*
     * 5. LCD：依赖 pin + spi 设备。
     *
     * 先配置控制引脚，再把设备指针、引脚号、分辨率、偏移和
     * 扫描方向打包成 st7735s_config_t，注册为 "lcd"。
     * 真正的面板初始化（复位/命令序列）不在此时执行，
     * 而是在 main 调用 app_boot_display -> fw_display_setup 时执行。
     */
    {
        fw_dev_t *pin = fw_dev_find("pin");
        fw_dev_t *spi = lcd_spi_dev();
        st7735s_config_t config;
        if (pin == NULL || spi == NULL) return FW_ENOTREADY;

        result = lcd_pins_init(pin);
        if (result != FW_OK) return result;

        config.spi = spi;
        config.pin = pin;
        config.cs = FW_PIN_NUMBER(BOARD_LCD_CS_PORT, BOARD_LCD_CS_PIN);
        config.dc = FW_PIN_NUMBER(BOARD_LCD_DC_PORT, BOARD_LCD_DC_PIN);
        config.res = FW_PIN_NUMBER(BOARD_LCD_RES_PORT, BOARD_LCD_RES_PIN);
        config.bl = FW_PIN_NUMBER(BOARD_LCD_BL_PORT, BOARD_LCD_BL_PIN);
        config.bl_active_high = BOARD_LCD_BL_ACTIVE_HIGH != 0;
        config.width = BOARD_LCD_WIDTH;
        config.height = BOARD_LCD_HEIGHT;
        config.x_offset = BOARD_LCD_X_OFFSET;
        config.y_offset = BOARD_LCD_Y_OFFSET;
        config.madctl = (uint8_t)BOARD_LCD_MADCTL;
        config.invert = BOARD_LCD_INVERT != 0;

        result = st7735s_init(&lcd, "lcd", &config);
        if (result != FW_OK) return result;
    }
#endif

    devices_ready = true;
    return FW_OK;
}

/*
 * 打开串口中断。
 *
 * 必须在设备注册完成后调用；main 在 open 串口后调用它。
 * 此时 ring、HAL 句柄和事件对象都已就绪，可以安全接收数据。
 */
fw_status_t board_serial_start_all(void)
{
    if (!devices_ready) return FW_ENOTREADY;
    return drv_serial_start_all();
}

/* 返回控制台设备；设备表未就绪时返回 NULL。 */
fw_dev_t *board_console_dev(void)
{
    return devices_ready ? drv_serial_dev(BOARD_CONSOLE_UART_NUM) : NULL;
}

/* 返回 pin 设备；未初始化时返回 NULL。 */
fw_dev_t *board_pin_dev(void)
{
    return devices_ready ? fw_dev_find("pin") : NULL;
}

bool board_led_enabled(void)
{
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    return true;
#else
    return false;
#endif
}

/* 返回 LED 引脚编号；未启用返回 -1。 */
int board_led_pin(void)
{
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    return FW_PIN_NUMBER(BOARD_LED_PORT, BOARD_LED_PIN);
#else
    return -1;
#endif
}

/* 返回 LED 有效电平。 */
bool board_led_active_high(void)
{
#if defined(BOARD_LED_ENABLE) && BOARD_LED_ENABLE
    return BOARD_LED_ACTIVE_HIGH != 0;
#else
    return false;
#endif
}

/* 返回 LCD 对象；未启用或未初始化时返回 NULL。 */
fw_display_t *board_lcd(void)
{
#if defined(BOARD_LCD_ENABLE) && BOARD_LCD_ENABLE
    return devices_ready ? &lcd.base : NULL;
#else
    return NULL;
#endif
}
